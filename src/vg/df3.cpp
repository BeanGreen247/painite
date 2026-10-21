#include "df3.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace pn::vg::v3 {

// ======================================================================================================================
// RandomState
// ======================================================================================================================

RandomState3::RandomState3(const Registry& r, int64_t s, bool legacy_random) : reg(r), seed(s), legacy(legacy_random) {
    if (legacy_random) {
        LegacyRandom lr(s);
        random_ = lr.fork_positional();
    } else {
        Xoroshiro x(s);
        random_ = x.fork_positional();
    }
}

NoiseParams RandomState3::noise_params(const std::string& name) const {
    const Json& j = reg.get("noise", name);
    NoiseParams p;
    p.base_amplitude = j.has("base_amplitude") ? j.at("base_amplitude").number() : 1.0;
    p.base_octave = int(j.at("base_octave").int_value());
    p.octave_count = int(j.get_int("octave_count", 1));
    if (const Json* n = j.find("normalize")) p.normalize = n->is_bool() ? (n->b ? NoiseParams::ENABLED : NoiseParams::DISABLED) : NoiseParams::LEGACY;
    if (const Json* a = j.find("amplitude_modifiers")) for (auto& e : a->arr) p.amplitude_modifiers.push_back(e.number());
    return p;
}

std::unique_ptr<Rng> RandomState3::random_from_hash(const std::string& id) { return random_->from_hash_of(id); }

std::shared_ptr<Noise> RandomState3::noise(const std::string& name_in) {
    std::string name = name_in.rfind("minecraft:", 0) == 0 ? name_in : "minecraft:" + name_in;
    std::lock_guard<std::mutex> l(mu_);
    auto it = noises_.find(name);
    if (it != noises_.end()) return it->second;
    NoiseParams p = noise_params(name);
    std::shared_ptr<Noise> n;
    if (name == "minecraft:nether/temperature" || name == "minecraft:nether/vegetation") {
        LegacyRandom lr(seed + (name == "minecraft:nether/temperature" ? 0 : 1));
        n = create_legacy_nether_noise(p, lr);
    } else {
        auto r = random_->from_hash_of(name);
        n = create_normal_noise(p, *r);
    }
    noises_[name] = n;
    return n;
}

PosFactory& RandomState3::random_factory(const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    auto& slot = factories_[name];
    if (!slot) {
        auto r = random_->from_hash_of(name);
        slot = r->fork_positional();
    }
    return *slot;
}

// ======================================================================================================================
// The function tree
// ======================================================================================================================

namespace {

enum Axis { AX = 1, AY = 2, AZ = 4, ALL = 7 };

enum class K {
    Constant, Ref, Noise, ShiftA, ShiftB, Shift, EndIslands, DistToPoint, Gradient, Unary, Round, Binary, Pow, Spline, Lerp, Clamp, RangeChoice, IntervalSelect,
    Cache, Prepared, BlendDensity, Interpolated, Slice, FindTop, OldBlended, BlendAlpha, BlendOffset, Beardifier
};

enum UnaryType { U_ABS, U_SQUARE, U_CUBE, U_SQRT, U_HALF_NEG, U_QUARTER_NEG, U_RECIPROCAL, U_NEGATE, U_SQUEEZE, U_LOG, U_SIGN };
enum BinaryType { B_ADD, B_SUB, B_MUL, B_DIV, B_MIN, B_MAX };
enum RoundType { R_FLOOR, R_ROUND, R_CEIL, R_TRUNC };

}  // namespace

struct SplineDef {
    bool is_const = false;
    float cval = 0;
    int coord = -1;   // index into Df::kids
    std::vector<float> loc, deriv;
    std::vector<std::shared_ptr<SplineDef>> sub;   // one per point (a constant point has is_const set)
};

struct Df {
    K kind = K::Constant;
    std::string key;
    int axes = 0;
    std::vector<DfP> kids;
    double xz = 0, ys = 0, d2 = 0, d3 = 0, d4 = 0;
    float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
    int i0 = 0, i1 = 0, i2 = 0, i3 = 0;
    int sub = 0;   // unary / binary / round type, gradient tiling, distance metric, axis
    std::string s;
    std::vector<float> thresholds;
    std::shared_ptr<SplineDef> sp;
    std::shared_ptr<Sampler> prepared;   // K::Prepared
};

namespace {

std::string fkey(double v) { char b[40]; snprintf(b, sizeof b, "%a", v); return b; }

DfP make(K kind, std::vector<DfP> kids, const std::function<void(Df&)>& fill) {
    auto d = std::make_shared<Df>();
    d->kind = kind;
    d->kids = std::move(kids);
    fill(*d);
    return d;
}

int axes_of(const Df& d) {
    auto kid_axes = [&](size_t i) { return d.kids[i]->axes; };
    switch (d.kind) {
        case K::Constant: return 0;
        case K::Ref: return kid_axes(0);
        case K::Noise: {
            int axes = ALL;
            if (d.ys == 0.0) axes &= ~AY;
            if (d.xz == 0.0) axes &= ~(AX | AZ);
            return axes | kid_axes(0) | kid_axes(1) | kid_axes(2);
        }
        case K::ShiftA: case K::ShiftB: return AX | AZ;
        case K::Shift: return ALL;
        case K::EndIslands: return AX | AZ;
        case K::DistToPoint: return ALL;
        case K::Gradient: return d.sub == 0 ? AX : d.sub == 1 ? AY : AZ;   // sub: the axis
        case K::BlendAlpha: case K::BlendOffset: return AX | AZ;
        case K::Beardifier: return ALL;
        case K::OldBlended: return ALL;
        case K::Prepared: return d.i0;
        case K::Slice: return kid_axes(0) & ~(d.sub == 0 ? AX : d.sub == 1 ? AY : AZ);
        case K::FindTop: return (kid_axes(0) | kid_axes(1)) & ~AY;
        default: {
            int a = 0;
            for (auto& k : d.kids) a |= k->axes;
            return a;
        }
    }
}

}  // namespace

// ======================================================================================================================
// Samplers
// ======================================================================================================================

namespace {

float jmin(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && std::signbit(b)) return b;
    return a <= b ? a : b;
}
float jmax(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && !std::signbit(b)) return b;
    return a >= b ? a : b;
}

struct ConstantS : Sampler {
    float v;
    explicit ConstantS(float x) : v(x) {}
    float value(SamplerCtx&, int, int, int) const override { return v; }
    void volume(SamplerCtx&, float* out, const Volume& vol) const override { std::fill(out, out + vol.size(), v); }
};

template <class F>
struct UnaryS : Sampler {
    const Sampler* in;
    F f;
    UnaryS(const Sampler* i, F fn) : in(i), f(fn) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return f(in->value(c, x, y, z)); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        in->volume(c, out, v);
        for (int i = 0, n = v.size(); i < n; i++) out[i] = f(out[i]);
    }
};
template <class F>
UnaryS<F>* unary(const Sampler* i, F f) { return new UnaryS<F>(i, f); }

float squeeze_apply(float v) {
    float c = mth_clamp(v, -1.0f, 1.0f);
    return c / 2.0f - (c * c * c) / 24.0f;
}

// binary operations
struct ConstAddS : Sampler {
    const Sampler* l; float r;
    ConstAddS(const Sampler* a, float b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return l->value(c, x, y, z) + r; }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override { l->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) out[i] += r; }
};
struct AddS : Sampler {
    const Sampler *l, *r;
    AddS(const Sampler* a, const Sampler* b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return l->value(c, x, y, z) + r->value(c, x, y, z); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        l->volume(c, out, v);
        Buf t(c, size_t(v.size()));
        r->volume(c, t.data(), v);
        for (int i = 0, n = v.size(); i < n; i++) out[i] += t[size_t(i)];
    }
};
struct ConstSubS : Sampler {   // left constant
    float l; const Sampler* r;
    ConstSubS(float a, const Sampler* b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return l - r->value(c, x, y, z); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override { r->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) out[i] = l - out[i]; }
};
struct SubS : Sampler {
    const Sampler *l, *r;
    SubS(const Sampler* a, const Sampler* b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return l->value(c, x, y, z) - r->value(c, x, y, z); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        l->volume(c, out, v);
        Buf t(c, size_t(v.size()));
        r->volume(c, t.data(), v);
        for (int i = 0, n = v.size(); i < n; i++) out[i] += -t[size_t(i)];
    }
};
struct ConstMulS : Sampler {
    const Sampler* l; float r;
    ConstMulS(const Sampler* a, float b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return l->value(c, x, y, z) * r; }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override { l->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) out[i] = out[i] * r; }
};
struct MulS : Sampler {
    const Sampler *l, *r;
    MulS(const Sampler* a, const Sampler* b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override {
        float a = l->value(c, x, y, z);
        if (a == 0.0f) return 0.0f;
        return a * r->value(c, x, y, z);
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        l->volume(c, out, v);
        Buf t(c, size_t(v.size()));
        r->volume(c, t.data(), v);
        for (int i = 0, n = v.size(); i < n; i++) out[i] = out[i] * t[size_t(i)];
    }
};
struct ConstDivS : Sampler {   // left constant
    float l; const Sampler* r;
    ConstDivS(float a, const Sampler* b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return l / r->value(c, x, y, z); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override { r->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) out[i] = l / out[i]; }
};
struct DivS : Sampler {
    const Sampler *l, *r;
    DivS(const Sampler* a, const Sampler* b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override {
        float a = l->value(c, x, y, z);
        if (a == 0.0f) return 0.0f;
        return a / r->value(c, x, y, z);
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        l->volume(c, out, v);
        Buf t(c, size_t(v.size()));
        r->volume(c, t.data(), v);
        for (int i = 0, n = v.size(); i < n; i++) out[i] = out[i] / t[size_t(i)];
    }
};
struct ConstMinS : Sampler {
    const Sampler* l; float r;
    ConstMinS(const Sampler* a, float b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return jmin(l->value(c, x, y, z), r); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override { l->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) if (r < out[i]) out[i] = r; }
};
struct MinS : Sampler {
    const Sampler *l, *r;
    MinS(const Sampler* a, const Sampler* b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { float a = l->value(c, x, y, z); return jmin(a, r->value(c, x, y, z)); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        l->volume(c, out, v);
        Buf t(c, size_t(v.size()));
        r->volume(c, t.data(), v);
        for (int i = 0, n = v.size(); i < n; i++) if (t[size_t(i)] < out[i]) out[i] = t[size_t(i)];
    }
};
struct ConstMaxS : Sampler {
    const Sampler* l; float r;
    ConstMaxS(const Sampler* a, float b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return jmax(l->value(c, x, y, z), r); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override { l->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) if (r > out[i]) out[i] = r; }
};
struct MaxS : Sampler {
    const Sampler *l, *r;
    MaxS(const Sampler* a, const Sampler* b) : l(a), r(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { float a = l->value(c, x, y, z); return jmax(a, r->value(c, x, y, z)); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        l->volume(c, out, v);
        Buf t(c, size_t(v.size()));
        r->volume(c, t.data(), v);
        for (int i = 0, n = v.size(); i < n; i++) if (t[size_t(i)] > out[i]) out[i] = t[size_t(i)];
    }
};

struct ClampS : Sampler {
    const Sampler* in; float lo, hi;
    ClampS(const Sampler* i, float a, float b) : in(i), lo(a), hi(b) {}
    float value(SamplerCtx& c, int x, int y, int z) const override { return mth_clamp(in->value(c, x, y, z), lo, hi); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override { in->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) out[i] = mth_clamp(out[i], lo, hi); }
};

// lerp: the three variants of LerpFunction differ in which input is a constant
struct LerpS : Sampler {
    const Sampler *alpha, *first, *second;
    int const_which;   // 0 none, 1 first constant, 2 second constant
    float cv;
    LerpS(const Sampler* a, const Sampler* f, const Sampler* s, int which, float c) : alpha(a), first(f), second(s), const_which(which), cv(c) {}
    float value(SamplerCtx& c, int x, int y, int z) const override {
        float a = alpha->value(c, x, y, z);
        if (const_which == 1) {
            if (a == 0.0f) return cv;
            if (a == 1.0f) return second->value(c, x, y, z);
            return mth_lerp(a, cv, second->value(c, x, y, z));
        }
        if (const_which == 2) {
            if (a == 0.0f) return first->value(c, x, y, z);
            if (a == 1.0f) return cv;
            return mth_lerp(a, first->value(c, x, y, z), cv);
        }
        if (a == 0.0f) return first->value(c, x, y, z);
        if (a == 1.0f) return second->value(c, x, y, z);
        float f = first->value(c, x, y, z);
        float s = second->value(c, x, y, z);
        return mth_lerp(a, f, s);
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        alpha->volume(c, out, v);
        size_t n = size_t(v.size());
        if (const_which == 1) {
            Buf sb(c, n);
            second->volume(c, sb.data(), v);
            for (size_t i = 0; i < n; i++) {
                float a = out[i], s = sb[i];
                out[i] = a == 0.0f ? cv : a == 1.0f ? s : mth_lerp(a, cv, s);
            }
        } else if (const_which == 2) {
            Buf fb(c, n);
            first->volume(c, fb.data(), v);
            for (size_t i = 0; i < n; i++) {
                float a = out[i], f = fb[i];
                out[i] = a == 0.0f ? f : a == 1.0f ? cv : mth_lerp(a, f, cv);
            }
        } else {
            Buf fb(c, n);
            first->volume(c, fb.data(), v);
            Buf sb(c, n);
            second->volume(c, sb.data(), v);
            for (size_t i = 0; i < n; i++) {
                float a = out[i], f = fb[i], s = sb[i];
                out[i] = a == 0.0f ? f : a == 1.0f ? s : mth_lerp(a, f, s);
            }
        }
    }
};

struct RangeChoiceS : Sampler {
    const Sampler *in, *when_in, *when_out;
    float lo, hi;
    bool both_const;
    float cin, cout_;
    float choose(float x) const { return x >= lo && x < hi ? cin : cout_; }
    float value(SamplerCtx& c, int x, int y, int z) const override {
        float i = in->value(c, x, y, z);
        if (both_const) return choose(i);
        if (i >= lo && i < hi) return when_in->value(c, x, y, z);
        return when_out->value(c, x, y, z);
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        size_t n = size_t(v.size());
        if (both_const) {
            in->volume(c, out, v);
            for (size_t i = 0; i < n; i++) out[i] = choose(out[i]);
            return;
        }
        when_in->volume(c, out, v);
        Buf ib(c, n);
        in->volume(c, ib.data(), v);
        Buf ob(c, n);
        when_out->volume(c, ob.data(), v);
        for (size_t i = 0; i < n; i++) {
            float x = ib[i];
            if (x >= lo && x < hi) continue;
            out[i] = ob[i];
        }
    }
};

struct IntervalSelectS : Sampler {
    const Sampler* in;
    std::vector<float> th;
    std::vector<const Sampler*> fn;
    size_t select(float x) const {
        if (th.size() == 1) return x < th[0] ? 0 : 1;
        for (size_t i = 0; i < th.size(); i++) if (x < th[i]) return i;
        return fn.size() - 1;
    }
    float value(SamplerCtx& c, int x, int y, int z) const override {
        float i = in->value(c, x, y, z);
        return fn[select(i)]->value(c, x, y, z);
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        size_t n = size_t(v.size());
        in->volume(c, out, v);
        std::vector<std::unique_ptr<Buf>> bufs;
        for (size_t k = 0; k < fn.size(); k++) {
            bufs.push_back(std::make_unique<Buf>(c, n));
            fn[k]->volume(c, bufs.back()->data(), v);
        }
        for (size_t i = 0; i < n; i++) out[i] = (*bufs[select(out[i])])[i];
    }
};

// ---- splines ---------------------------------------------------------------------------------------------------------

struct SplineS : Sampler {
    std::shared_ptr<SplineDef> def;
    std::vector<const Sampler*> coords;   // by SplineDef::coord

    template <class Get>
    static float sample(const SplineDef& s, Get& get) {
        if (s.is_const) return s.cval;
        float input = get(s.coord);
        // Mth.binarySearch(0, n, i -> input < locations[i]) - 1
        int from = 0, len = int(s.loc.size());
        while (len > 0) {
            int half = len / 2, mid = from + half;
            if (input < s.loc[size_t(mid)]) { len = half; continue; }
            from = mid + 1;
            len -= half + 1;
        }
        int start = from - 1;
        int last = int(s.loc.size()) - 1;
        auto linear_extend = [&](float val, int idx) {
            float d = s.deriv[size_t(idx)];
            if (d == 0.0f) return val;
            return val + d * (input - s.loc[size_t(idx)]);
        };
        if (start < 0) return linear_extend(sample(*s.sub[0], get), 0);
        if (start == last) return linear_extend(sample(*s.sub[size_t(last)], get), last);
        float x1 = s.loc[size_t(start)], x2 = s.loc[size_t(start + 1)];
        float t = (input - x1) / (x2 - x1);
        float d1 = s.deriv[size_t(start)], d2 = s.deriv[size_t(start + 1)];
        float y1 = sample(*s.sub[size_t(start)], get);
        float y2 = sample(*s.sub[size_t(start + 1)], get);
        float a = d1 * (x2 - x1) - (y2 - y1);
        float b = -d2 * (x2 - x1) + (y2 - y1);
        return mth_lerp(t, y1, y2) + t * (1.0f - t) * mth_lerp(t, a, b);
    }

    float value(SamplerCtx& c, int x, int y, int z) const override {
        std::vector<float> cache(coords.size(), std::nanf(""));
        auto get = [&](int idx) {
            float cv = cache[size_t(idx)];
            if (cv == cv) return cv;
            float v = coords[size_t(idx)]->value(c, x, y, z);
            cache[size_t(idx)] = v;
            return v;
        };
        return sample(*def, get);
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        size_t n = size_t(v.size());
        std::vector<std::unique_ptr<Buf>> bufs(coords.size());
        size_t index = 0;
        auto get = [&](int idx) {
            if (!bufs[size_t(idx)]) {
                bufs[size_t(idx)] = std::make_unique<Buf>(c, n);
                coords[size_t(idx)]->volume(c, bufs[size_t(idx)]->data(), v);
            }
            return (*bufs[size_t(idx)])[index];
        };
        for (size_t i = 0; i < n; i++) { index = i; out[i] = sample(*def, get); }
    }
};

// ---- caches ----------------------------------------------------------------------------------------------------------

struct CachingS : Sampler {
    int id;
    const Sampler* in;
    CachingS(int i, const Sampler* s) : id(i), in(s) {}
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        SamplerCtx::Cell* cell = c.cell(id);
        if (!cell) { in->volume(c, out, v); return; }
        if (!cell->has_volume || cell->buf.empty() || !(v == cell->vol)) {
            cell->vol = v;
            cell->has_volume = true;
            cell->buf.assign(size_t(v.size()), 0.0f);
            in->volume(c, cell->buf.data(), v);
        }
        memcpy(out, cell->buf.data(), size_t(v.size()) * sizeof(float));
    }
    float value(SamplerCtx& c, int x, int y, int z) const override {
        SamplerCtx::Cell* cell = c.cell(id);
        if (!cell) return in->value(c, x, y, z);
        int64_t key = (int64_t(x & 0x3FFFFFF) << 38) | (int64_t(z & 0x3FFFFFF) << 12) | int64_t(y & 0xFFF);
        if (cell->key == key && cell->value == cell->value) return cell->value;
        if (cell->has_volume && !cell->buf.empty()) {
            int idx = cell->vol.index_of_block(x, y, z);
            if (idx != -1) return cell->buf[size_t(idx)];
        }
        float v = in->value(c, x, y, z);
        cell->key = key;
        cell->value = v;
        return v;
    }
};

// ---- interpolated ----------------------------------------------------------------------------------------------------

int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
int floor_mod(int a, int b) { int m = a % b; return (m != 0 && ((m < 0) != (b < 0))) ? m + b : m; }

struct InterpolatedS : Sampler {
    const Sampler* in;
    int cxz, cy;
    float cxz_inv, cy_inv;
    InterpolatedS(const Sampler* i, int a, int b) : in(i), cxz(a), cy(b), cxz_inv(1.0f / float(a)), cy_inv(1.0f / float(b)) {}

    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        if (!((v.stepx != cxz && v.sx != 1) || (v.stepy != cy && v.sy != 1) || (v.stepz != cxz && v.sz != 1) || floor_mod(v.minx, cxz) != 0 || floor_mod(v.miny, cy) != 0 ||
              floor_mod(v.minz, cxz) != 0)) {
            in->volume(c, out, v);
            return;
        }
        if (v.stepx != 1 || v.stepy != 1 || v.stepz != 1) { non_block_step(c, out, v); return; }
        block_step(c, out, v);
    }
    void non_block_step(SamplerCtx& c, float* out, const Volume& v) const {
        Volume bv;
        bv.sx = v.sx * v.stepx; bv.sy = v.sy * v.stepy; bv.sz = v.sz * v.stepz;
        bv.minx = v.minx; bv.miny = v.miny; bv.minz = v.minz;
        Buf bb(c, size_t(bv.size()));
        block_step(c, bb.data(), bv);
        for (int z = 0; z < v.sz; z++)
            for (int x = 0; x < v.sx; x++)
                for (int y = 0; y < v.sy; y++) out[v.index(x, y, z)] = bb[size_t(bv.index(x * v.stepx, y * v.stepy, z * v.stepz))];
    }
    void block_step(SamplerCtx& c, float* out, const Volume& v) const {
        int min_cx = floor_div(v.minx, cxz), min_cy = floor_div(v.miny, cy), min_cz = floor_div(v.minz, cxz);
        int max_cx = floor_div(v.max_block_x(), cxz), max_cy = floor_div(v.max_block_y(), cy), max_cz = floor_div(v.max_block_z(), cxz);
        int ccx = max_cx - min_cx + 1, ccy = max_cy - min_cy + 1, ccz = max_cz - min_cz + 1;
        Volume cv;
        cv.sx = floor_mod(v.max_block_x(), cxz) == 0 ? ccx : ccx + 1;
        cv.sy = floor_mod(v.max_block_y(), cy) == 0 ? ccy : ccy + 1;
        cv.sz = floor_mod(v.max_block_z(), cxz) == 0 ? ccz : ccz + 1;
        cv.minx = min_cx * cxz; cv.miny = min_cy * cy; cv.minz = min_cz * cxz;
        cv.stepx = cxz; cv.stepy = cy; cv.stepz = cxz;
        Buf cb(c, size_t(cv.size()));
        in->volume(c, cb.data(), cv);
        for (int cz = 0; cz < ccz; cz++) {
            int ncz = std::min(cz + 1, cv.sz - 1);
            for (int cx = 0; cx < ccx; cx++) {
                int ncx = std::min(cx + 1, cv.sx - 1);
                float v000 = cb[size_t(cv.index(cx, 0, cz))];
                float v100 = cb[size_t(cv.index(ncx, 0, cz))];
                float v001 = cb[size_t(cv.index(cx, 0, ncz))];
                float v101 = cb[size_t(cv.index(ncx, 0, ncz))];
                for (int cyy = 0; cyy < ccy; cyy++) {
                    int ncy = std::min(cyy + 1, cv.sy - 1);
                    float v010 = cb[size_t(cv.index(cx, ncy, cz))];
                    float v110 = cb[size_t(cv.index(ncx, ncy, cz))];
                    float v011 = cb[size_t(cv.index(cx, ncy, ncz))];
                    float v111 = cb[size_t(cv.index(ncx, ncy, ncz))];
                    fill_cell(out, v, cv, cx, cyy, cz, v000, v100, v010, v110, v001, v101, v011, v111);
                    v000 = v010; v100 = v110; v001 = v011; v101 = v111;
                }
            }
        }
    }
    void fill_cell(float* out, const Volume& ov, const Volume& cv, int cx, int cyy, int cz, float v000, float v100, float v010, float v110, float v001, float v101, float v011,
                   float v111) const {
        int cox = cv.block_x(cx) - ov.minx, coy = cv.block_y(cyy) - ov.miny, coz = cv.block_z(cz) - ov.minz;
        int x0 = std::max(0, -cox), y0 = std::max(0, -coy), z0 = std::max(0, -coz);
        int x1 = std::min(cxz, ov.sx - cox) - 1, y1 = std::min(cy, ov.sy - coy) - 1, z1 = std::min(cxz, ov.sz - coz) - 1;
        for (int z = z0; z <= z1; z++) {
            int oz = coz + z;
            float az = float(z) * cxz_inv;
            float v00_ = mth_lerp(az, v000, v001), v01_ = mth_lerp(az, v010, v011), v10_ = mth_lerp(az, v100, v101), v11_ = mth_lerp(az, v110, v111);
            for (int x = x0; x <= x1; x++) {
                int ox = cox + x;
                float ax = float(x) * cxz_inv;
                float v_0_ = mth_lerp(ax, v00_, v10_), v_1_ = mth_lerp(ax, v01_, v11_);
                float step = (v_1_ - v_0_) * cy_inv;
                float value = v_0_ + step * float(y0);
                int oi = ov.index(ox, coy + y0, oz);
                for (int y = y0; y <= y1; y++) {
                    out[oi++] = value;
                    value += step;
                }
            }
        }
    }
    float value(SamplerCtx& c, int bx, int by, int bz) const override {
        int xin = floor_mod(bx, cxz), yin = floor_mod(by, cy), zin = floor_mod(bz, cxz);
        if (xin == 0 && yin == 0 && zin == 0) return in->value(c, bx, by, bz);
        Volume v;
        v.sx = v.sy = v.sz = 2;
        v.minx = bx - xin; v.miny = by - yin; v.minz = bz - zin;
        v.stepx = cxz; v.stepy = cy; v.stepz = cxz;
        Buf b(c, 8);
        in->volume(c, b.data(), v);
        return mth_lerp3(float(xin) / float(cxz), float(yin) / float(cy), float(zin) / float(cxz), b[size_t(v.index(0, 0, 0))], b[size_t(v.index(1, 0, 0))], b[size_t(v.index(0, 1, 0))],
                         b[size_t(v.index(1, 1, 0))], b[size_t(v.index(0, 0, 1))], b[size_t(v.index(1, 0, 1))], b[size_t(v.index(0, 1, 1))], b[size_t(v.index(1, 1, 1))]);
    }
};

// ---- slices ----------------------------------------------------------------------------------------------------------

struct SliceS : Sampler {   // axis 0 x, 1 y, 2 z, 3 the x and z pair (the fused double slice)
    const Sampler* in;
    int axis, a, b;   // a: the coordinate (x for the pair), b: z for the pair
    SliceS(const Sampler* i, int ax, int c1, int c2 = 0) : in(i), axis(ax), a(c1), b(c2) {}
    float value(SamplerCtx& c, int x, int y, int z) const override {
        switch (axis) {
            case 0: return in->value(c, a, y, z);
            case 1: return in->value(c, x, a, z);
            case 2: return in->value(c, x, y, a);
            default: return in->value(c, a, y, b);
        }
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        if (axis == 0) {
            if (v.sx == 1 && v.minx == a) { in->volume(c, out, v); return; }
            Volume iv = v; iv.sx = 1; iv.minx = a;
            Buf ib(c, size_t(iv.size()));
            in->volume(c, ib.data(), iv);
            int index = 0;
            for (int z = 0; z < v.sz; z++) for (int x = 0; x < v.sx; x++) for (int y = 0; y < v.sy; y++) out[index++] = ib[size_t(iv.index(0, y, z))];
        } else if (axis == 1) {
            if (v.sy == 1 && v.miny == a) { in->volume(c, out, v); return; }
            Volume iv = v; iv.sy = 1; iv.miny = a;
            Buf ib(c, size_t(iv.size()));
            in->volume(c, ib.data(), iv);
            for (int z = 0; z < v.sz; z++) for (int x = 0; x < v.sx; x++) {
                float val = ib[size_t(iv.index(x, 0, z))];
                std::fill(out + v.index(x, 0, z), out + v.index(x, 0, z) + v.sy, val);
            }
        } else if (axis == 2) {
            if (v.sz == 1 && v.minz == a) { in->volume(c, out, v); return; }
            Volume iv = v; iv.sz = 1; iv.minz = a;
            Buf ib(c, size_t(iv.size()));
            in->volume(c, ib.data(), iv);
            int index = 0;
            for (int z = 0; z < v.sz; z++) for (int x = 0; x < v.sx; x++) for (int y = 0; y < v.sy; y++) out[index++] = ib[size_t(iv.index(x, y, 0))];
        } else {
            if (v.sx == 1 && v.sz == 1 && v.minx == a && v.minz == b) { in->volume(c, out, v); return; }
            Volume iv = v; iv.sx = 1; iv.sz = 1; iv.minx = a; iv.minz = b;
            Buf ib(c, size_t(iv.size()));
            in->volume(c, ib.data(), iv);
            for (int y = 0; y < v.sy; y++) {
                float val = ib[size_t(iv.index(0, y, 0))];
                int index = v.index(0, y, 0);
                for (int z = 0; z < v.sz; z++) for (int x = 0; x < v.sx; x++) { out[index] = val; index += v.sy; }
            }
        }
    }
};

// ---- generators ------------------------------------------------------------------------------------------------------

struct NoiseS : Sampler {   // NoiseFunction.Sampler and the shifted variants
    std::shared_ptr<Noise> noise;
    double xz, ys;
    const Sampler *sx = nullptr, *sy = nullptr, *sz = nullptr;
    float value(SamplerCtx& c, int bx, int by, int bz) const override {
        if (!sx) return noise->get(double(bx) * xz, double(by) * ys, double(bz) * xz);
        double x = double(bx) * xz + double(sx->value(c, bx, by, bz));
        double y = sy ? double(by) * ys + double(sy->value(c, bx, by, bz)) : double(by) * ys;
        double z = double(bz) * xz + double(sz->value(c, bx, by, bz));
        return noise->get(x, y, z);
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        if (!sx) {
            std::fill(out, out + v.size(), 0.0f);
            noise->add_to_volume(out, v, xz, ys, 1.0f);
            return;
        }
        sx->volume(c, out, v);
        Buf zb(c, size_t(v.size()));
        sz->volume(c, zb.data(), v);
        std::unique_ptr<Buf> yb;
        if (sy) { yb = std::make_unique<Buf>(c, size_t(v.size())); sy->volume(c, yb->data(), v); }
        int index = 0;
        for (int z = 0; z < v.sz; z++) {
            double bz = double(v.block_z(z)) * xz;
            for (int x = 0; x < v.sx; x++) {
                double bx = double(v.block_x(x)) * xz;
                for (int y = 0; y < v.sy; y++) {
                    double nx = bx + double(out[index]);
                    double ny = sy ? double(v.block_y(y)) * ys + double((*yb)[size_t(index)]) : double(v.block_y(y)) * ys;
                    double nz = bz + double(zb[size_t(index)]);
                    out[index] = noise->get(nx, ny, nz);
                    index++;
                }
            }
        }
    }
};

struct ShiftBS : Sampler {
    std::shared_ptr<Noise> noise;
    float value(SamplerCtx&, int bx, int, int bz) const override { return noise->get(double(bz) * 0.25, double(bx) * 0.25, 0.0) * 4.0f; }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        Volume tv;
        tv.sx = v.sz; tv.sy = v.sx; tv.sz = 1; tv.minx = v.minz; tv.miny = v.minx; tv.minz = 0;
        tv.stepx = v.stepz; tv.stepy = v.stepx; tv.stepz = 1;
        Buf tb(c, size_t(tv.size()));
        std::fill(tb.data(), tb.data() + tv.size(), 0.0f);
        noise->add_to_volume(tb.data(), tv, 0.25, 0.25, 4.0f);
        for (int z = 0; z < v.sz; z++)
            for (int x = 0; x < v.sx; x++) {
                float val = tb[size_t(tv.index(z, x, 0))];
                std::fill(out + v.index(x, 0, z), out + v.index(x, 0, z) + v.sy, val);
            }
    }
};

struct GradientS : Sampler {
    int axis, tiling;   // tiling 0 clamp, 1 repeat, 2 mirrored
    int from, range, mn, mx;
    float from_value, factor;
    float compute(int coord) const {
        if (tiling == 0) {
            int rel = std::min(std::max(coord, mn), mx);   // Mth.clamp(int)
            rel -= from;
            return from_value + float(rel) * factor;
        }
        int rel = coord - from;
        if (tiling == 1) return from_value + float(floor_mod(rel, range)) * factor;
        int tile = floor_div(rel, range);
        int local = rel - tile * range;
        if ((tile & 1) == 0) return from_value + float(local) * factor;
        return from_value + float(range - local) * factor;
    }
    float value(SamplerCtx&, int x, int y, int z) const override { return compute(axis == 0 ? x : axis == 1 ? y : z); }
    void volume(SamplerCtx&, float* out, const Volume& v) const override {
        if (axis == 0) {
            for (int x = 0; x < v.sx; x++) {
                float val = compute(v.block_x(x));
                for (int z = 0; z < v.sz; z++) std::fill(out + v.index(x, 0, z), out + v.index(x, 0, z) + v.sy, val);
            }
        } else if (axis == 1) {
            for (int y = 0; y < v.sy; y++) {
                float val = compute(v.block_y(y));
                for (int z = 0; z < v.sz; z++) for (int x = 0; x < v.sx; x++) out[v.index(x, y, z)] = val;
            }
        } else {
            for (int z = 0; z < v.sz; z++) {
                float val = compute(v.block_z(z));
                std::fill(out + v.index(0, 0, z), out + v.index(0, 0, z) + v.sx * v.sy, val);
            }
        }
    }
};

struct DistToPointS : Sampler {
    int px, py, pz, metric;
    float value(SamplerCtx&, int x, int y, int z) const override {
        float dx = float(px - x), dy = float(py - y), dz = float(pz - z);
        switch (metric) {
            case 0: return float(std::sqrt(double(dx * dx + dy * dy + dz * dz)));
            case 1: return dx * dx + dy * dy + dz * dz;
            case 2: return std::fabs(dx) + std::fabs(dy) + std::fabs(dz);
            default: return jmax(jmax(std::fabs(dx), std::fabs(dy)), std::fabs(dz));
        }
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        int index = 0;
        for (int z = 0; z < v.sz; z++) for (int x = 0; x < v.sx; x++) for (int y = 0; y < v.sy; y++) out[index++] = value(c, v.block_x(x), v.block_y(y), v.block_z(z));
    }
};

struct EndIslandS : Sampler {
    std::shared_ptr<SimplexNoise> noise;
    float height_value(int sx, int sz) const {
        int chunk_x = sx / 2, chunk_z = sz / 2;
        int sub_x = sx % 2, sub_z = sz % 2;
        float doffs = -100.0f;
        for (int xo = -12; xo <= 12; xo++)
            for (int zo = -12; zo <= 12; zo++) {
                int64_t tx = chunk_x + xo, tz = chunk_z + zo;
                if (tx * tx + tz * tz <= 4096 || !(noise->get(double(tx), double(tz)) < -0.9f)) continue;
                float island = std::fmod(float(std::llabs(tx)) * 3439.0f + float(std::llabs(tz)) * 147.0f, 13.0f) + 9.0f;
                float xd = float(sub_x - xo * 2), zd = float(sub_z - zo * 2);
                float nd = 100.0f - float(std::sqrt(double(xd * xd + zd * zd))) * island;
                nd = mth_clamp(nd, -100.0f, 80.0f);
                doffs = jmax(doffs, nd);
            }
        return doffs;
    }
    float value(SamplerCtx&, int x, int, int z) const override { return (height_value(x / 8, z / 8) - 8.0f) / 128.0f; }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        for (int z = 0; z < v.sz; z++)
            for (int x = 0; x < v.sx; x++) {
                float val = value(c, v.block_x(x), 0, v.block_z(z));
                std::fill(out + v.index(x, 0, z), out + v.index(x, 0, z) + v.sy, val);
            }
    }
};

struct FindTopS : Sampler {
    const Sampler *density, *upper;
    int lower, cell_h;
    int find(SamplerCtx& c, int x, int z, float ub) const {
        int top = int(std::floor(ub / float(cell_h))) * cell_h;
        if (top <= lower) return lower;
        for (int py = top; py >= lower; py -= cell_h)
            if (density->value(c, x, py, z) > 0.0f) return py;
        return lower;
    }
    float value(SamplerCtx& c, int x, int y, int z) const override { return float(find(c, x, z, upper->value(c, x, y, z))); }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        if (v.sy != 1) throw std::runtime_error("find_top_surface sampled with sizeY != 1");
        upper->volume(c, out, v);
        int index = 0;
        for (int z = 0; z < v.sz; z++) for (int x = 0; x < v.sx; x++) { out[index] = float(find(c, v.block_x(x), v.block_z(z), out[index])); index++; }
    }
};

struct BlendDensityS : Sampler {
    const Sampler* in;
    float value(SamplerCtx& c, int x, int y, int z) const override { return in->value(c, x, y, z); }   // new worlds never blend
    void volume(SamplerCtx& c, float* out, const Volume& v) const override { in->volume(c, out, v); }
};

struct ContextBoundS : Sampler {   // blend_alpha, blend_offset and the beardifier: a field of the context or a constant
    int which;   // 0 beardifier, 1 blend alpha, 2 blend offset
    float fallback;
    const Sampler* field(SamplerCtx& c) const { return which == 0 ? c.beardifier : which == 1 ? c.blend_alpha : c.blend_offset; }
    float value(SamplerCtx& c, int x, int y, int z) const override {
        const Sampler* f = field(c);
        return f ? f->value(c, x, y, z) : fallback;
    }
    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
        const Sampler* f = field(c);
        if (f) f->volume(c, out, v);
        else std::fill(out, out + v.size(), fallback);
    }
};

}  // namespace

// ======================================================================================================================
// Compiler
// ======================================================================================================================

struct DfCompiler::Impl {
    RandomState3& rs;
    std::mutex mu;
    std::map<std::string, DfP> refs;                                   // parsed density function files
    std::map<std::string, DfP> prepared;                               // preparedCaches by the cache input's key
    std::vector<std::unique_ptr<Sampler>> arena;
    int next_cache_id = 0;

    explicit Impl(RandomState3& r) : rs(r) {}

    template <class T>
    T* keep(T* s) { arena.emplace_back(s); return s; }

    // ---- parsing -------------------------------------------------------------------------------------------------
    DfP constant(float v) {
        return make(K::Constant, {}, [&](Df& d) { d.f0 = v; d.key = "c" + fkey(double(v)); d.axes = 0; });
    }
    DfP node(K kind, std::vector<DfP> kids, const std::string& tag, const std::function<void(Df&)>& fill) {
        auto d = std::make_shared<Df>();
        d->kind = kind;
        d->kids = std::move(kids);
        fill(*d);
        std::string key = tag;
        if (kind != K::Ref) for (auto& k : d->kids) key += "(" + k->key + ")";
        d->key = key + d->s;
        d->axes = axes_of(*d);
        return d;
    }
    DfP parse_ref(const std::string& id_in) {
        std::string id = id_in.rfind("minecraft:", 0) == 0 ? id_in : "minecraft:" + id_in;
        auto it = refs.find(id);
        if (it != refs.end()) return it->second;
        DfP target = parse(rs.reg.get("density_function", id));
        DfP ref = node(K::Ref, {target}, "@" + id, [&](Df& d) { d.s = ""; });
        refs[id] = ref;
        return ref;
    }
    DfP parse(const Json& j) {
        if (j.is_num()) return constant(j.float_value());
        if (j.is_str()) return parse_ref(j.str);
        std::string type = j.at("type").str;
        if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
        auto kid = [&](const char* name) { return parse(j.at(name)); };
        auto opt_kid = [&](const char* name, float dflt) { const Json* f = j.find(name); return f ? parse(*f) : constant(dflt); };
        if (type == "constant") return constant(j.at("value").float_value());
        static const char* unary_names[] = {"abs", "square", "cube", "sqrt", "half_negative", "quarter_negative", "reciprocal", "negate", "squeeze", "log", "sign"};
        for (int t = 0; t < 11; t++)
            if (type == unary_names[t]) return node(K::Unary, {kid("input")}, "u" + std::to_string(t), [&](Df& d) { d.sub = t; });
        static const char* round_names[] = {"floor", "round", "ceil", "truncate"};
        for (int t = 0; t < 4; t++)
            if (type == round_names[t]) return node(K::Round, {kid("input"), opt_kid("multiple", 1.0f)}, "r" + std::to_string(t), [&](Df& d) { d.sub = t; });
        static const char* binary_names[] = {"add", "sub", "mul", "div", "min", "max"};
        for (int t = 0; t < 6; t++)
            if (type == binary_names[t]) return node(K::Binary, {kid("left"), kid("right")}, "b" + std::to_string(t), [&](Df& d) { d.sub = t; });
        if (type == "pow") return node(K::Pow, {kid("base"), kid("exponent")}, "pow", [&](Df&) {});
        if (type == "clamp") {
            float lo = j.at("min").float_value(), hi = j.at("max").float_value();
            return node(K::Clamp, {kid("input")}, "clamp" + fkey(lo) + fkey(hi), [&](Df& d) { d.f0 = lo; d.f1 = hi; });
        }
        if (type == "lerp") return node(K::Lerp, {kid("alpha"), kid("first"), kid("second")}, "lerp", [&](Df&) {});
        if (type == "range_choice") {
            float lo = j.at("min_inclusive").float_value(), hi = j.at("max_exclusive").float_value();
            return node(K::RangeChoice, {kid("input"), kid("when_in_range"), kid("when_out_of_range")}, "rc" + fkey(lo) + fkey(hi), [&](Df& d) { d.f0 = lo; d.f1 = hi; });
        }
        if (type == "interval_select") {
            std::vector<DfP> kids{kid("input")};
            std::string tag = "is";
            std::vector<float> th;
            for (auto& t : j.at("thresholds").arr) { th.push_back(t.float_value()); tag += fkey(double(th.back())); }
            for (auto& f : j.at("functions").arr) kids.push_back(parse(f));
            return node(K::IntervalSelect, kids, tag, [&](Df& d) { d.thresholds = th; });
        }
        if (type == "cache") return node(K::Cache, {kid("input")}, "cache", [&](Df&) {});
        if (type == "blend_density") return node(K::BlendDensity, {kid("input")}, "bd", [&](Df&) {});
        if (type == "interpolated") {
            int xz = int(j.at("cell_size_xz").int_value()), y = int(j.at("cell_size_y").int_value());
            return node(K::Interpolated, {kid("input")}, "ip" + std::to_string(xz) + "," + std::to_string(y), [&](Df& d) { d.i0 = xz; d.i1 = y; });
        }
        if (type == "slice") {
            std::string ax = j.at("axis").str;
            int a = ax == "x" ? 0 : ax == "y" ? 1 : 2;
            int coord = int(j.at("coordinate").int_value());
            return node(K::Slice, {kid("input")}, "sl" + std::to_string(a) + "," + std::to_string(coord), [&](Df& d) { d.sub = a; d.i0 = coord; });
        }
        if (type == "find_top_surface") {
            int lo = int(j.at("lower_bound").int_value()), ch = int(j.at("cell_height").int_value());
            return node(K::FindTop, {kid("density"), kid("upper_bound")}, "ft" + std::to_string(lo) + "," + std::to_string(ch), [&](Df& d) { d.i0 = lo; d.i1 = ch; });
        }
        if (type == "gradient") {
            std::string ax = j.at("axis").str;
            int a = ax == "x" ? 0 : ax == "y" ? 1 : 2;
            std::string tl = j.has("tiling") ? j.at("tiling").str : "clamp_to_edge";
            int tiling = tl == "repeat" ? 1 : tl == "mirrored_repeat" ? 2 : 0;
            int from = int(j.at("from_coordinate").int_value()), to = int(j.at("to_coordinate").int_value());
            float fv = j.at("from_value").float_value(), tv = j.at("to_value").float_value();
            return node(K::Gradient, {}, "g" + std::to_string(a) + std::to_string(tiling) + "," + std::to_string(from) + "," + std::to_string(to) + fkey(fv) + fkey(tv),
                        [&](Df& d) { d.sub = a; d.i0 = from; d.i1 = to; d.i2 = tiling; d.f0 = fv; d.f1 = tv; });
        }
        if (type == "noise") {
            std::string name = j.at("noise").str;
            double xz = j.at("xz_scale").number(), ys = j.at("y_scale").number();
            DfP z = constant(0.0f);
            std::vector<DfP> kids{j.has("shift_x") ? kid("shift_x") : z, j.has("shift_y") ? kid("shift_y") : z, j.has("shift_z") ? kid("shift_z") : z};
            return node(K::Noise, kids, "n" + name + fkey(xz) + fkey(ys), [&](Df& d) { d.s = name; d.xz = xz; d.ys = ys; });
        }
        if (type == "shift_a") return node(K::ShiftA, {}, "sa" + j.at("noise").str, [&](Df& d) { d.s = j.at("noise").str; });
        if (type == "shift_b") return node(K::ShiftB, {}, "sb" + j.at("noise").str, [&](Df& d) { d.s = j.at("noise").str; });
        if (type == "shift") return node(K::Shift, {}, "sh" + j.at("noise").str, [&](Df& d) { d.s = j.at("noise").str; });
        if (type == "end_outer_islands") return node(K::EndIslands, {}, "eoi", [&](Df&) {});
        if (type == "blend_alpha") return node(K::BlendAlpha, {}, "ba", [&](Df&) {});
        if (type == "blend_offset") return node(K::BlendOffset, {}, "bo", [&](Df&) {});
        if (type == "beardifier") return node(K::Beardifier, {}, "bf", [&](Df&) {});
        if (type == "distance_to_point") {
            const Json& p = j.at("point");
            std::string m = j.at("metric").str;
            int metric = m == "euclidean" ? 0 : m == "euclidean_squared" ? 1 : m == "manhattan" ? 2 : 3;
            int x = int(p.arr[0].int_value()), y = int(p.arr[1].int_value()), z = int(p.arr[2].int_value());
            return node(K::DistToPoint, {}, "dp" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + m, [&](Df& d) { d.i0 = x; d.i1 = y; d.i2 = z; d.sub = metric; });
        }
        if (type == "old_blended_noise") {
            double xz = j.at("xz_scale").number(), ys = j.at("y_scale").number(), xf = j.at("xz_factor").number(), yf = j.at("y_factor").number(), sm = j.at("smear_scale_multiplier").number();
            return node(K::OldBlended, {}, "obn" + fkey(xz) + fkey(ys) + fkey(xf) + fkey(yf) + fkey(sm), [&](Df& d) { d.xz = xz; d.ys = ys; d.d2 = xf; d.d3 = yf; d.d4 = sm; });
        }
        if (type == "spline") {
            std::vector<DfP> kids;
            std::string tag = "sp";
            std::function<std::shared_ptr<SplineDef>(const Json&)> build = [&](const Json& sj) {
                auto s = std::make_shared<SplineDef>();
                if (sj.is_num()) { s->is_const = true; s->cval = sj.float_value(); tag += "k" + fkey(double(s->cval)); return s; }
                s->coord = int(kids.size());
                kids.push_back(parse(sj.at("coordinate")));
                tag += "[" + kids.back()->key;
                for (auto& pt : sj.at("points").arr) {
                    s->loc.push_back(pt.at("location").float_value());
                    s->deriv.push_back(pt.at("derivative").float_value());
                    tag += "," + fkey(double(s->loc.back())) + ":" + fkey(double(s->deriv.back()));
                    s->sub.push_back(build(pt.at("value")));
                }
                tag += "]";
                return s;
            };
            auto def = build(j.at("spline"));
            return node(K::Spline, kids, tag, [&](Df& d) { d.sp = def; });
        }
        throw std::runtime_error("unknown density function type " + type);
    }

    // ---- rewriting -----------------------------------------------------------------------------------------------
    using Rule = std::function<DfP(const DfP&)>;
    DfP rewrite_children(const DfP& f, const Rule& rule) {
        if (f->kind == K::Prepared || f->kind == K::Constant) return f;
        if (f->kind == K::Ref) {
            DfP nf = rule(f->kids[0]);
            return nf;   // HolderHolder.rewriteChildren returns the rewritten value (and `this` only when it is unchanged, which never happens for a rule that inlines)
        }
        bool changed = false;
        std::vector<DfP> nk;
        for (auto& k : f->kids) {
            DfP r = rule(k);
            if (r != k) changed = true;
            nk.push_back(r);
        }
        if (!changed) return f;
        auto d = std::make_shared<Df>(*f);
        d->kids = nk;
        d->axes = axes_of(*d);
        std::string key = f->key;   // the rewritten tree is never used as a key again
        d->key = key;
        return d;
    }
    DfP slice_wrap(const DfP& f, int axis, int coord = 0) {
        auto d = std::make_shared<Df>();
        d->kind = K::Slice;
        d->kids = {f};
        d->sub = axis;
        d->i0 = coord;
        d->axes = axes_of(*d);
        d->key = "slice" + std::to_string(axis) + "(" + f->key + ")";
        return d;
    }
    DfP slice_rewrite(const DfP& f, int parent) {
        if (f->kind == K::Constant || f->kind == K::Gradient) return f;
        int axes = f->axes;
        if (parent == axes) return rewrite_children(f, [&](const DfP& k) { return slice_rewrite(k, parent); });
        DfP nf = rewrite_children(f, [&](const DfP& k) { return slice_rewrite(k, axes); });
        int removed = parent & ~axes;
        // already sliced axes at the top of the new function
        int existing = 0;
        for (DfP g = nf; g->kind == K::Slice; g = g->kids[0]) existing |= g->sub == 0 ? AX : g->sub == 1 ? AY : AZ;
        int filtered = removed & ~existing;
        if (filtered & AX) nf = slice_wrap(nf, 0);
        if (filtered & AZ) nf = slice_wrap(nf, 2);
        if (filtered & AY) nf = slice_wrap(nf, 1);
        return nf;
    }
    DfP r1(const DfP& f_in) {
        DfP f = f_in;
        while (f->kind == K::Ref) f = f->kids[0];
        if (f->kind == K::Cache) return reuse_or_prepare(f);
        return rewrite_children(f, [&](const DfP& k) { return r1(k); });
    }
    DfP optimize(const DfP& f) { return slice_rewrite(r1(f), ALL); }
    DfP reuse_or_prepare(const DfP& cache) {
        const std::string& key = cache->kids[0]->key;
        auto it = prepared.find(key);
        if (it != prepared.end()) return it->second;
        int id = next_cache_id++;
        DfP input = optimize(cache->kids[0]);
        const Sampler* in = compile(input);
        auto d = std::make_shared<Df>();
        d->kind = K::Prepared;
        d->i0 = input->axes;
        d->axes = input->axes;
        d->key = "prepared" + std::to_string(id);
        d->prepared = std::shared_ptr<Sampler>(new CachingS(id, in));
        prepared[key] = d;
        return d;
    }

    // ---- compiling -----------------------------------------------------------------------------------------------
    static bool is_const(const DfP& f) { return f->kind == K::Constant; }
    const Sampler* compile(const DfP& f_in) {
        DfP f = f_in;
        while (f->kind == K::Ref) f = f->kids[0];
        switch (f->kind) {
            case K::Constant: return keep(new ConstantS(f->f0));
            case K::Prepared: return f->prepared.get();
            case K::Cache: throw std::runtime_error("cannot compile a cache before it has been deduplicated");
            case K::Unary: {
                const Sampler* in = compile(f->kids[0]);
                switch (f->sub) {
                    case U_ABS: return keep(unary(in, [](float x) { return std::fabs(x); }));
                    case U_SQUARE: return keep(unary(in, [](float x) { return x * x; }));
                    case U_CUBE: return keep(unary(in, [](float x) { return x * x * x; }));
                    case U_SQRT: return keep(unary(in, [](float x) { return float(std::sqrt(double(x))); }));
                    case U_HALF_NEG: return keep(unary(in, [](float x) { return x > 0.0f ? x : x * 0.5f; }));
                    case U_QUARTER_NEG: return keep(unary(in, [](float x) { return x > 0.0f ? x : x * 0.25f; }));
                    case U_RECIPROCAL: return keep(unary(in, [](float x) { return 1.0f / x; }));
                    case U_NEGATE: return keep(unary(in, [](float x) { return -x; }));
                    case U_SQUEEZE: return keep(unary(in, [](float x) { return squeeze_apply(x); }));
                    case U_LOG: return keep(unary(in, [](float x) { return float(std::log(double(x))); }));
                    default: return keep(unary(in, [](float x) { return x > 0.0f ? 1.0f : x < 0.0f ? -1.0f : x; }));
                }
            }
            case K::Round: {
                const Sampler* in = compile(f->kids[0]);
                const Sampler* mult = compile(f->kids[1]);
                int type = f->sub;
                auto to_int = [type](float x) -> float {
                    switch (type) {
                        case R_FLOOR: return float(std::floor(double(x)));
                        case R_ROUND: return float(std::floor(double(x) + 0.5));
                        case R_CEIL: return float(std::ceil(double(x)));
                        default: return x > 0.0f ? float(std::floor(double(x))) : float(std::ceil(double(x)));
                    }
                };
                if (is_const(f->kids[1]) && f->kids[1]->f0 == 1.0f) return keep(unary(in, to_int));
                struct RoundS : Sampler {
                    const Sampler *in, *mult;
                    std::function<float(float)> to_int;
                    float apply(float x, float m) const { return m == 0.0f ? x : to_int(x / m) * m; }
                    float value(SamplerCtx& c, int x, int y, int z) const override { float a = in->value(c, x, y, z); float m = mult->value(c, x, y, z); return apply(a, m); }
                    void volume(SamplerCtx& c, float* out, const Volume& v) const override {
                        in->volume(c, out, v);
                        Buf mb(c, size_t(v.size()));
                        mult->volume(c, mb.data(), v);
                        for (int i = 0, n = v.size(); i < n; i++) out[i] = apply(out[i], mb[size_t(i)]);
                    }
                };
                auto* s = new RoundS;
                s->in = in; s->mult = mult; s->to_int = to_int;
                return keep(s);
            }
            case K::Binary: {
                const DfP &l = f->kids[0], &r = f->kids[1];
                const Sampler* ls = compile(l);
                const Sampler* rs_ = compile(r);
                bool lc = is_const(l), rc = is_const(r);
                switch (f->sub) {
                    case B_ADD:
                        if (lc) return keep(new ConstAddS(rs_, l->f0));
                        if (!rc) return keep(new AddS(ls, rs_));
                        return keep(new ConstAddS(ls, r->f0));
                    case B_SUB:
                        if (lc) return keep(new ConstSubS(l->f0, rs_));
                        if (!rc) return keep(new SubS(ls, rs_));
                        return keep(new ConstAddS(ls, -r->f0));
                    case B_MUL:
                        if (lc) return keep(new ConstMulS(rs_, l->f0));
                        if (!rc) return keep(new MulS(ls, rs_));
                        return keep(new ConstMulS(ls, r->f0));
                    case B_DIV:
                        if (lc) return keep(new ConstDivS(l->f0, rs_));
                        if (!rc) return keep(new DivS(ls, rs_));
                        return keep(new ConstMulS(ls, 1.0f / r->f0));
                    case B_MIN:
                        if (lc) return keep(new ConstMinS(rs_, l->f0));
                        if (!rc) return keep(new MinS(ls, rs_));
                        return keep(new ConstMinS(ls, r->f0));
                    default:
                        if (lc) return keep(new ConstMaxS(rs_, l->f0));
                        if (!rc) return keep(new MaxS(ls, rs_));
                        return keep(new ConstMaxS(ls, r->f0));
                }
            }
            case K::Pow: {
                const DfP &b = f->kids[0], &e = f->kids[1];
                const Sampler* bs = compile(b);
                const Sampler* es = compile(e);
                if (is_const(b)) {
                    struct ConstBase : Sampler {
                        double base; const Sampler* ex;
                        float value(SamplerCtx& c, int x, int y, int z) const override { return float(std::pow(base, double(ex->value(c, x, y, z)))); }
                        void volume(SamplerCtx& c, float* out, const Volume& v) const override { ex->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) out[i] = float(std::pow(base, double(out[i]))); }
                    };
                    auto* s = new ConstBase; s->base = double(b->f0); s->ex = es;
                    return keep(s);
                }
                if (!is_const(e)) {
                    struct PowS : Sampler {
                        const Sampler *b, *e;
                        float value(SamplerCtx& c, int x, int y, int z) const override { float bb = b->value(c, x, y, z); float ee = e->value(c, x, y, z); return float(std::pow(double(bb), double(ee))); }
                        void volume(SamplerCtx& c, float* out, const Volume& v) const override {
                            b->volume(c, out, v);
                            Buf eb(c, size_t(v.size()));
                            e->volume(c, eb.data(), v);
                            for (int i = 0, n = v.size(); i < n; i++) out[i] = float(std::pow(double(out[i]), double(eb[size_t(i)])));
                        }
                    };
                    auto* s = new PowS; s->b = bs; s->e = es;
                    return keep(s);
                }
                float ex = e->f0, ab = std::fabs(ex);
                const Sampler* special = nullptr;
                if (ab == 0.5f) special = keep(unary(bs, [](float x) { return float(std::sqrt(double(x))); }));
                else if (ab == 1.0f) special = bs;
                else if (ab == 2.0f) special = keep(unary(bs, [](float x) { return x * x; }));
                else if (ab == 3.0f) special = keep(unary(bs, [](float x) { return x * x * x; }));
                if (special) return ex >= 0.0f ? special : keep(unary(special, [](float x) { return 1.0f / x; }));
                struct ConstExp : Sampler {
                    const Sampler* b; double ex;
                    float value(SamplerCtx& c, int x, int y, int z) const override { return float(std::pow(double(b->value(c, x, y, z)), ex)); }
                    void volume(SamplerCtx& c, float* out, const Volume& v) const override { b->volume(c, out, v); for (int i = 0, n = v.size(); i < n; i++) out[i] = float(std::pow(double(out[i]), ex)); }
                };
                auto* s = new ConstExp; s->b = bs; s->ex = double(ex);
                return keep(s);
            }
            case K::Spline: {
                auto* s = new SplineS;
                s->def = f->sp;
                for (auto& k : f->kids) s->coords.push_back(compile(k));
                return keep(s);
            }
            case K::Lerp: {
                const DfP &a = f->kids[0], &fi = f->kids[1], &se = f->kids[2];
                const Sampler* as = compile(a);
                const Sampler* fs = compile(fi);
                const Sampler* ss = compile(se);
                if (is_const(fi)) return keep(new LerpS(as, fs, ss, 1, fi->f0));
                if (is_const(se)) return keep(new LerpS(as, fs, ss, 2, se->f0));
                return keep(new LerpS(as, fs, ss, 0, 0.0f));
            }
            case K::Clamp: return keep(new ClampS(compile(f->kids[0]), f->f0, f->f1));
            case K::RangeChoice: {
                auto* s = new RangeChoiceS;
                s->in = compile(f->kids[0]);
                s->lo = f->f0; s->hi = f->f1;
                s->both_const = is_const(f->kids[1]) && is_const(f->kids[2]);
                s->cin = f->kids[1]->f0; s->cout_ = f->kids[2]->f0;
                s->when_in = s->both_const ? nullptr : compile(f->kids[1]);
                s->when_out = s->both_const ? nullptr : compile(f->kids[2]);
                return keep(s);
            }
            case K::IntervalSelect: {
                auto* s = new IntervalSelectS;
                s->in = compile(f->kids[0]);
                s->th = f->thresholds;
                for (size_t i = 1; i < f->kids.size(); i++) s->fn.push_back(compile(f->kids[i]));
                if (s->th.size() == 1) { const Sampler* a = s->fn.front(); const Sampler* b = s->fn.back(); s->fn = {a, b}; }
                return keep(s);
            }
            case K::BlendDensity: {
                auto* s = new BlendDensityS;
                s->in = compile(f->kids[0]);
                return keep(s);
            }
            case K::Interpolated: return keep(new InterpolatedS(compile(f->kids[0]), f->i0, f->i1));
            case K::Slice: {
                const DfP& in = f->kids[0];
                if (in->kind == K::Slice) {
                    int a1 = f->sub, a2 = in->sub;
                    if ((a1 == 0 && a2 == 2) || (a1 == 2 && a2 == 0)) {
                        int x = a1 == 0 ? f->i0 : in->i0, z = a1 == 0 ? in->i0 : f->i0;
                        return keep(new SliceS(compile(in->kids[0]), 3, x, z));
                    }
                }
                return keep(new SliceS(compile(in), f->sub, f->i0));
            }
            case K::FindTop: {
                auto* s = new FindTopS;
                s->density = compile(f->kids[0]);
                s->upper = compile(f->kids[1]);
                s->lower = f->i0; s->cell_h = f->i1;
                return keep(new SliceS(keep(s), 1, 0));
            }
            case K::Gradient: {
                auto* s = new GradientS;
                s->axis = f->sub; s->tiling = f->i2; s->from = f->i0;
                s->range = f->i1 - f->i0;
                s->factor = (f->f1 - f->f0) / float(s->range);
                s->from_value = f->f0;
                s->mn = std::min(f->i0, f->i1); s->mx = std::max(f->i0, f->i1);
                return keep(s);
            }
            case K::Noise: {
                auto* s = new NoiseS;
                s->noise = rs.noise(f->s);
                s->xz = f->xz; s->ys = f->ys;
                bool zx = is_const(f->kids[0]) && f->kids[0]->f0 == 0.0f, zy = is_const(f->kids[1]) && f->kids[1]->f0 == 0.0f, zz = is_const(f->kids[2]) && f->kids[2]->f0 == 0.0f;
                if (!(zx && zy && zz)) {
                    s->sx = compile(f->kids[0]);
                    s->sz = compile(f->kids[2]);
                    if (!zy) s->sy = compile(f->kids[1]);
                }
                return keep(s);
            }
            case K::ShiftA: case K::Shift: {
                auto* n = new NoiseS;
                n->noise = rs.noise(f->s);
                n->xz = 0.25; n->ys = f->kind == K::Shift ? 0.25 : 0.0;
                return keep(new ConstMulS(keep(n), 4.0f));
            }
            case K::ShiftB: {
                auto* s = new ShiftBS;
                s->noise = rs.noise(f->s);
                return keep(s);
            }
            case K::DistToPoint: {
                auto* s = new DistToPointS;
                s->px = f->i0; s->py = f->i1; s->pz = f->i2; s->metric = f->sub;
                return keep(s);
            }
            case K::EndIslands: {
                auto* s = new EndIslandS;
                LegacyRandom r(rs.seed);
                r.consume_count(17292);
                s->noise = std::make_shared<SimplexNoise>(r, true);
                return keep(s);
            }
            case K::BlendAlpha: { auto* s = new ContextBoundS; s->which = 1; s->fallback = 1.0f; return keep(s); }
            case K::BlendOffset: { auto* s = new ContextBoundS; s->which = 2; s->fallback = 0.0f; return keep(s); }
            case K::Beardifier: { auto* s = new ContextBoundS; s->which = 0; s->fallback = 0.0f; return keep(s); }
            case K::OldBlended: {
                double xz_mult = 684.412 * f->xz, y_mult = 684.412 * f->ys;
                std::unique_ptr<Rng> random;
                if (rs.legacy) random = std::make_unique<LegacyRandom>(rs.seed);
                else random = rs.random_from_hash("minecraft:terrain");
                double limit_smear = y_mult * f->d4;
                double main_smear = limit_smear / f->d3;
                auto fbm = [&](int first_octave, double smear_y, double value_factor) {
                    int octaves = -first_octave + 1;
                    double factor = 1.0;
                    value_factor /= std::pow(2.0, double(octaves)) - 1.0;
                    auto stack = std::make_shared<NoiseStack>();
                    for (int i = octaves - 1; i >= 0; i--) {
                        stack->add(std::make_shared<SmearedPerlinNoise>(*random, smear_y * factor), factor, float(value_factor));
                        factor /= 2.0;
                        value_factor *= 2.0;
                    }
                    return stack;
                };
                auto min_limit = fbm(-15, limit_smear, 0.9999847412109375);
                auto max_limit = fbm(-15, limit_smear, 0.9999847412109375);
                auto main_n = fbm(-7, main_smear, 12.75);
                auto mk = [&](std::shared_ptr<Noise> n, double xz, double ys) { auto* s = new NoiseS; s->noise = n; s->xz = xz; s->ys = ys; return keep(s); };
                const Sampler* min_s = mk(min_limit, xz_mult, y_mult);
                const Sampler* max_s = mk(max_limit, xz_mult, y_mult);
                const Sampler* main_s = mk(main_n, xz_mult / f->d2, y_mult / f->d3);
                const Sampler* choice = keep(new ClampS(keep(new ConstAddS(main_s, 0.5f)), 0.0f, 1.0f));
                return keep(new LerpS(choice, min_s, max_s, 0, 0.0f));
            }
            default: throw std::runtime_error("cannot compile this density function");
        }
    }

    std::map<std::string, const Sampler*> memo;
    const Sampler* sampler_of(const DfP& f) {
        std::lock_guard<std::mutex> l(mu);
        auto it = memo.find(f->key);
        if (it != memo.end()) return it->second;
        const Sampler* s = compile(optimize(f));
        memo[f->key] = s;
        return s;
    }
};

DfCompiler::DfCompiler(RandomState3& r) : rs(r), p_(std::make_unique<Impl>(r)) {}
DfCompiler::~DfCompiler() = default;

const Sampler* DfCompiler::sampler(const Json& fn) {
    DfP f;
    {
        std::lock_guard<std::mutex> l(p_->mu);
        f = p_->parse(fn);
    }
    return p_->sampler_of(f);
}
const Sampler* DfCompiler::sampler_of_ref(const std::string& id) {
    DfP f;
    {
        std::lock_guard<std::mutex> l(p_->mu);
        f = p_->parse_ref(id);
    }
    return p_->sampler_of(f);
}

}  // namespace pn::vg::v3
