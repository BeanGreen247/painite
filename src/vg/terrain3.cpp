#include "terrain3.hpp"
#include "biomezoom.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace pn::vg::v3 {

namespace {

constexpr int kWayBelowMinY = -32512;   // DimensionType.WAY_BELOW_MIN_Y

inline int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline int jfloor(double v) { return int(std::floor(v)); }
inline int64_t pack_xz(int x, int z) { return int64_t(uint32_t(x)) | (int64_t(uint32_t(z)) << 32); }

double mth_lerp_d(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
double mth_clamped_lerp(double f, double lo, double hi) { return f < 0.0 ? lo : f > 1.0 ? hi : mth_lerp_d(f, lo, hi); }
double mth_inverse_lerp(double v, double lo, double hi) { return (v - lo) / (hi - lo); }
double mth_map(double v, double a, double b, double c, double d) { return mth_lerp_d(mth_inverse_lerp(v, a, b), c, d); }
double mth_clamped_map(double v, double a, double b, double c, double d) { return mth_clamped_lerp(mth_inverse_lerp(v, a, b), c, d); }
double mth_clamp(double v, double lo, double hi) { return v < lo ? lo : std::min(v, hi); }
int mth_quantize(double v, int q) { return jfloor(v / double(q)) * q; }

// ---------------------------------------------------------------------------------------------------------------- biome temperature

struct BiomeNoises {
    std::unique_ptr<SimplexNoise> temperature, info;
    std::unique_ptr<NoiseStack> frozen;
    BiomeNoises() {
        { LegacyRandom r(1234); temperature = std::make_unique<SimplexNoise>(r, true); }
        {
            LegacyRandom r(3456);
            frozen = std::make_unique<NoiseStack>();
            frozen->add(std::make_shared<SimplexNoise>(r, true), 1.0, 0.14285715f);
            frozen->add(std::make_shared<SimplexNoise>(r, true), 0.5, 0.2857143f);
            frozen->add(std::make_shared<SimplexNoise>(r, true), 0.25, 0.5714286f);
        }
        { LegacyRandom r(2345); info = std::make_unique<SimplexNoise>(r, true); }
    }
};
const BiomeNoises& biome_noises() { static BiomeNoises n; return n; }

}  // namespace

float biome_temperature3(const BiomeData3& b, int x, int y, int z, int sea_level) {
    const BiomeNoises& N = biome_noises();
    float t = b.temperature;
    if (b.frozen) {
        double large = double(N.frozen->get(double(x) * 0.05, double(z) * 0.05) * 7.0f);
        double edge = double(N.info->get(double(x) * 0.2, double(z) * 0.2));
        if (large + edge < 0.3 && double(N.info->get(double(x) * 0.09, double(z) * 0.09)) < 0.8) t = 0.2f;
    }
    int snow_level = sea_level + 17;
    if (y > snow_level) {
        float v = N.temperature->get(double(float(x) / 8.0f), double(float(z) / 8.0f)) * 8.0f;
        return t - (v + float(y) - float(snow_level)) * 0.05f / 40.0f;
    }
    return t;
}

// ---------------------------------------------------------------------------------------------------------------- aquifer

namespace {

struct FluidStatus {
    int level = 0;
    uint16_t type = 0;
    bool operator==(const FluidStatus& o) const { return level == o.level && type == o.type; }
};

struct AquiferCfg {
    const Sampler *barrier = nullptr, *floodedness = nullptr, *spread = nullptr, *lava = nullptr, *exclusion = nullptr, *surface = nullptr;
};

}  // namespace


struct NoiseChunk3 {
    Volume vol;
    SamplerCtx ctx;
    // the aquifer (Aquifer.NoiseBasedAquifer, or the disabled one)
    bool enabled = false;
    bool schedule = false;
    const PosFactory* rand = nullptr;
    AquiferCfg cfg;
    std::function<FluidStatus(int, int, int)> picker;
    uint16_t air = 0, water = 0, lava = 0;
    int skip_above_y = 0, min_gx = 0, min_gy = 0, min_gz = 0, gsx = 0, gsz = 0;
    std::vector<FluidStatus> status;
    std::vector<uint8_t> status_ok;
    struct Loc { int x, y, z; };
    std::vector<Loc> loc;
    std::vector<uint8_t> loc_ok;
    std::unordered_map<int64_t, int> surface_cache;

    static int gx(int b) { return b >> 4; }
    static int gy(int b) { return floor_div(b, 12); }
    static int gz(int b) { return b >> 4; }
    static int from_gx(int g, int o) { return (g << 4) + o; }
    static int from_gy(int g, int o) { return g * 12 + o; }
    static int from_gz(int g, int o) { return (g << 4) + o; }
    static int adjust(int s) { return s + 8; }

    int surface_level(int bx, int bz) {
        int qx = (bx >> 2) << 2, qz = (bz >> 2) << 2;
        auto key = pack_xz(qx, qz);
        auto it = surface_cache.find(key);
        if (it != surface_cache.end()) return it->second;
        int v = jfloor(double(cfg.surface->value(ctx, qx, 0, qz)));
        surface_cache.emplace(key, v);
        return v;
    }

    int max_surface_level(int min_bx, int min_bz, int max_bx, int max_bz) {
        int minq = min_bx >> 2, maxq = max_bx >> 2, minqz = min_bz >> 2, maxqz = max_bz >> 2;
        Volume v;
        v.sx = maxq - minq + 1; v.sy = 1; v.sz = maxqz - minqz + 1;
        v.minx = minq << 2; v.miny = 0; v.minz = minqz << 2;
        v.stepx = 4; v.stepy = 1; v.stepz = 4;
        Buf buf(ctx, size_t(v.size()));
        cfg.surface->volume(ctx, buf.data(), v);
        int max_y = INT_MIN;
        for (int z = 0; z < v.sz; z++)
            for (int x = 0; x < v.sx; x++) {
                int s = jfloor(double(buf[size_t(v.index(x, 0, z))]));
                surface_cache[pack_xz(v.block_x(x), v.block_z(z))] = s;
                if (s > max_y) max_y = s;
            }
        return max_y;
    }

    void init_aquifer(const Volume& volume) {
        min_gx = gx(volume.minx - 5);
        int max_gx = gx(volume.max_block_x() - 5) + 1;
        gsx = max_gx - min_gx + 1;
        min_gy = gy(volume.miny + 1) - 1;
        int max_gy = gy(volume.max_block_y() + 1) + 1;
        int gsy = max_gy - min_gy + 1;
        min_gz = gz(volume.minz - 5);
        int max_gz = gz(volume.max_block_z() - 5) + 1;
        gsz = max_gz - min_gz + 1;
        size_t total = size_t(gsx) * size_t(gsy) * size_t(gsz);
        status.assign(total, FluidStatus{});
        status_ok.assign(total, 0);
        loc.assign(total, Loc{0, 0, 0});
        loc_ok.assign(total, 0);
        int max_adj = adjust(max_surface_level(from_gx(min_gx, 0), from_gz(min_gz, 0), from_gx(max_gx, 9), from_gz(max_gz, 9)));
        int skip_gy = gy(max_adj + 12) + 1;
        skip_above_y = from_gy(skip_gy, 11) - 1;
    }

    size_t index(int gxx, int gyy, int gzz) const { return (size_t(gyy - min_gy) * size_t(gsz) + size_t(gzz - min_gz)) * size_t(gsx) + size_t(gxx - min_gx); }

    static double similarity(int d1, int d2) { return 1.0 - double(d2 - d1) / 25.0; }

    uint16_t at(const FluidStatus& s, int y) const { return y < s.level ? s.type : air; }

    double pressure(int bx, int by, int bz, double& barrier_value, const FluidStatus& s1, const FluidStatus& s2) {
        uint16_t t1 = at(s1, by), t2 = at(s2, by);
        if ((t1 == lava && t2 == water) || (t1 == water && t2 == lava)) return 2.0;
        int diff = std::abs(s1.level - s2.level);
        if (diff == 0) return 0.0;
        double avg = 0.5 * double(s1.level + s2.level);
        double above = double(by) + 0.5 - avg;
        double base = double(diff) / 2.0;
        double dist = base - std::abs(above);
        double grad;
        if (above > 0.0) {
            double c = 0.0 + dist;
            grad = c > 0.0 ? c / 1.5 : c / 2.5;
        } else {
            double c = 3.0 + dist;
            grad = c > 0.0 ? c / 3.0 : c / 10.0;
        }
        double noise_value;
        if (grad < -2.0 || grad > 2.0) {
            noise_value = 0.0;
        } else {
            if (std::isnan(barrier_value)) barrier_value = double(cfg.barrier->value(ctx, bx, by, bz));
            noise_value = barrier_value;
        }
        return 2.0 * (noise_value + grad);
    }

    FluidStatus compute_fluid(int x, int y, int z) {
        FluidStatus global = picker(x, y, z);
        int lowest = INT_MAX;
        int top = y + 12, bottom = y - 12;
        bool center_under_global = false;
        static const int kOff[13][2] = {{0, 0}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0}, {-1, 0}, {1, 0}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}};
        for (auto& o : kOff) {
            int sx = x + o[0] * 16, sz = z + o[1] * 16;
            int surf = surface_level(sx, sz);
            int adj = adjust(surf);
            bool start = o[0] == 0 && o[1] == 0;
            if (start && bottom > adj) return global;
            bool pokes = top > adj;
            if (pokes || start) {
                FluidStatus g = picker(sx, adj, sz);
                if (at(g, adj) != air) {
                    if (start) center_under_global = true;
                    if (pokes) return g;
                }
            }
            lowest = std::min(lowest, surf);
        }
        int fluid_surface = compute_surface_level(x, y, z, global, lowest, center_under_global);
        return FluidStatus{fluid_surface, compute_fluid_type(x, y, z, global, fluid_surface)};
    }

    int compute_surface_level(int x, int y, int z, const FluidStatus& global, int lowest, bool center_under_global) {
        double fully, partially;
        if (double(cfg.exclusion->value(ctx, x, y, z)) > 0.0) {
            partially = -1.0;
            fully = -1.0;
        } else {
            int below = adjust(lowest) - y;
            double factor = center_under_global ? mth_clamped_map(double(below), 0.0, 64.0, 1.0, 0.0) : 0.0;
            double noise = mth_clamp(double(cfg.floodedness->value(ctx, x, y, z)), -1.0, 1.0);
            double fully_thr = mth_map(factor, 1.0, 0.0, -0.3, 0.8);
            double partial_thr = mth_map(factor, 1.0, 0.0, -0.8, 0.4);
            partially = noise - partial_thr;
            fully = noise - fully_thr;
        }
        return fully > 0.0 ? global.level : (partially > 0.0 ? randomized_surface(x, y, z, lowest) : kWayBelowMinY);
    }

    int randomized_surface(int x, int y, int z, int lowest) {
        int cx = floor_div(x, 16), cy = floor_div(y, 40), cz = floor_div(z, 16);
        int middle = cy * 40 + 20;
        double spread = double(cfg.spread->value(ctx, cx, cy, cz) * 10.0f);
        int q = mth_quantize(spread, 3);
        return std::min(lowest, middle + q);
    }

    uint16_t compute_fluid_type(int x, int y, int z, const FluidStatus& global, int fluid_surface) {
        uint16_t type = global.type;
        if (fluid_surface <= -10 && fluid_surface != kWayBelowMinY && global.type != lava) {
            int cx = floor_div(x, 64), cy = floor_div(y, 40), cz = floor_div(z, 64);
            double v = double(cfg.lava->value(ctx, cx, cy, cz));
            if (std::abs(v) > 0.3) type = lava;
        }
        return type;
    }

    const FluidStatus& get_status(size_t i) {
        if (!status_ok[i]) {
            status[i] = compute_fluid(loc[i].x, loc[i].y, loc[i].z);
            status_ok[i] = 1;
        }
        return status[i];
    }

    // Aquifer.computeSubstance: -1 for null
    int substance(int bx, int by, int bz, double density) {
        if (density > 0.0) { schedule = false; return -1; }
        FluidStatus global = picker(bx, by, bz);
        if (!enabled) { schedule = false; return at(global, by); }
        if (by > skip_above_y) { schedule = false; return at(global, by); }
        if (at(global, by) == lava) { schedule = false; return lava; }
        int xa = gx(bx - 5), ya = gy(by + 1), za = gz(bz - 5);
        int d1 = INT_MAX, d2 = INT_MAX, d3 = INT_MAX, d4 = INT_MAX;
        size_t i1 = 0, i2 = 0, i3 = 0, i4 = 0;
        for (int x1 = 0; x1 <= 1; x1++)
            for (int y1 = -1; y1 <= 1; y1++)
                for (int z1 = 0; z1 <= 1; z1++) {
                    int sgx = xa + x1, sgy = ya + y1, sgz = za + z1;
                    size_t idx = index(sgx, sgy, sgz);
                    if (!loc_ok[idx]) {
                        auto r = rand->at(sgx, sgy, sgz);
                        int ox = r->next_int(10), oy = r->next_int(9), oz = r->next_int(10);
                        loc[idx] = Loc{from_gx(sgx, ox), from_gy(sgy, oy), from_gz(sgz, oz)};
                        loc_ok[idx] = 1;
                    }
                    const Loc& l = loc[idx];
                    int dx = l.x - bx, dy = l.y - by, dz = l.z - bz;
                    int nd = dx * dx + dy * dy + dz * dz;
                    if (d1 >= nd) { i4 = i3; i3 = i2; i2 = i1; i1 = idx; d4 = d3; d3 = d2; d2 = d1; d1 = nd; }
                    else if (d2 >= nd) { i4 = i3; i3 = i2; i2 = idx; d4 = d3; d3 = d2; d2 = nd; }
                    else if (d3 >= nd) { i4 = i3; i3 = idx; d4 = d3; d3 = nd; }
                    else if (d4 >= nd) { i4 = idx; d4 = nd; }
                }
        FluidStatus s1 = get_status(i1);
        double sim12 = similarity(d1, d2);
        uint16_t fluid_state = at(s1, by);
        const double kFlow = similarity(100, 144);
        if (sim12 <= 0.0) {
            if (sim12 >= kFlow) { FluidStatus s2 = get_status(i2); schedule = !(s1 == s2); }
            else schedule = false;
            return fluid_state;
        }
        if (fluid_state == water && at(picker(bx, by - 1, bz), by - 1) == lava) { schedule = true; return fluid_state; }
        double barrier_value = std::nan("");
        FluidStatus s2 = get_status(i2);
        double b12 = sim12 * pressure(bx, by, bz, barrier_value, s1, s2);
        if (density + b12 > 0.0) { schedule = false; return -1; }
        FluidStatus s3 = get_status(i3);
        double sim13 = similarity(d1, d3);
        if (sim13 > 0.0) {
            double b13 = sim12 * sim13 * pressure(bx, by, bz, barrier_value, s1, s3);
            if (density + b13 > 0.0) { schedule = false; return -1; }
        }
        double sim23 = similarity(d2, d3);
        if (sim23 > 0.0) {
            double b23 = sim12 * sim23 * pressure(bx, by, bz, barrier_value, s2, s3);
            if (density + b23 > 0.0) { schedule = false; return -1; }
        }
        bool may12 = !(s1 == s2);
        bool may23 = sim23 >= kFlow && !(s2 == s3);
        bool may13 = sim13 >= kFlow && !(s1 == s3);
        if (may12 || may23 || may13) schedule = true;
        else schedule = sim13 >= kFlow && similarity(d1, d4) >= kFlow && !(s1 == get_status(i4));
        return fluid_state;
    }
};

// ---------------------------------------------------------------------------------------------------------------- material rules

namespace {

struct MCtx;

struct MCond {
    virtual ~MCond() = default;
    virtual bool test(MCtx& c) const = 0;
};
struct MRule {
    virtual ~MRule() = default;
    virtual int apply(MCtx& c, int x, int y, int z) const = 0;   // block id or -1
};

}  // namespace

struct Terrain3::Impl {
    Terrain3& T;
    std::vector<std::unique_ptr<MCond>> conds;
    std::vector<std::unique_ptr<MRule>> rules;
    std::map<std::string, const MRule*> rule_by_id;
    std::map<std::string, const MCond*> cond_by_id;
    const MRule* root = nullptr;
    int node_count = 0;
    int64_t zoom_seed = 0;
    uint16_t default_block = 0, default_fluid = 0, lava = 0, water = 0, air = 0;
    uint16_t white_t, orange_t, terracotta, yellow_t, brown_t, red_t, light_gray_t, packed_ice, snow_block;
    std::vector<uint16_t> clay_bands;
    std::shared_ptr<Noise> clay_offset, surface_noise, surface_secondary, pillar, pillar_roof, badlands_surface, iceberg_pillar, iceberg_roof, iceberg_surface;
    PosFactory* noise_random = nullptr;
    const Sampler* final_density = nullptr;
    const Sampler* prelim_surface = nullptr;   // router chunk_surface_level
    AquiferCfg aq;
    std::string rule_name;
    std::map<std::string, int> noise_slots;
    int eroded_badlands = -1;
    std::vector<int> frozen_ocean;
    bool legacy = false;
    explicit Impl(Terrain3& t) : T(t) {}
    const MRule* parse_rule(const Json& j);
    const MCond* parse_cond(const Json& j);
};

namespace {

// MaterialRuleContext
struct MCtx {
    const Terrain3::Impl* sys = nullptr;
    const Terrain3* T = nullptr;
    NoiseChunk3* nc = nullptr;
    const ChunkBlocks* blocks = nullptr;
    const std::function<int(int, int, int)>* quart_biome = nullptr;
    Volume expected, prelim_vol;
    std::vector<float> prelim_buf;
    bool prelim_ready = false;
    int64_t xz_epoch = 1, y_epoch = 1;
    int bx = 0, bz = 0, by = 0, grad_x = 0, grad_z = 0, surface_depth = 0;
    int64_t sec_epoch = 0, minsurf_epoch = 0;
    double secondary = 0;
    int min_surface = 0;
    int biome = -1;
    bool have_biome = false;
    int water_height = INT_MIN, stone_below = 0, stone_above = 0;
    struct Lazy { int64_t epoch = INT64_MIN; bool result = false; };
    std::vector<Lazy> lazy;
    struct NoiseCache { int64_t epoch = INT64_MIN; double value = 0; };
    std::vector<NoiseCache> noise_cache;
    struct Prefill { bool ready = false; std::vector<float> density, richness; };
    std::vector<Prefill> prefill;

    void update_xz(int x, int z, int gx, int gz);
    void update_y(int above, int below, int water, int y) {
        ++y_epoch;
        have_biome = false;
        by = y; water_height = water; stone_below = below; stone_above = above;
    }
    int get_biome() {
        if (!have_biome) { biome = T->biome_at(bx, by, bz, *quart_biome); have_biome = true; }
        return biome;
    }
    double get_secondary();
    int get_min_surface_level();
};

}  // namespace

// ---- conditions and rules (the SurfaceRules of 26.3 as data)

namespace {

struct LazyY : MCond {
    int id;
    explicit LazyY(int i) : id(i) {}
    bool test(MCtx& c) const final {
        auto& l = c.lazy[size_t(id)];
        if (l.epoch == c.y_epoch) return l.result;
        l.epoch = c.y_epoch;
        l.result = compute(c);
        return l.result;
    }
    virtual bool compute(MCtx& c) const = 0;
};
struct LazyXZ : MCond {
    int id;
    explicit LazyXZ(int i) : id(i) {}
    bool test(MCtx& c) const final {
        auto& l = c.lazy[size_t(id)];
        if (l.epoch == c.xz_epoch) return l.result;
        l.epoch = c.xz_epoch;
        l.result = compute(c);
        return l.result;
    }
    virtual bool compute(MCtx& c) const = 0;
};

int resolve_anchor(const Json& a, int min_y, int height) {
    if (a.has("absolute")) return int(a.at("absolute").int_value());
    if (a.has("above_bottom")) return min_y + int(a.at("above_bottom").int_value());
    if (a.has("below_top")) return height - 1 + min_y - int(a.at("below_top").int_value());
    throw std::runtime_error("unknown vertical anchor");
}

struct StoneDepth : LazyY {
    int offset, range; bool add_surface, ceiling;
    StoneDepth(int i, int o, bool a, int r, bool c) : LazyY(i), offset(o), range(r), add_surface(a), ceiling(c) {}
    bool compute(MCtx& c) const override {
        int depth = ceiling ? c.stone_below : c.stone_above;
        int surf = add_surface ? c.surface_depth : 0;
        int sec = range == 0 ? 0 : int(mth_map(c.get_secondary(), -1.0, 1.0, 0.0, double(range)));
        return depth <= 1 + offset + surf + sec;
    }
};
struct YAbove : LazyY {
    int anchor, mult; bool add_stone;
    YAbove(int i, int a, int m, bool s) : LazyY(i), anchor(a), mult(m), add_stone(s) {}
    bool compute(MCtx& c) const override { return c.by + (add_stone ? c.stone_above : 0) >= anchor + c.surface_depth * mult; }
};
struct WaterCond : LazyY {
    int offset, mult; bool add_stone;
    WaterCond(int i, int o, int m, bool s) : LazyY(i), offset(o), mult(m), add_stone(s) {}
    bool compute(MCtx& c) const override {
        return c.water_height == INT_MIN || c.by + (add_stone ? c.stone_above : 0) >= c.water_height + offset + c.surface_depth * mult;
    }
};
struct BiomeCond : LazyY {
    std::set<int> biomes;
    BiomeCond(int i, std::set<int> b) : LazyY(i), biomes(std::move(b)) {}
    bool compute(MCtx& c) const override { return biomes.count(c.get_biome()) != 0; }
};
struct NoiseThreshold : MCond {
    int slot; std::shared_ptr<Noise> noise; double lo, hi; bool d3;
    NoiseThreshold(int s, std::shared_ptr<Noise> n, double a, double b, bool t) : slot(s), noise(std::move(n)), lo(a), hi(b), d3(t) {}
    bool test(MCtx& c) const override {
        auto& nc = c.noise_cache[size_t(slot)];
        int64_t epoch = d3 ? c.y_epoch : c.xz_epoch;
        if (nc.epoch != epoch) { nc.value = double(noise->get(double(c.bx), d3 ? double(c.by) : 0.0, double(c.bz))); nc.epoch = epoch; }
        return nc.value >= lo && nc.value <= hi;
    }
};
struct VerticalGradient : LazyY {
    int true_at, false_at; const PosFactory* factory;
    VerticalGradient(int i, int t, int f, const PosFactory* p) : LazyY(i), true_at(t), false_at(f), factory(p) {}
    bool compute(MCtx& c) const override {
        int y = c.by;
        if (y <= true_at) return true;
        if (y >= false_at) return false;
        double p = mth_map(double(y), double(true_at), double(false_at), 1.0, 0.0);
        auto r = factory->at(c.bx, y, c.bz);
        return double(r->next_float()) < p;
    }
};
struct Steep : LazyXZ {
    using LazyXZ::LazyXZ;
    bool compute(MCtx& c) const override { return c.grad_x <= -4 || c.grad_z >= 4; }
};
struct Hole : LazyXZ {
    using LazyXZ::LazyXZ;
    bool compute(MCtx& c) const override { return c.surface_depth <= 0; }
};
struct AbovePrelim : MCond {
    bool test(MCtx& c) const override { return c.by >= c.get_min_surface_level(); }
};
struct Temperature : MCond {
    bool test(MCtx& c) const override {
        const auto& b = c.T->biomes()[size_t(c.get_biome())];
        return !(biome_temperature3(b, c.bx, c.by, c.bz, c.T->settings().sea_level) >= 0.15f);
    }
};
struct Not : MCond {
    const MCond* in;
    explicit Not(const MCond* i) : in(i) {}
    bool test(MCtx& c) const override { return !in->test(c); }
};

struct BlockRule : MRule {
    int state;
    explicit BlockRule(int s) : state(s) {}
    int apply(MCtx&, int, int, int) const override { return state; }
};
struct CondRule : MRule {
    const MCond* cond; const MRule* then;
    CondRule(const MCond* c, const MRule* t) : cond(c), then(t) {}
    int apply(MCtx& c, int x, int y, int z) const override { return cond->test(c) ? then->apply(c, x, y, z) : -1; }
};
struct SeqRule : MRule {
    std::vector<const MRule*> seq;
    int apply(MCtx& c, int x, int y, int z) const override {
        for (const MRule* r : seq) { int s = r->apply(c, x, y, z); if (s >= 0) return s; }
        return -1;
    }
};
struct BandRule : MRule {
    const Terrain3::Impl* sys;
    explicit BandRule(const Terrain3::Impl* s) : sys(s) {}
    int apply(MCtx& c, int x, int y, int z) const override;
};
struct OreVein : MRule {
    int slot; const Sampler *density, *richness, *gap; uint16_t ore, raw, filler; float raw_chance; const PosFactory* factory;
    int apply(MCtx& c, int x, int y, int z) const override;
};

}  // namespace

const MRule* Terrain3::Impl::parse_rule(const Json& j) {
    if (j.is_str()) {
        std::string id = j.str;
        auto it = rule_by_id.find(id);
        if (it != rule_by_id.end()) return it->second;
        const MRule* r = parse_rule(T.reg_.get("material_rule", id));
        rule_by_id[id] = r;
        return r;
    }
    std::string type = j.at("type").str;
    auto keep = [&](std::unique_ptr<MRule> p) { rules.push_back(std::move(p)); return rules.back().get(); };
    if (type == "minecraft:block") {
        const Json& s = j.at("result_state");
        int id = s.is_str() ? int(T.bd_.default_state(s.str)) : int(T.bd_.state(s));
        return keep(std::make_unique<BlockRule>(id));
    }
    if (type == "minecraft:condition") return keep(std::make_unique<CondRule>(parse_cond(j.at("if_true")), parse_rule(j.at("then_run"))));
    if (type == "minecraft:sequence") {
        auto s = std::make_unique<SeqRule>();
        for (auto& e : j.at("sequence").arr) s->seq.push_back(parse_rule(e));
        if (s->seq.size() == 1) return s->seq[0];
        return keep(std::move(s));
    }
    if (type == "minecraft:bandlands") return keep(std::make_unique<BandRule>(this));
    if (type == "minecraft:ore_vein") {
        auto o = std::make_unique<OreVein>();
        o->slot = node_count++;
        o->density = T.comp_->sampler(j.at("density"));
        o->richness = T.comp_->sampler(j.at("richness"));
        o->gap = T.comp_->sampler(j.at("filler_gap"));
        o->ore = T.bd_.default_state(j.at("ore_block").str);
        o->raw = T.bd_.default_state(j.at("raw_ore_block").str);
        o->filler = T.bd_.default_state(j.at("filler_block").str);
        o->raw_chance = j.at("raw_ore_chance").float_value();
        o->factory = &T.rs_->random_factory("minecraft:ore");
        return keep(std::move(o));
    }
    throw std::runtime_error("unknown material rule " + type);
}

const MCond* Terrain3::Impl::parse_cond(const Json& j) {
    if (j.is_str()) {
        auto it = cond_by_id.find(j.str);
        if (it != cond_by_id.end()) return it->second;
        const MCond* c = parse_cond(T.reg_.get("material_condition", j.str));
        cond_by_id[j.str] = c;
        return c;
    }
    std::string type = j.at("type").str;
    auto keep = [&](std::unique_ptr<MCond> p) { conds.push_back(std::move(p)); return conds.back().get(); };
    int id = node_count++;
    if (type == "minecraft:stone_depth")
        return keep(std::make_unique<StoneDepth>(id, int(j.at("offset").int_value()), j.at("add_surface_depth").boolean(), int(j.at("secondary_depth_range").int_value()), j.at("surface_type").str == "ceiling"));
    if (type == "minecraft:y_above")
        return keep(std::make_unique<YAbove>(id, resolve_anchor(j.at("anchor"), T.ns_.min_y, T.ns_.height), int(j.at("surface_depth_multiplier").int_value()), j.at("add_stone_depth").boolean()));
    if (type == "minecraft:water")
        return keep(std::make_unique<WaterCond>(id, int(j.at("offset").int_value()), int(j.at("surface_depth_multiplier").int_value()), j.at("add_stone_depth").boolean()));
    if (type == "minecraft:biome") {
        std::set<int> set;
        auto add = [&](const std::string& n) {
            for (size_t i = 0; i < T.biomes_.size(); i++) if (T.biomes_[i].name == n) set.insert(int(i));
        };
        const Json& b = j.at("biome_is");
        if (b.is_arr()) for (auto& e : b.arr) add(e.str); else add(b.str);
        return keep(std::make_unique<BiomeCond>(id, std::move(set)));
    }
    if (type == "minecraft:noise_threshold") {
        bool d3 = j.get_bool("is_3d", false);
        std::string n = j.at("noise").str;
        auto cond = std::make_unique<NoiseThreshold>(0, T.rs_->noise(n), j.at("min_threshold").number(), j.at("max_threshold").number(), d3);
        // each (noise, dimensionality) pair shares one cache slot like the context's sampler maps
        std::string key = n + (d3 ? "#3" : "#2");
        auto it = noise_slots.find(key);
        if (it == noise_slots.end()) it = noise_slots.emplace(key, int(noise_slots.size())).first;
        cond->slot = it->second;
        return keep(std::move(cond));
    }
    if (type == "minecraft:vertical_gradient")
        return keep(std::make_unique<VerticalGradient>(id, resolve_anchor(j.at("true_at_and_below"), T.ns_.min_y, T.ns_.height), resolve_anchor(j.at("false_at_and_above"), T.ns_.min_y, T.ns_.height),
                                                       &T.rs_->random_factory(j.at("random_name").str)));
    if (type == "minecraft:steep") return keep(std::make_unique<Steep>(id));
    if (type == "minecraft:hole") return keep(std::make_unique<Hole>(id));
    if (type == "minecraft:above_preliminary_surface") return keep(std::make_unique<AbovePrelim>());
    if (type == "minecraft:temperature") return keep(std::make_unique<Temperature>());
    if (type == "minecraft:not") return keep(std::make_unique<Not>(parse_cond(j.at("invert"))));
    throw std::runtime_error("unknown material condition " + type);
}

// ---- context

namespace {

void MCtx::update_xz(int x, int z, int gx, int gz) {
    ++xz_epoch;
    ++y_epoch;
    bx = x; bz = z; grad_x = gx; grad_z = gz;
    double v = double(sys->surface_noise->get(double(x), 0.0, double(z)));
    surface_depth = int(v * 2.75 + 3.0 + sys->noise_random->at(x, 0, z)->next_double() * 0.25);
}

double MCtx::get_secondary() {
    if (sec_epoch != xz_epoch) {
        sec_epoch = xz_epoch;
        secondary = double(sys->surface_secondary->get(double(bx), 0.0, double(bz)));
    }
    return secondary;
}

int MCtx::get_min_surface_level() {
    if (minsurf_epoch != xz_epoch) {
        minsurf_epoch = xz_epoch;
        float level;
        int index = prelim_vol.index_of_block(bx, 0, bz);
        if (index != -1) {
            if (!prelim_ready) {
                prelim_buf.resize(size_t(prelim_vol.size()));
                sys->prelim_surface->volume(nc->ctx, prelim_buf.data(), prelim_vol);
                prelim_ready = true;
            }
            level = prelim_buf[size_t(index)];
        } else {
            level = sys->prelim_surface->value(nc->ctx, bx, 0, bz);
        }
        min_surface = jfloor(double(level)) + surface_depth - 8;
    }
    return min_surface;
}

int BandRule::apply(MCtx& c, int x, int y, int z) const {
    int offset = int(std::floor(double(sys->clay_offset->get(double(x), 0.0, double(z)) * 4.0f) + 0.5));
    int n = int(sys->clay_bands.size());
    return sys->clay_bands[size_t(((y + offset + n) % n + n) % n)];
}

int OreVein::apply(MCtx& c, int x, int y, int z) const {
    auto& pf = c.prefill[size_t(slot)];
    if (!pf.ready) {
        pf.density.resize(size_t(c.expected.size()));
        pf.richness.resize(size_t(c.expected.size()));
        density->volume(c.nc->ctx, pf.density.data(), c.expected);
        richness->volume(c.nc->ctx, pf.richness.data(), c.expected);
        pf.ready = true;
    }
    int index = c.expected.index_of_block(c.bx, c.by, c.bz);
    float d = index == -1 ? density->value(c.nc->ctx, c.bx, c.by, c.bz) : pf.density[size_t(index)];
    if (d <= 0.0f) return -1;
    auto random = factory->at(x, y, z);
    if (random->next_float() > d) return -1;
    float r = index == -1 ? richness->value(c.nc->ctx, c.bx, c.by, c.bz) : pf.richness[size_t(index)];
    if (random->next_float() < r && gap->value(c.nc->ctx, c.bx, c.by, c.bz) < 0.0f) return random->next_float() < raw_chance ? raw : ore;
    return filler;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- Terrain3

namespace {

std::vector<uint16_t> generate_bands(Rng& random, const Terrain3::Impl& I) {
    std::vector<uint16_t> bands(192, I.terracotta);
    for (size_t i = 0; i < bands.size(); ++i) {
        i += size_t(random.next_int(5) + 1);
        if (i >= bands.size()) continue;
        bands[i] = I.orange_t;
    }
    auto make = [&](int base_width, uint16_t state) {
        int count = random.next_int_between_inclusive(6, 15);
        for (int i = 0; i < count; i++) {
            int width = base_width + random.next_int(3);
            int start = random.next_int(int(bands.size()));
            for (int p = 0; start + p < int(bands.size()) && p < width; p++) bands[size_t(start + p)] = state;
        }
    };
    make(1, I.yellow_t);
    make(2, I.brown_t);
    make(1, I.red_t);
    int white_count = random.next_int_between_inclusive(9, 15);
    int i = 0;
    for (int start = 0; i < white_count && start < int(bands.size()); ++i, start += random.next_int(16) + 4) {
        bands[size_t(start)] = I.white_t;
        if (start - 1 > 0 && random.next_boolean()) bands[size_t(start - 1)] = I.light_gray_t;
        if (start + 1 >= int(bands.size()) || !random.next_boolean()) continue;
        bands[size_t(start + 1)] = I.light_gray_t;
    }
    return bands;
}

uint16_t state_of(const BlockData& bd, const Json& j) { return j.is_str() ? bd.default_state(j.str) : bd.state(j); }

}  // namespace

Terrain3::Terrain3(const Registry& reg, const BlockData& bd, int64_t seed, const std::string& settings_name, std::vector<BiomeData3> biomes, int dim_min_y, int dim_height)
    : reg_(reg), bd_(bd), biomes_(std::move(biomes)) {
    const Json& s = reg.get("noise_settings", settings_name);
    int nmin = int(s.at("noise").at("min_y").int_value()), nh = int(s.at("noise").at("height").int_value());
    ns_.min_y = std::max(nmin, dim_min_y);
    ns_.height = std::min(nmin + nh, dim_min_y + dim_height) - ns_.min_y;
    ns_.dim_min_y = dim_min_y;
    ns_.dim_height = dim_height;
    ns_.sea_level = int(s.at("sea_level").int_value());
    ns_.has_aquifers = s.has("aquifers");
    bool legacy = s.get_bool("legacy_random_source", false);
    rs_ = std::make_unique<RandomState3>(reg, seed, legacy);
    comp_ = std::make_unique<DfCompiler>(*rs_);
    p_ = std::make_unique<Impl>(*this);
    Impl& I = *p_;
    I.legacy = legacy;
    I.default_block = state_of(bd, s.at("default_block"));
    I.default_fluid = state_of(bd, s.at("default_fluid"));
    I.air = 0;
    I.water = bd.find("minecraft:water", "level=0");
    I.lava = bd.find("minecraft:lava", "level=0");
    water_ = I.water;
    I.zoom_seed = obfuscate_seed(seed);
    auto by = [&](const char* n) { return bd.default_state(n); };
    I.white_t = by("minecraft:white_terracotta"); I.orange_t = by("minecraft:orange_terracotta"); I.terracotta = by("minecraft:terracotta");
    I.yellow_t = by("minecraft:yellow_terracotta"); I.brown_t = by("minecraft:brown_terracotta"); I.red_t = by("minecraft:red_terracotta");
    I.light_gray_t = by("minecraft:light_gray_terracotta"); I.packed_ice = by("minecraft:packed_ice"); I.snow_block = by("minecraft:snow_block");
    I.noise_random = const_cast<PosFactory*>(&rs_->root());
    auto n = [&](const char* name) { return rs_->noise(name); };
    I.clay_offset = n("minecraft:clay_bands_offset");
    I.surface_noise = n("minecraft:surface");
    I.surface_secondary = n("minecraft:surface_secondary");
    I.pillar = n("minecraft:badlands_pillar");
    I.pillar_roof = n("minecraft:badlands_pillar_roof");
    I.badlands_surface = n("minecraft:badlands_surface");
    I.iceberg_pillar = n("minecraft:iceberg_pillar");
    I.iceberg_roof = n("minecraft:iceberg_pillar_roof");
    I.iceberg_surface = n("minecraft:iceberg_surface");
    { auto r = rs_->random_from_hash("minecraft:clay_bands"); I.clay_bands = generate_bands(*r, I); }
    const Json& router = s.at("noise_router");
    I.final_density = comp_->sampler(router.at("final_density"));
    I.prelim_surface = comp_->sampler(router.at("chunk_surface_level"));
    if (ns_.has_aquifers) {
        const Json& a = s.at("aquifers");
        I.aq.barrier = comp_->sampler(a.at("barrier"));
        I.aq.floodedness = comp_->sampler(a.at("fluid_level_floodedness"));
        I.aq.spread = comp_->sampler(a.at("fluid_level_spread"));
        I.aq.lava = comp_->sampler(a.at("lava"));
        I.aq.exclusion = comp_->sampler(a.at("exclusion"));
        I.aq.surface = comp_->sampler(a.at("surface_level"));
    }
    for (size_t i = 0; i < biomes_.size(); i++) {
        if (biomes_[i].name == "minecraft:eroded_badlands") I.eroded_badlands = int(i);
        if (biomes_[i].name == "minecraft:frozen_ocean" || biomes_[i].name == "minecraft:deep_frozen_ocean") I.frozen_ocean.push_back(int(i));
    }
    I.root = I.parse_rule(s.at("material_rule"));
}

Terrain3::~Terrain3() = default;

void NoiseChunk3Deleter::operator()(NoiseChunk3* p) const { delete p; }

static NoiseChunkP make_chunk_vol(const Terrain3& t, const Terrain3::Impl& I, const Volume& vol, const Sampler* beardifier) {
    NoiseChunkP nc(new NoiseChunk3());
    nc->vol = vol;
    nc->ctx.caches = true;
    nc->ctx.beardifier = beardifier;
    nc->air = 0;
    nc->water = I.water;
    nc->lava = I.lava;
    nc->enabled = t.settings().has_aquifers;
    int sea = t.settings().sea_level;
    FluidStatus lava_status{-54, I.lava}, sea_status{sea, I.default_fluid};
    int limit = std::min(-54, sea);
    nc->picker = [lava_status, sea_status, limit](int, int y, int) { return y < limit ? lava_status : sea_status; };
    if (nc->enabled) {
        nc->cfg = I.aq;
        nc->rand = &I.T.random_state().random_factory("minecraft:aquifer");
        nc->init_aquifer(vol);
    }
    return nc;
}

NoiseChunkP Terrain3::make_chunk(int chunk_x, int chunk_z, const Sampler* beardifier) const {
    Volume v;
    v.sx = 16; v.sy = ns_.height; v.sz = 16;
    v.minx = chunk_x * 16; v.miny = ns_.min_y; v.minz = chunk_z * 16;
    return make_chunk_vol(*this, *p_, v, beardifier);
}

void Terrain3::do_fill(NoiseChunk3& nc, ChunkBlocks& out) const {
    const Impl& I = *p_;
    const Volume& v = nc.vol;
    std::vector<float> dens(size_t(v.size()));
    I.final_density->volume(nc.ctx, dens.data(), v);
    std::vector<uint16_t> blocks(size_t(ns_.dim_height) * 256, I.air);
    std::vector<uint8_t> marks(blocks.size(), 0);
    const uint8_t* flags = bd_.chunk_flags();
    for (int z = 0; z < v.sz; z++) {
        int bz = v.block_z(z);
        for (int x = 0; x < v.sx; x++) {
            int bx = v.block_x(x);
            for (int y = v.sy - 1; y >= 0; y--) {
                int by = v.block_y(y);
                float density = dens[size_t(v.index(x, y, z))];
                int sub = nc.substance(bx, by, bz, double(density));
                uint16_t state = sub < 0 ? I.default_block : uint16_t(sub);
                if (state == I.air) continue;
                size_t idx = size_t(by - ns_.dim_min_y) * 256 + size_t(z) * 16 + size_t(x);
                blocks[idx] = state;
                if (nc.schedule && (flags[state] & kFlagFluid)) marks[idx] = 1;
            }
        }
    }
    out.init(ns_.dim_min_y, ns_.dim_height, std::move(blocks), flags);
    out.marks = std::move(marks);
}

int Terrain3::aquifer_substance(NoiseChunk3& nc, int x, int y, int z, double density) const { return nc.substance(x, y, z, density); }
bool Terrain3::aquifer_should_schedule(const NoiseChunk3& nc) const { return nc.schedule; }

int Terrain3::iterate_column(int bx, int bz, const std::function<bool(uint16_t)>& tester, std::vector<uint16_t>* states) const {
    const Impl& I = *p_;
    if (ns_.height <= 0) return ns_.min_y;
    Volume v;
    v.sx = 1; v.sy = ns_.height; v.sz = 1; v.minx = bx; v.miny = ns_.min_y; v.minz = bz;
    auto nc = make_chunk_vol(*this, I, v, nullptr);
    std::vector<float> dens(size_t(v.size()));
    I.final_density->volume(nc->ctx, dens.data(), v);
    if (states) states->assign(size_t(v.sy), I.air);
    for (int y = v.sy - 1; y >= 0; y--) {
        int by = v.block_y(y);
        int sub = nc->substance(bx, by, bz, double(dens[size_t(v.index(0, y, 0))]));
        uint16_t state = sub < 0 ? I.default_block : uint16_t(sub);
        if (states) (*states)[size_t(y)] = state;
        if (tester && tester(state)) return by + 1;
    }
    return ns_.min_y;
}

int Terrain3::biome_at(int x, int y, int z, const std::function<int(int, int, int)>& lookup) const {
    int abs_x = x - 2, abs_y = y - 2, abs_z = z - 2;
    int px = abs_x >> 2, py = abs_y >> 2, pz = abs_z >> 2;
    double fx = double(abs_x & 3) / 4.0, fy = double(abs_y & 3) / 4.0, fz = double(abs_z & 3) / 4.0;
    int min_i = 0;
    double min_d = INFINITY;
    for (int i = 0; i < 8; i++) {
        bool xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
        int cx = xe ? px : px + 1, cy = ye ? py : py + 1, cz = ze ? pz : pz + 1;
        double dx = xe ? fx : fx - 1.0, dy = ye ? fy : fy - 1.0, dz = ze ? fz : fz - 1.0;
        double d = fiddled_distance(p_->zoom_seed, cx, cy, cz, dx, dy, dz);
        if (min_d > d) { min_i = i; min_d = d; }
    }
    int bx = (min_i & 4) == 0 ? px : px + 1, by = (min_i & 2) == 0 ? py : py + 1, bz = (min_i & 1) == 0 ? pz : pz + 1;
    int min_q = ns_.dim_min_y >> 2, max_q = min_q + (ns_.dim_height >> 2) - 1;
    return lookup(bx, std::clamp(by, min_q, max_q), bz);
}

// ---- buildSurface

namespace {

struct Column {
    ChunkBlocks& b;
    int x, z;   // block coordinates
    uint16_t get(int y) const { return b.get(x, y, z); }
    void set(int y, uint16_t id) const { b.set(x, y, z, id); }
};

void eroded_badlands_extension(const Terrain3::Impl& I, Column& col, int bx, int bz, int height, int min_y) {
    double pillar_buffer = std::min(std::abs(double(I.badlands_surface->get(double(bx), 0.0, double(bz))) * 8.25),
                                    double(I.pillar->get(double(bx) * 0.2, 0.0, double(bz) * 0.2) * 15.0f));
    if (pillar_buffer <= 0.0) return;
    double pillar_floor = std::abs(double(I.pillar_roof->get(double(bx) * 0.75, 0.0, double(bz) * 0.75)) * 1.5);
    double extension_top = 64.0 + std::min(pillar_buffer * pillar_buffer * 2.5, std::ceil(pillar_floor * 50.0) + 24.0);
    int start_y = jfloor(extension_top);
    if (height > start_y) return;
    int y;
    for (y = start_y; y >= min_y; --y) {
        uint16_t old = col.get(y);
        if (old == I.default_block) break;
        if (old == I.water) return;
    }
    for (y = start_y; y >= min_y && col.get(y) == 0; --y) col.set(y, I.default_block);
}

void frozen_ocean_extension(const Terrain3& T, const Terrain3::Impl& I, int min_surface, const BiomeData3& biome, Column& col, int bx, int bz, int height, const uint8_t* flags) {
    int sea = T.settings().sea_level;
    double iceberg = std::min(std::abs(double(I.iceberg_surface->get(double(bx), 0.0, double(bz))) * 8.25), double(I.iceberg_pillar->get(double(bx) * 1.28, 0.0, double(bz) * 1.28) * 15.0f));
    if (iceberg <= 1.8) return;
    double iceberg_roof = std::abs(double(I.iceberg_roof->get(double(bx) * 1.17, 0.0, double(bz) * 1.17)) * 1.5);
    double top = std::min(iceberg * iceberg * 1.2, std::ceil(iceberg_roof * 40.0) + 14.0);
    if (biome_temperature3(biome, bx, sea, bz, sea) > 0.1f) top -= 2.0;
    if (top <= 2.0) return;
    double extension_bottom = double(sea) - top - 7.0;
    top += double(sea);
    double extension_top = top;
    auto random = I.noise_random->at(bx, 0, bz);
    int max_snow_depth = 2 + random->next_int(4);
    int min_snow_height = sea + 18 + random->next_int(10);
    int snow_depth = 0;
    for (int y = std::max(height, int(extension_top) + 1); y >= min_surface; --y) {
        uint16_t here = col.get(y);
        bool air = (flags[here] & kFlagAir) != 0;
        if (!(air && y < int(extension_top) && random->next_double() > 0.01) &&
            (!(here == I.water) || y <= int(extension_bottom) || y >= sea || !(random->next_double() > 0.15))) continue;
        if (snow_depth <= max_snow_depth && y > min_snow_height) {
            col.set(y, I.snow_block);
            ++snow_depth;
            continue;
        }
        col.set(y, I.packed_ice);
    }
}

}  // namespace

void Terrain3::build_surface(NoiseChunk3& nc, ChunkBlocks& blocks, const std::function<int(int, int, int)>& quart_biome) const {
    const Impl& I = *p_;
    const uint8_t* flags = bd_.chunk_flags();
    int chunk_min_x = nc.vol.minx, chunk_min_z = nc.vol.minz;
    int min_y = ns_.dim_min_y, max_y = ns_.dim_min_y + ns_.dim_height - 1;
    // getHighestFilledSectionIndex: the top section with a block other than air
    int highest = -1;
    for (int sec = ns_.dim_height / 16 - 1; sec >= 0 && highest < 0; sec--)
        for (size_t i = size_t(sec) * 16 * 256; i < size_t(sec + 1) * 16 * 256; i++)
            if (!(flags[blocks.b[i]] & kFlagAir)) { highest = sec; break; }
    int max_block_y = (min_y >> 4) * 16 + (highest + 0) * 16 + 15;
    if (highest < 0) max_block_y = min_y - 1;
    MCtx ctx;
    ctx.sys = &I;
    ctx.T = this;
    ctx.nc = &nc;
    ctx.blocks = &blocks;
    ctx.quart_biome = &quart_biome;
    ctx.expected.sx = 16; ctx.expected.sz = 16;
    ctx.expected.sy = std::max(max_block_y - nc.vol.miny + 1, 1);
    ctx.expected.minx = chunk_min_x; ctx.expected.miny = nc.vol.miny; ctx.expected.minz = chunk_min_z;
    ctx.prelim_vol.sx = 16; ctx.prelim_vol.sy = 1; ctx.prelim_vol.sz = 16;
    ctx.prelim_vol.minx = chunk_min_x; ctx.prelim_vol.miny = 0; ctx.prelim_vol.minz = chunk_min_z;
    ctx.lazy.assign(size_t(I.node_count), MCtx::Lazy{});
    ctx.noise_cache.assign(I.noise_slots.size(), MCtx::NoiseCache{});
    ctx.prefill.assign(size_t(I.node_count), MCtx::Prefill{});
    for (int x = 0; x < 16; x++) {
        for (int z = 0; z < 16; z++) {
            int bx = chunk_min_x + x, bz = chunk_min_z + z;
            int starting_height = blocks.first_free(WORLD_SURFACE_WG, x, z);
            Column col{blocks, bx, bz};
            int surface_biome = biome_at(bx, starting_height, bz, quart_biome);
            if (surface_biome == I.eroded_badlands) eroded_badlands_extension(I, col, bx, bz, starting_height, min_y);
            int height = blocks.first_free(WORLD_SURFACE_WG, x, z);
            int gx = blocks.first_free(WORLD_SURFACE_WG, std::min(x + 1, 15), z) - blocks.first_free(WORLD_SURFACE_WG, std::max(x - 1, 0), z);
            int gz = blocks.first_free(WORLD_SURFACE_WG, x, std::min(z + 1, 15)) - blocks.first_free(WORLD_SURFACE_WG, x, std::max(z - 1, 0));
            ctx.update_xz(bx, bz, gx, gz);
            int stone_above = 0, water_height = INT_MIN, next_ceiling = INT_MAX, end_y = min_y;
            for (int y = height; y >= end_y; --y) {
                uint16_t old = col.get(y);
                if (flags[old] & kFlagAir) { stone_above = 0; water_height = INT_MIN; continue; }
                if (flags[old] & kFlagFluid) {
                    if (water_height != INT_MIN) continue;
                    water_height = y + 1;
                    continue;
                }
                if (next_ceiling >= y) {
                    next_ceiling = kWayBelowMinY;
                    for (int look = y - 1; look >= end_y - 1; --look) {
                        uint16_t next = col.get(look);
                        bool stone = !(flags[next] & kFlagAir) && !(flags[next] & kFlagFluid);
                        if (stone) continue;
                        next_ceiling = look + 1;
                        break;
                    }
                }
                int stone_below = y - next_ceiling + 1;
                ctx.update_y(++stone_above, stone_below, water_height, y);
                if (y < min_y || y > max_y) continue;
                int state = I.root->apply(ctx, bx, y, bz);
                if (state < 0) continue;
                col.set(y, uint16_t(state));
            }
            bool frozen = false;
            for (int f : I.frozen_ocean) if (f == surface_biome) frozen = true;
            if (frozen) frozen_ocean_extension(*this, I, ctx.get_min_surface_level(), biomes_[size_t(surface_biome)], col, bx, bz, starting_height, flags);
        }
    }
}

bool Terrain3::top_material(NoiseChunk3& nc, ChunkBlocks& blocks, const std::function<int(int, int, int)>& quart_biome, int x, int y, int z, bool under_fluid, uint16_t& out) const {
    const Impl& I = *p_;
    MCtx ctx;
    ctx.sys = &I;
    ctx.T = this;
    ctx.nc = &nc;
    ctx.blocks = &blocks;
    ctx.quart_biome = &quart_biome;
    ctx.expected.sx = ctx.expected.sy = ctx.expected.sz = 1;
    ctx.expected.minx = x; ctx.expected.miny = y; ctx.expected.minz = z;
    ctx.prelim_vol = ctx.expected;
    ctx.prelim_vol.miny = 0;
    ctx.lazy.assign(size_t(I.node_count), MCtx::Lazy{});
    ctx.noise_cache.assign(I.noise_slots.size(), MCtx::NoiseCache{});
    ctx.prefill.assign(size_t(I.node_count), MCtx::Prefill{});
    int lx = x & 15, lz = z & 15;
    int gx = blocks.first_free(WORLD_SURFACE_WG, std::min(lx + 1, 15), lz) - blocks.first_free(WORLD_SURFACE_WG, std::max(lx - 1, 0), lz);
    int gz = blocks.first_free(WORLD_SURFACE_WG, lx, std::min(lz + 1, 15)) - blocks.first_free(WORLD_SURFACE_WG, lx, std::max(lz - 1, 0));
    ctx.update_xz(x, z, gx, gz);
    ctx.update_y(1, 1, under_fluid ? y + 1 : INT_MIN, y);
    int state = I.root->apply(ctx, x, y, z);
    if (state < 0) return false;
    out = uint16_t(state);
    return true;
}

}  // namespace pn::vg::v3
