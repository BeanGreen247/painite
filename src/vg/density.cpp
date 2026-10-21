#include "density.hpp"
#include "beardifier.hpp"
#include <cmath>
#include <stdexcept>

namespace pn::vg {

namespace {

// ---- leaves ------------------------------------------------------------------------------------------------------

struct Constant : Node {
    double v;
    explicit Constant(double x) : v(x) {}
    double compute(const Ctx&) const override { return v; }
};

struct YClampedGradient : Node {
    int from_y, to_y;
    double from_v, to_v;
    double compute(const Ctx& c) const override { return clamped_map(double(c.y), double(from_y), double(to_y), from_v, to_v); }
};

struct NoiseNode : Node {
    const NormalNoise* noise;
    double xz_scale, y_scale;
    double compute(const Ctx& c) const override { return noise ? noise->get_value(c.x * xz_scale, c.y * y_scale, c.z * xz_scale) : 0.0; }
};

struct ShiftedNoise : Node {
    NodeP sx, sy, sz;
    const NormalNoise* noise;
    double xz_scale, y_scale;
    double compute(const Ctx& c) const override {
        double x = c.x * xz_scale + sx->compute(c);
        double y = c.y * y_scale + sy->compute(c);
        double z = c.z * xz_scale + sz->compute(c);
        return noise ? noise->get_value(x, y, z) : 0.0;
    }
};

struct ShiftNode : Node {
    enum Kind { A, B, Both } kind;
    const NormalNoise* noise;
    double compute(const Ctx& c) const override {
        double lx, ly, lz;
        switch (kind) {
            case A: lx = c.x; ly = 0.0; lz = c.z; break;
            case B: lx = c.z; ly = c.x; lz = 0.0; break;
            default: lx = c.x; ly = c.y; lz = c.z; break;
        }
        return noise ? noise->get_value(lx * 0.25, ly * 0.25, lz * 0.25) * 4.0 : 0.0;
    }
};

struct OldBlended : Node {
    std::unique_ptr<BlendedNoise> noise;
    double compute(const Ctx& c) const override { return noise->compute(double(c.x), double(c.y), double(c.z)); }
};

struct EndIslands : Node {
    std::unique_ptr<SimplexNoise> noise;
    static float height_value(const SimplexNoise& n, int section_x, int section_z) {
        int chunk_x = section_x / 2, chunk_z = section_z / 2;
        int sub_x = section_x % 2, sub_z = section_z % 2;
        float doffs = 100.0f - std::sqrt(float(section_x * section_x + section_z * section_z)) * 8.0f;
        doffs = doffs < -100.0f ? -100.0f : (doffs > 80.0f ? 80.0f : doffs);
        for (int xo = -12; xo <= 12; xo++)
            for (int zo = -12; zo <= 12; zo++) {
                int64_t tx = int64_t(chunk_x) + xo, tz = int64_t(chunk_z) + zo;
                if (tx * tx + tz * tz > 4096 && n.get_value(double(tx), double(tz)) < double(-0.9f)) {
                    float island_size = std::fmod(std::fabs(float(tx)) * 3439.0f + std::fabs(float(tz)) * 147.0f, 13.0f) + 9.0f;
                    float xd = float(sub_x - xo * 2), zd = float(sub_z - zo * 2);
                    float nd = 100.0f - std::sqrt(xd * xd + zd * zd) * island_size;
                    nd = nd < -100.0f ? -100.0f : (nd > 80.0f ? 80.0f : nd);
                    doffs = std::max(doffs, nd);
                }
            }
        return doffs;
    }
    double compute(const Ctx& c) const override { return (double(height_value(*noise, c.x / 8, c.z / 8)) - 8.0) / 128.0; }
};

// ---- operators ---------------------------------------------------------------------------------------------------

struct Ap2 : Node {
    enum Kind { Add, Mul, Min, Max } kind;
    NodeP a, b;
    double compute(const Ctx& c) const override {
        double v1 = a->compute(c);
        switch (kind) {
            case Add: return v1 + b->compute(c);
            case Mul: return v1 == 0.0 ? 0.0 : v1 * b->compute(c);
            case Min: return jmin(v1, b->compute(c));
            default: return jmax(v1, b->compute(c));
        }
    }
};

struct MulOrAdd : Node {   // one argument was a constant
    bool is_mul;
    NodeP input;
    double arg;
    double compute(const Ctx& c) const override { double v = input->compute(c); return is_mul ? v * arg : v + arg; }
};

struct Mapped : Node {
    enum Kind { Abs, Square, Cube, HalfNegative, QuarterNegative, Invert, Squeeze } kind;
    NodeP input;
    double compute(const Ctx& c) const override {
        double v = input->compute(c);
        switch (kind) {
            case Abs: return std::fabs(v);
            case Square: return v * v;
            case Cube: return v * v * v;
            case HalfNegative: return v > 0.0 ? v : v * 0.5;
            case QuarterNegative: return v > 0.0 ? v : v * 0.25;
            case Invert: return 1.0 / v;
            default: { double cl = v < -1.0 ? -1.0 : jmin(v, 1.0); return cl / 2.0 - cl * cl * cl / 24.0; }
        }
    }
};

struct Clamp : Node {
    NodeP input;
    double lo, hi;
    double compute(const Ctx& c) const override { double v = input->compute(c); return v < lo ? lo : jmin(v, hi); }
};

struct RangeChoice : Node {
    NodeP input, in_range, out_range;
    double lo, hi;
    double compute(const Ctx& c) const override {
        double v = input->compute(c);
        return v >= lo && v < hi ? in_range->compute(c) : out_range->compute(c);
    }
};

// 26.1's weird_scaled_sampler (26.2 spells the same caves out with interval_select): the input picks a rarity that scales the noise lookup
struct WeirdScaledSampler : Node {
    NodeP input;
    const NormalNoise* noise;
    bool type2;
    double compute(const Ctx& c) const override {
        double v = input->compute(c);
        double rarity;
        if (type2) rarity = v < -0.75 ? 0.5 : v < -0.5 ? 0.75 : v < 0.5 ? 1.0 : v < 0.75 ? 2.0 : 3.0;
        else rarity = v < -0.5 ? 0.75 : v < 0.0 ? 1.0 : v < 0.5 ? 1.5 : 2.0;
        return rarity * std::abs(noise ? noise->get_value(c.x / rarity, c.y / rarity, c.z / rarity) : 0.0);
    }
};

struct IntervalSelect : Node {
    NodeP input;
    std::vector<double> thresholds;
    std::vector<NodeP> functions;
    double compute(const Ctx& c) const override {
        double v = input->compute(c);
        for (size_t i = 0; i < thresholds.size(); i++) if (v < thresholds[i]) return functions[i]->compute(c);
        return functions.back()->compute(c);
    }
};

struct FindTopSurface : Node {
    NodeP density, upper;
    int lower, cell_height;
    double compute(const Ctx& c) const override {
        int top_y = int(std::floor(upper->compute(c) / cell_height)) * cell_height;
        if (top_y <= lower) return lower;
        for (int y = top_y; y >= lower; y -= cell_height)
            if (density->compute(Ctx{c.x, y, c.z, nullptr}) > 0.0) return y;
        return lower;
    }
};

// ---- splines (float arithmetic, like Mojang's CubicSpline) -----------------------------------------------------------

struct SplineNode {
    NodeP coordinate;
    std::vector<float> locations, derivatives;
    std::vector<std::unique_ptr<SplineNode>> subs;   // null where the value is a constant
    std::vector<float> constants;
    bool is_constant = false;
    float constant = 0;

    static float lerpf(float a, float p0, float p1) { return p0 + a * (p1 - p0); }
    static float linear_extend(float input, const std::vector<float>& locs, float value, const std::vector<float>& ders, int index) {
        float d = ders[size_t(index)];
        return d == 0.0f ? value : value + d * (input - locs[size_t(index)]);
    }
    float sample(const Ctx& c) const {
        if (is_constant) return constant;
        float input = float(coordinate->compute(c));
        // Mth.binarySearch(0, n, i -> input < locations[i]) - 1
        int from = 0, len = int(locations.size());
        while (len > 0) {
            int half = len / 2, middle = from + half;
            if (input < locations[size_t(middle)]) len = half; else { from = middle + 1; len -= half + 1; }
        }
        int start = from - 1;
        int last = int(locations.size()) - 1;
        auto value_at = [&](int i) { return subs[size_t(i)] ? subs[size_t(i)]->sample(c) : constants[size_t(i)]; };
        if (start < 0) return linear_extend(input, locations, value_at(0), derivatives, 0);
        if (start == last) return linear_extend(input, locations, value_at(last), derivatives, last);
        float x1 = locations[size_t(start)], x2 = locations[size_t(start) + 1];
        float t = (input - x1) / (x2 - x1);
        float d1 = derivatives[size_t(start)], d2 = derivatives[size_t(start) + 1];
        float y1 = value_at(start), y2 = value_at(start + 1);
        float a = d1 * (x2 - x1) - (y2 - y1);
        float b = -d2 * (x2 - x1) + (y2 - y1);
        return lerpf(t, y1, y2) + t * (1.0f - t) * lerpf(t, a, b);
    }
};

struct SplineDensity : Node {
    std::unique_ptr<SplineNode> spline;
    double compute(const Ctx& c) const override { return double(spline->sample(c)); }
};

struct BeardifierNode : Node {
    const ChunkNoise* chunk;
    explicit BeardifierNode(const ChunkNoise* c) : chunk(c) {}
    double compute(const Ctx& c) const override;
};

// ---- caches and markers ------------------------------------------------------------------------------------------

struct FlatCache : Node {
    ChunkNoise* chunk;
    NodeP filler;
    std::vector<double> values;
    int size_xz;
    FlatCache(ChunkNoise* c, NodeP f) : chunk(c), filler(std::move(f)) {
        size_xz = chunk->noise_size_xz + 1;
        values.resize(size_t(size_xz * size_xz));
        for (int x = 0; x <= chunk->noise_size_xz; x++)
            for (int z = 0; z <= chunk->noise_size_xz; z++)
                values[size_t(x + z * size_xz)] = filler->compute(Ctx{(chunk->first_noise_x + x) * 4, 0, (chunk->first_noise_z + z) * 4, nullptr});
    }
    double compute(const Ctx& c) const override {
        int x = (c.x >> 2) - chunk->first_noise_x, z = (c.z >> 2) - chunk->first_noise_z;
        return x >= 0 && z >= 0 && x < size_xz && z < size_xz ? values[size_t(x + z * size_xz)] : filler->compute(c);
    }
};

struct Cache2D : Node {
    NodeP filler;
    mutable int64_t last_pos = INT64_MIN;
    mutable double last_value = 0;
    double compute(const Ctx& c) const override {
        int64_t pos = (int64_t(c.x) << 32) | uint32_t(c.z);
        if (last_pos == pos) return last_value;
        last_pos = pos;
        last_value = filler->compute(c);
        return last_value;
    }
};

struct CacheOnce : Node {
    ChunkNoise* chunk;
    NodeP filler;
    mutable uint64_t epoch = 0;
    mutable int lx = 0, ly = 0, lz = 0;
    mutable double value = 0;
    double compute(const Ctx& c) const override {
        if (c.nc != chunk) return filler->compute(c);
        if (epoch == chunk->epoch && lx == c.x && ly == c.y && lz == c.z) return value;
        epoch = chunk->epoch; lx = c.x; ly = c.y; lz = c.z;
        value = filler->compute(c);
        return value;
    }
};

}  // namespace

// ---- interpolator and cell cache ----------------------------------------------------------------------------------

InterpolatorNode::InterpolatorNode(ChunkNoise* c, NodeP f) : chunk(c), filler(std::move(f)) {
    slice0.assign(size_t(chunk->cell_count_xz + 1), std::vector<double>(size_t(chunk->cell_count_y + 1), 0.0));
    slice1 = slice0;
    chunk->interpolators.push_back(this);
}

double InterpolatorNode::compute(const Ctx& ctx) const {
    if (ctx.nc != chunk) return filler->compute(ctx);
    if (chunk->filling_cell)
        return lerp3(double(chunk->in_cell_x) / chunk->cell_width, double(chunk->in_cell_y) / chunk->cell_height, double(chunk->in_cell_z) / chunk->cell_width,
                     n000, n100, n010, n110, n001, n101, n011, n111);
    return value;
}

void InterpolatorNode::fill_slice(bool s0, int zi) const {
    std::vector<double>& slice = (s0 ? slice0 : slice1)[size_t(zi)];
    for (int yi = 0; yi <= chunk->cell_count_y; yi++) {
        chunk->epoch++;
        Ctx c{chunk->cell_start_x, (yi + chunk->cell_noise_min_y) * chunk->cell_height, chunk->cell_start_z, chunk};
        slice[size_t(yi)] = filler->compute(c);
    }
}

void InterpolatorNode::select_cell_yz(int cy, int cz) const {
    n000 = slice0[size_t(cz)][size_t(cy)];
    n001 = slice0[size_t(cz) + 1][size_t(cy)];
    n100 = slice1[size_t(cz)][size_t(cy)];
    n101 = slice1[size_t(cz) + 1][size_t(cy)];
    n010 = slice0[size_t(cz)][size_t(cy) + 1];
    n011 = slice0[size_t(cz) + 1][size_t(cy) + 1];
    n110 = slice1[size_t(cz)][size_t(cy) + 1];
    n111 = slice1[size_t(cz) + 1][size_t(cy) + 1];
}

void InterpolatorNode::update_for_y(double f) const {
    vxz00 = lerp(f, n000, n010);
    vxz10 = lerp(f, n100, n110);
    vxz01 = lerp(f, n001, n011);
    vxz11 = lerp(f, n101, n111);
}
void InterpolatorNode::update_for_x(double f) const {
    vz0 = lerp(f, vxz00, vxz10);
    vz1 = lerp(f, vxz01, vxz11);
}
void InterpolatorNode::update_for_z(double f) const { value = lerp(f, vz0, vz1); }

CacheAllInCellNode::CacheAllInCellNode(ChunkNoise* c, NodeP f) : chunk(c), filler(std::move(f)) {
    values.assign(size_t(chunk->cell_width * chunk->cell_width * chunk->cell_height), 0.0);
    chunk->cell_caches.push_back(this);
}

double CacheAllInCellNode::compute(const Ctx& ctx) const {
    if (ctx.nc != chunk) return filler->compute(ctx);
    int x = chunk->in_cell_x, y = chunk->in_cell_y, z = chunk->in_cell_z;
    if (x >= 0 && y >= 0 && z >= 0 && x < chunk->cell_width && y < chunk->cell_height && z < chunk->cell_width)
        return values[size_t(((chunk->cell_height - 1 - y) * chunk->cell_width + x) * chunk->cell_width + z)];
    return filler->compute(ctx);
}

void CacheAllInCellNode::fill_cell() const {
    size_t i = 0;
    for (int y = chunk->cell_height - 1; y >= 0; y--) {
        chunk->in_cell_y = y;
        for (int x = 0; x < chunk->cell_width; x++) {
            chunk->in_cell_x = x;
            for (int z = 0; z < chunk->cell_width; z++) {
                chunk->in_cell_z = z;
                chunk->epoch++;
                values[i++] = filler->compute(chunk->here());
            }
        }
    }
}

// ---- compiler ------------------------------------------------------------------------------------------------------

NodeP Compiler::constant(double v) { return std::make_shared<Constant>(v); }

NodeP Compiler::reference(const std::string& id0) {
    std::string id = id0.find(':') == std::string::npos ? "minecraft:" + id0 : id0;
    auto it = refs_.find(id);
    if (it != refs_.end()) return it->second;
    NodeP n = compile(state_.registry().get("density_function", id));
    refs_[id] = n;
    return n;
}

NodeP Compiler::compile(const Json& j) {
    if (j.is_num()) return constant(j.number());
    if (j.is_str()) return reference(j.str);
    return object(j);
}

NodeP Compiler::object(const Json& j) {
    std::string type = j.get_string("type");
    if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
    auto arg = [&](const char* key) { return compile(j.at(key)); };
    auto noise_of = [&](const char* key) { return state_.noise(j.at(key).str); };
    if (type == "constant") return constant(j.at("argument").number());
    if (type == "add" || type == "mul" || type == "min" || type == "max") {
        NodeP a = arg("argument1"), b = arg("argument2");
        bool is_add = type == "add", is_mul = type == "mul";
        if (is_add || is_mul) {
            auto ca = std::dynamic_pointer_cast<Constant>(a), cb = std::dynamic_pointer_cast<Constant>(b);
            if (ca || cb) {
                auto n = std::make_shared<MulOrAdd>();
                n->is_mul = is_mul;
                n->input = ca ? b : a;
                n->arg = ca ? ca->v : cb->v;
                return n;
            }
        }
        auto n = std::make_shared<Ap2>();
        n->kind = is_add ? Ap2::Add : is_mul ? Ap2::Mul : type == "min" ? Ap2::Min : Ap2::Max;
        n->a = a;
        n->b = b;
        return n;
    }
    static const std::map<std::string, Mapped::Kind> mapped = {{"abs", Mapped::Abs}, {"square", Mapped::Square}, {"cube", Mapped::Cube}, {"half_negative", Mapped::HalfNegative},
                                                               {"quarter_negative", Mapped::QuarterNegative}, {"invert", Mapped::Invert}, {"squeeze", Mapped::Squeeze}};
    if (auto it = mapped.find(type); it != mapped.end()) {
        auto n = std::make_shared<Mapped>();
        n->kind = it->second;
        n->input = arg("argument");
        return n;
    }
    if (type == "clamp") {
        auto n = std::make_shared<Clamp>();
        n->input = arg("input");
        n->lo = j.at("min").number();
        n->hi = j.at("max").number();
        return n;
    }
    if (type == "noise") {
        auto n = std::make_shared<NoiseNode>();
        n->noise = noise_of("noise");
        n->xz_scale = j.at("xz_scale").number();
        n->y_scale = j.at("y_scale").number();
        return n;
    }
    if (type == "shifted_noise") {
        auto n = std::make_shared<ShiftedNoise>();
        n->sx = arg("shift_x");
        n->sy = arg("shift_y");
        n->sz = arg("shift_z");
        n->noise = noise_of("noise");
        n->xz_scale = j.at("xz_scale").number();
        n->y_scale = j.at("y_scale").number();
        return n;
    }
    if (type == "shift_a" || type == "shift_b" || type == "shift") {
        auto n = std::make_shared<ShiftNode>();
        n->kind = type == "shift_a" ? ShiftNode::A : type == "shift_b" ? ShiftNode::B : ShiftNode::Both;
        n->noise = state_.noise(j.at("argument").str);
        return n;
    }
    if (type == "y_clamped_gradient") {
        auto n = std::make_shared<YClampedGradient>();
        n->from_y = int(j.at("from_y").int_value());
        n->to_y = int(j.at("to_y").int_value());
        n->from_v = j.at("from_value").number();
        n->to_v = j.at("to_value").number();
        return n;
    }
    if (type == "range_choice") {
        auto n = std::make_shared<RangeChoice>();
        n->input = arg("input");
        n->lo = j.at("min_inclusive").number();
        n->hi = j.at("max_exclusive").number();
        n->in_range = arg("when_in_range");
        n->out_range = arg("when_out_of_range");
        return n;
    }
    if (type == "weird_scaled_sampler") {
        auto n = std::make_shared<WeirdScaledSampler>();
        n->input = arg("input");
        n->noise = noise_of("noise");
        n->type2 = j.at("rarity_value_mapper").str == "type_2";
        return n;
    }
    if (type == "interval_select") {
        auto n = std::make_shared<IntervalSelect>();
        n->input = arg("input");
        for (auto& t : j.at("thresholds").arr) n->thresholds.push_back(t.number());
        for (auto& f : j.at("functions").arr) n->functions.push_back(compile(f));
        return n;
    }
    if (type == "find_top_surface") {
        auto n = std::make_shared<FindTopSurface>();
        n->density = arg("density");
        n->upper = arg("upper_bound");
        n->lower = int(j.at("lower_bound").int_value());
        n->cell_height = int(j.at("cell_height").int_value());
        return n;
    }
    if (type == "spline") {
        std::function<std::unique_ptr<SplineNode>(const Json&)> build = [&](const Json& sj) -> std::unique_ptr<SplineNode> {
            auto s = std::make_unique<SplineNode>();
            if (sj.is_num()) { s->is_constant = true; s->constant = sj.float_value(); return s; }
            s->coordinate = compile(sj.at("coordinate"));
            for (auto& p : sj.at("points").arr) {
                s->locations.push_back(p.at("location").float_value());
                s->derivatives.push_back(p.at("derivative").float_value());
                const Json& v = p.at("value");
                if (v.is_num()) { s->subs.push_back(nullptr); s->constants.push_back(v.float_value()); }
                else { s->subs.push_back(build(v)); s->constants.push_back(0); }
            }
            return s;
        };
        auto n = std::make_shared<SplineDensity>();
        n->spline = build(j.at("spline"));
        return n;
    }
    if (type == "old_blended_noise") {
        auto n = std::make_shared<OldBlended>();
        auto r = state_.terrain_random();
        n->noise = std::make_unique<BlendedNoise>(*r, j.at("xz_scale").number(), j.at("y_scale").number(), j.at("xz_factor").number(), j.at("y_factor").number(), j.at("smear_scale_multiplier").number());
        return n;
    }
    if (type == "end_islands") {
        auto n = std::make_shared<EndIslands>();
        LegacyRandom r(state_.seed());
        r.consume_count(17292);
        n->noise = std::make_unique<SimplexNoise>(r);
        return n;
    }
    if (type == "blend_alpha") return constant(1.0);
    if (type == "blend_offset") return constant(0.0);
    if (type == "blend_density") return arg("argument");
    if (type == "beardifier") return chunk_ ? std::make_shared<BeardifierNode>(chunk_) : constant(0.0);
    if (type == "interpolated") {
        NodeP f = arg("argument");
        return chunk_ ? std::make_shared<InterpolatorNode>(chunk_, f) : f;
    }
    if (type == "flat_cache") {
        NodeP f = arg("argument");
        return chunk_ ? std::make_shared<FlatCache>(chunk_, f) : f;
    }
    if (type == "cache_2d") {
        NodeP f = arg("argument");
        if (!chunk_) return f;
        auto n = std::make_shared<Cache2D>();
        n->filler = f;
        return n;
    }
    if (type == "cache_once") {
        NodeP f = arg("argument");
        if (!chunk_) return f;
        auto n = std::make_shared<CacheOnce>();
        n->chunk = chunk_;
        n->filler = f;
        return n;
    }
    if (type == "cache_all_in_cell") {
        NodeP f = arg("argument");
        return chunk_ ? std::make_shared<CacheAllInCellNode>(chunk_, f) : f;
    }
    throw std::runtime_error("unknown density function type " + type);
}

double BeardifierNode::compute(const Ctx& c) const { return chunk && chunk->beardifier ? chunk->beardifier->compute(c.x, c.y, c.z) : 0.0; }

}  // namespace pn::vg
