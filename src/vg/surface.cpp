#include "surface.hpp"
#include "noise.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <openssl/evp.h>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace pn::vg {

namespace {

constexpr int kWayBelowMinY = -32512;

inline int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

int64_t lcg_next(int64_t rval, int64_t c) {
    uint64_t r = uint64_t(rval);
    r *= r * 6364136223846793005ULL + 1442695040888963407ULL;
    return int64_t(r + uint64_t(c));
}

// Guava Hashing.sha256().hashLong(seed).asLong(): first eight digest bytes, little endian
int64_t obfuscate_seed(int64_t seed) {
    unsigned char in[8], out[32];
    for (int i = 0; i < 8; i++) in[i] = uint8_t(uint64_t(seed) >> (8 * i));
    unsigned int len = 0;
    EVP_Digest(in, 8, out, &len, EVP_sha256(), nullptr);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | out[i];
    return int64_t(v);
}

double fiddle(int64_t rval) {
    int64_t m = (rval >> 24) % 1024;
    if (m < 0) m += 1024;
    return (double(m) / 1024.0 - 0.5) * 0.9;
}

double fiddled_distance(int64_t seed, int x, int y, int z, double dx, double dy, double dz) {
    int64_t r = lcg_next(seed, x);
    r = lcg_next(r, y);
    r = lcg_next(r, z);
    r = lcg_next(r, x);
    r = lcg_next(r, y);
    r = lcg_next(r, z);
    double fx = fiddle(r);
    r = lcg_next(r, seed);
    double fy = fiddle(r);
    r = lcg_next(r, seed);
    double fz = fiddle(r);
    double a = dz + fz, b = dy + fy, c = dx + fx;
    return a * a + b * b + c * c;
}

}  // namespace

// the per-chunk state the rules read (Mojang's SurfaceRules.Context)
struct SCtx {
    const SurfaceSystem::Impl* sys;
    ChunkFill* fill;
    const ChunkBlocks* blocks;
    int chunk_x, chunk_z;
    int block_x = 0, block_z = 0, block_y = 0, surface_depth = 0, stone_depth_above = 0, stone_depth_below = 0, water_height = INT_MIN;
    uint64_t xz_epoch = 1, y_epoch = 1;
    int biome = -1;
    bool have_biome = false;
    const std::function<int(int, int, int)>* lookup = nullptr;
    uint64_t secondary_epoch = 0, min_surface_epoch = 0;
    double surface_secondary = 0;
    int min_surface_level = 0;
    int64_t prelim_cell = INT64_MIN;
    int prelim[4] = {0, 0, 0, 0};
    struct Cache { uint64_t epoch = 0; double value = 0; };
    std::vector<Cache> caches;
};

struct SurfaceSystem::Impl {
    const WorldState& state;
    SurfaceBlocks blocks;
    uint16_t default_block;
    int sea_level, min_y, height;
    bool legacy;
    std::vector<SurfaceBiome> biomes;
    std::function<int(int, int, int)> noise_biome;
    int64_t zoom_seed;
    std::vector<uint16_t> clay_bands;
    const NormalNoise *clay_offset, *surface_noise, *surface_secondary_noise, *pillar, *pillar_roof, *badlands_surface, *iceberg_pillar, *iceberg_roof, *iceberg_surface;
    uint16_t white_terracotta, orange_terracotta, terracotta, yellow_terracotta, brown_terracotta, red_terracotta, light_gray_terracotta, packed_ice, snow_block;
    int eroded_badlands = -1;
    std::unique_ptr<SurfaceRuleTree> tree;
    bool has_frozen_ocean = false;
    std::vector<int> frozen_ocean_biomes;

    Impl(const WorldState& s) : state(s) {}

    bool air(uint16_t id) const { return blocks.flags[id] & 1; }
    bool fluid(uint16_t id) const { return blocks.flags[id] & 2; }

    int get_surface_depth(int x, int z) const {
        double v = surface_noise->get_value(x, 0.0, z);
        return int(v * 2.75 + 3.0 + state.random().at(x, 0, z)->next_double() * 0.25);
    }
    double get_surface_secondary(int x, int z) const { return surface_secondary_noise->get_value(x, 0.0, z); }
    uint16_t band(int x, int y, int z) const {
        int offset = int(std::floor(clay_offset->get_value(x, 0.0, z) * 4.0 + 0.5));
        int n = int(clay_bands.size());
        return clay_bands[size_t(((y + offset + n) % n + n) % n)];
    }
    int get_biome(int x, int y, int z) const { return get_biome(x, y, z, noise_biome); }
    int get_biome(int x, int y, int z, const std::function<int(int, int, int)>& lookup) const {
        int abs_x = x - 2, abs_y = y - 2, abs_z = z - 2;
        int px = abs_x >> 2, py = abs_y >> 2, pz = abs_z >> 2;
        double fx = double(abs_x & 3) / 4.0, fy = double(abs_y & 3) / 4.0, fz = double(abs_z & 3) / 4.0;
        int min_i = 0;
        double min_d = INFINITY;
        for (int i = 0; i < 8; i++) {
            bool xe = (i & 4) == 0, ye = (i & 2) == 0, ze = (i & 1) == 0;
            int cx = xe ? px : px + 1, cy = ye ? py : py + 1, cz = ze ? pz : pz + 1;
            double dx = xe ? fx : fx - 1.0, dy = ye ? fy : fy - 1.0, dz = ze ? fz : fz - 1.0;
            double d = fiddled_distance(zoom_seed, cx, cy, cz, dx, dy, dz);
            if (min_d > d) { min_i = i; min_d = d; }
        }
        int bx = (min_i & 4) == 0 ? px : px + 1, by = (min_i & 2) == 0 ? py : py + 1, bz = (min_i & 1) == 0 ? pz : pz + 1;
        // ChunkAccess.getNoiseBiome clamps the quart y into the generated range
        int min_q = min_y >> 2, max_q = min_q + (height >> 2) - 1;
        return lookup(bx, std::clamp(by, min_q, max_q), bz);
    }
    float temperature_at(int biome, int x, int y, int z) const { return biome_temperature(biomes[size_t(biome)], x, y, z, sea_level); }
    bool cold_enough_to_snow(int biome, int x, int y, int z) const { return !(temperature_at(biome, x, y, z) >= 0.15f); }
};

// ---------------------------------------------------------------- rule tree

struct Cond {
    virtual ~Cond() = default;
    virtual bool test(SCtx& c) const = 0;
};
struct Rule {
    virtual ~Rule() = default;
    virtual bool apply(SCtx& c, int x, int y, int z, uint16_t& out) const = 0;
};
using CondP = std::unique_ptr<Cond>;
using RuleP = std::unique_ptr<Rule>;

struct SurfaceRuleTree {
    RuleP root;
    int slots = 0;
};

namespace {

using Impl = SurfaceSystem::Impl;

// cached per update of the column (xz) or of the y step
template <bool Y>
struct Lazy : Cond {
    int slot;
    explicit Lazy(int s) : slot(s) {}
    bool test(SCtx& c) const final {
        uint64_t epoch = Y ? c.y_epoch : c.xz_epoch;
        auto& e = c.caches[size_t(slot)];
        if (e.epoch == epoch) return e.value != 0.0;
        e.epoch = epoch;
        bool r = compute(c);
        e.value = r ? 1.0 : 0.0;
        return r;
    }
    virtual bool compute(SCtx& c) const = 0;
};

struct Anchor {
    int kind = 0;   // 0 absolute, 1 above_bottom, 2 below_top
    int v = 0;
    int resolve(int min_y, int height) const { return kind == 0 ? v : kind == 1 ? min_y + v : height - 1 + min_y - v; }
    static Anchor parse(const Json& j) {
        Anchor a;
        if (const Json* x = j.find("absolute")) { a.kind = 0; a.v = int(x->int_value()); }
        else if (const Json* y = j.find("above_bottom")) { a.kind = 1; a.v = int(y->int_value()); }
        else if (const Json* z = j.find("below_top")) { a.kind = 2; a.v = int(z->int_value()); }
        return a;
    }
};

struct NotCond : Cond {
    CondP t;
    bool test(SCtx& c) const override { return !t->test(c); }
};

struct StoneDepthCond : Lazy<true> {
    int offset, secondary_range;
    bool add_surface_depth, ceiling;
    using Lazy<true>::Lazy;
    bool compute(SCtx& c) const override {
        int stone_depth = ceiling ? c.stone_depth_below : c.stone_depth_above;
        int surface_depth = add_surface_depth ? c.surface_depth : 0;
        int secondary = 0;
        if (secondary_range != 0) {
            if (c.secondary_epoch != c.xz_epoch) { c.secondary_epoch = c.xz_epoch; c.surface_secondary = c.sys->get_surface_secondary(c.block_x, c.block_z); }
            secondary = int(clamped_map(c.surface_secondary, -1.0, 1.0, 0.0, double(secondary_range)));
        }
        return stone_depth <= 1 + offset + surface_depth + secondary;
    }
};

struct YCond : Lazy<true> {
    Anchor anchor;
    int multiplier;
    bool add_stone_depth;
    using Lazy<true>::Lazy;
    bool compute(SCtx& c) const override {
        return c.block_y + (add_stone_depth ? c.stone_depth_above : 0) >= anchor.resolve(c.sys->min_y, c.sys->height) + c.surface_depth * multiplier;
    }
};

struct WaterCond : Lazy<true> {
    int offset, multiplier;
    bool add_stone_depth;
    using Lazy<true>::Lazy;
    bool compute(SCtx& c) const override {
        return c.water_height == INT_MIN || c.block_y + (add_stone_depth ? c.stone_depth_above : 0) >= c.water_height + offset + c.surface_depth * multiplier;
    }
};

int current_biome(SCtx& c) {
    if (!c.have_biome) { c.biome = c.lookup ? c.sys->get_biome(c.block_x, c.block_y, c.block_z, *c.lookup) : c.sys->get_biome(c.block_x, c.block_y, c.block_z); c.have_biome = true; }
    return c.biome;
}

struct BiomeCond : Lazy<true> {
    std::set<std::string> names;
    std::vector<uint8_t> match;   // per biome index
    using Lazy<true>::Lazy;
    bool compute(SCtx& c) const override { return match[size_t(current_biome(c))] != 0; }
};

struct NoiseCond : Lazy<false> {
    const NormalNoise* noise;
    double lo, hi;
    using Lazy<false>::Lazy;
    bool compute(SCtx& c) const override {
        double v = noise->get_value(c.block_x, 0.0, c.block_z);
        return v >= lo && v <= hi;
    }
};

struct GradientCond : Lazy<true> {
    int true_below, false_above;
    const PosFactory* random;
    using Lazy<true>::Lazy;
    bool compute(SCtx& c) const override {
        int y = c.block_y;
        if (y <= true_below) return true;
        if (y >= false_above) return false;
        double p = lerp(inverse_lerp(double(y), double(true_below), double(false_above)), 1.0, 0.0);
        return random->at(c.block_x, y, c.block_z)->next_float() < p;
    }
};

struct SteepCond : Lazy<false> {
    using Lazy<false>::Lazy;
    bool compute(SCtx& c) const override {
        int cx = c.block_x & 15, cz = c.block_z & 15;
        int zn = std::max(cz - 1, 0), zs = std::min(cz + 1, 15);
        int hn = c.blocks->hm[WORLD_SURFACE_WG][zn * 16 + cx], hs = c.blocks->hm[WORLD_SURFACE_WG][zs * 16 + cx];
        if (hs >= hn + 4) return true;
        int xw = std::max(cx - 1, 0), xe = std::min(cx + 1, 15);
        int hw = c.blocks->hm[WORLD_SURFACE_WG][cz * 16 + xw], he = c.blocks->hm[WORLD_SURFACE_WG][cz * 16 + xe];
        return hw >= he + 4;
    }
};

struct HoleCond : Lazy<false> {
    using Lazy<false>::Lazy;
    bool compute(SCtx& c) const override { return c.surface_depth <= 0; }
};

struct TemperatureCond : Lazy<true> {
    using Lazy<true>::Lazy;
    bool compute(SCtx& c) const override { return c.sys->cold_enough_to_snow(current_biome(c), c.block_x, c.block_y, c.block_z); }
};

int min_surface_level(SCtx& c) {
    if (c.min_surface_epoch != c.xz_epoch) {
        c.min_surface_epoch = c.xz_epoch;
        int cell_x = c.block_x >> 4, cell_z = c.block_z >> 4;
        int64_t cell = (int64_t(cell_x) << 32) | uint32_t(cell_z);
        if (c.prelim_cell != cell) {
            c.prelim_cell = cell;
            c.prelim[0] = c.fill->preliminary_surface_level(cell_x << 4, cell_z << 4);
            c.prelim[1] = c.fill->preliminary_surface_level((cell_x + 1) << 4, cell_z << 4);
            c.prelim[2] = c.fill->preliminary_surface_level(cell_x << 4, (cell_z + 1) << 4);
            c.prelim[3] = c.fill->preliminary_surface_level((cell_x + 1) << 4, (cell_z + 1) << 4);
        }
        double l = lerp2(double(float(c.block_x & 15) / 16.0f), double(float(c.block_z & 15) / 16.0f), c.prelim[0], c.prelim[1], c.prelim[2], c.prelim[3]);
        c.min_surface_level = mth_floor(l) + c.surface_depth - 8;
    }
    return c.min_surface_level;
}

struct AboveSurfaceCond : Cond {
    bool test(SCtx& c) const override { return c.block_y >= min_surface_level(c); }
};

struct BlockRule : Rule {
    uint16_t state;
    bool apply(SCtx&, int, int, int, uint16_t& out) const override { out = state; return true; }
};
struct BandRule : Rule {
    bool apply(SCtx& c, int x, int y, int z, uint16_t& out) const override { out = c.sys->band(x, y, z); return true; }
};
struct SeqRule : Rule {
    std::vector<RuleP> rules;
    bool apply(SCtx& c, int x, int y, int z, uint16_t& out) const override {
        for (auto& r : rules) if (r->apply(c, x, y, z, out)) return true;
        return false;
    }
};
struct TestRule : Rule {
    CondP cond;
    RuleP then;
    bool apply(SCtx& c, int x, int y, int z, uint16_t& out) const override { return cond->test(c) && then->apply(c, x, y, z, out); }
};

struct TreeBuilder {
    Impl& I;
    int slots = 0;
    std::vector<std::pair<BiomeCond*, std::set<std::string>>> biome_conds;

    static std::string short_type(const Json& j) { return j.at("type").str; }

    CondP cond(const Json& j) {
        std::string t = short_type(j);
        if (t == "minecraft:not") { auto n = std::make_unique<NotCond>(); n->t = cond(j.at("invert")); return n; }
        if (t == "minecraft:stone_depth") {
            auto c = std::make_unique<StoneDepthCond>(slots++);
            c->offset = int(j.at("offset").int_value());
            c->add_surface_depth = j.at("add_surface_depth").boolean();
            c->secondary_range = int(j.at("secondary_depth_range").int_value());
            c->ceiling = j.at("surface_type").str == "ceiling";
            return c;
        }
        if (t == "minecraft:y_above") {
            auto c = std::make_unique<YCond>(slots++);
            c->anchor = Anchor::parse(j.at("anchor"));
            c->multiplier = int(j.at("surface_depth_multiplier").int_value());
            c->add_stone_depth = j.at("add_stone_depth").boolean();
            return c;
        }
        if (t == "minecraft:water") {
            auto c = std::make_unique<WaterCond>(slots++);
            c->offset = int(j.at("offset").int_value());
            c->multiplier = int(j.at("surface_depth_multiplier").int_value());
            c->add_stone_depth = j.at("add_stone_depth").boolean();
            return c;
        }
        if (t == "minecraft:biome") {
            auto c = std::make_unique<BiomeCond>(slots++);
            const Json& b = j.at("biome_is");
            if (b.is_arr()) for (auto& e : b.arr) c->names.insert(e.str); else c->names.insert(b.str);
            c->match.assign(I.biomes.size(), 0);
            for (size_t i = 0; i < I.biomes.size(); i++) c->match[i] = c->names.count(I.biomes[i].name) ? 1 : 0;
            return c;
        }
        if (t == "minecraft:noise_threshold") {
            auto c = std::make_unique<NoiseCond>(slots++);
            c->noise = I.state.noise(j.at("noise").str);
            c->lo = j.at("min_threshold").number();
            c->hi = j.at("max_threshold").number();
            return c;
        }
        if (t == "minecraft:vertical_gradient") {
            auto c = std::make_unique<GradientCond>(slots++);
            c->true_below = Anchor::parse(j.at("true_at_and_below")).resolve(I.min_y, I.height);
            c->false_above = Anchor::parse(j.at("false_at_and_above")).resolve(I.min_y, I.height);
            c->random = &I.state.random_factory(j.at("random_name").str);
            return c;
        }
        if (t == "minecraft:steep") return std::make_unique<SteepCond>(slots++);
        if (t == "minecraft:hole") return std::make_unique<HoleCond>(slots++);
        if (t == "minecraft:temperature") return std::make_unique<TemperatureCond>(slots++);
        if (t == "minecraft:above_preliminary_surface") return std::make_unique<AboveSurfaceCond>();
        throw std::runtime_error("unknown surface condition " + t);
    }

    RuleP rule(const Json& j) {
        std::string t = short_type(j);
        if (t == "minecraft:block") { auto r = std::make_unique<BlockRule>(); r->state = I.blocks.resolve(j.at("result_state")); return r; }
        if (t == "minecraft:bandlands") return std::make_unique<BandRule>();
        if (t == "minecraft:sequence") {
            const Json& seq = j.at("sequence");
            if (seq.arr.size() == 1) return rule(seq.arr[0]);
            auto r = std::make_unique<SeqRule>();
            for (auto& e : seq.arr) r->rules.push_back(rule(e));
            return r;
        }
        if (t == "minecraft:condition") {
            auto r = std::make_unique<TestRule>();
            r->cond = cond(j.at("if_true"));
            r->then = rule(j.at("then_run"));
            return r;
        }
        throw std::runtime_error("unknown surface rule " + t);
    }
};

void make_bands(Rng& random, std::vector<uint16_t>& bands, int base_width, uint16_t state) {
    int count = random.next_int_between_inclusive(6, 15);
    for (int i = 0; i < count; i++) {
        int width = base_width + random.next_int(3);
        int start = random.next_int(int(bands.size()));
        for (int p = 0; start + p < int(bands.size()) && p < width; p++) bands[size_t(start + p)] = state;
    }
}

}  // namespace

SurfaceSystem::~SurfaceSystem() = default;

SurfaceSystem::SurfaceSystem(const WorldState& state, const Json& rule, const SurfaceBlocks& blocks, uint16_t default_block, int sea_level, int min_y, int height, bool legacy,
                             std::vector<SurfaceBiome> biomes, std::function<int(int, int, int)> noise_biome)
    : p_(std::make_unique<Impl>(state)) {
    Impl& I = *p_;
    I.blocks = blocks;
    I.default_block = default_block;
    I.sea_level = sea_level;
    I.min_y = min_y;
    I.height = height;
    I.legacy = legacy;
    I.biomes = std::move(biomes);
    I.noise_biome = std::move(noise_biome);
    I.zoom_seed = obfuscate_seed(state.seed());
    auto by = [&](const char* n) { return blocks.by_name(n); };
    I.white_terracotta = by("minecraft:white_terracotta");
    I.orange_terracotta = by("minecraft:orange_terracotta");
    I.terracotta = by("minecraft:terracotta");
    I.yellow_terracotta = by("minecraft:yellow_terracotta");
    I.brown_terracotta = by("minecraft:brown_terracotta");
    I.red_terracotta = by("minecraft:red_terracotta");
    I.light_gray_terracotta = by("minecraft:light_gray_terracotta");
    I.packed_ice = by("minecraft:packed_ice");
    I.snow_block = by("minecraft:snow_block");
    I.clay_offset = state.noise("minecraft:clay_bands_offset");
    I.surface_noise = state.noise("minecraft:surface");
    I.surface_secondary_noise = state.noise("minecraft:surface_secondary");
    I.pillar = state.noise("minecraft:badlands_pillar");
    I.pillar_roof = state.noise("minecraft:badlands_pillar_roof");
    I.badlands_surface = state.noise("minecraft:badlands_surface");
    I.iceberg_pillar = state.noise("minecraft:iceberg_pillar");
    I.iceberg_roof = state.noise("minecraft:iceberg_pillar_roof");
    I.iceberg_surface = state.noise("minecraft:iceberg_surface");
    {   // clay bands
        auto random = state.random().from_hash_of("minecraft:clay_bands");
        I.clay_bands.assign(192, I.terracotta);
        for (size_t i = 0; i < I.clay_bands.size(); i++) {
            i += size_t(random->next_int(5) + 1);
            if (i < I.clay_bands.size()) I.clay_bands[i] = I.orange_terracotta;
        }
        make_bands(*random, I.clay_bands, 1, I.yellow_terracotta);
        make_bands(*random, I.clay_bands, 2, I.brown_terracotta);
        make_bands(*random, I.clay_bands, 1, I.red_terracotta);
        int white_count = random->next_int_between_inclusive(9, 15);
        int n = 0;
        for (int start = 0; n < white_count && start < int(I.clay_bands.size()); start += random->next_int(16) + 4) {
            I.clay_bands[size_t(start)] = I.white_terracotta;
            if (start - 1 > 0 && random->next_boolean()) I.clay_bands[size_t(start - 1)] = I.light_gray_terracotta;
            if (start + 1 < int(I.clay_bands.size()) && random->next_boolean()) I.clay_bands[size_t(start + 1)] = I.light_gray_terracotta;
            n++;
        }
    }
    for (size_t i = 0; i < I.biomes.size(); i++) {
        if (I.biomes[i].name == "minecraft:eroded_badlands") I.eroded_badlands = int(i);
        if (I.biomes[i].name == "minecraft:frozen_ocean" || I.biomes[i].name == "minecraft:deep_frozen_ocean") I.frozen_ocean_biomes.push_back(int(i));
    }
    TreeBuilder tb{I};
    I.tree = std::make_unique<SurfaceRuleTree>();
    I.tree->root = tb.rule(rule);
    I.tree->slots = tb.slots;
}

int SurfaceSystem::biome_at(int x, int y, int z) const { return p_->get_biome(x, y, z); }
int SurfaceSystem::biome_with(int x, int y, int z, const std::function<int(int, int, int)>& lookup) const { return p_->get_biome(x, y, z, lookup); }

bool SurfaceSystem::top_material(const ChunkBlocks& blocks, int chunk_x, int chunk_z, ChunkFill& fill, int x, int y, int z, bool under_fluid, uint16_t& out, const std::function<int(int, int, int)>* lookup) const {
    const Impl& I = *p_;
    SCtx c;
    c.sys = &I;
    c.fill = &fill;
    c.blocks = &blocks;
    c.chunk_x = chunk_x;
    c.chunk_z = chunk_z;
    c.lookup = lookup;
    c.caches.assign(size_t(I.tree->slots), SCtx::Cache());
    c.block_x = x;
    c.block_z = z;
    c.xz_epoch++;
    c.y_epoch++;
    c.surface_depth = I.get_surface_depth(x, z);
    c.block_y = y;
    c.water_height = under_fluid ? y + 1 : INT_MIN;
    c.stone_depth_below = 1;
    c.stone_depth_above = 1;
    return I.tree->root->apply(c, x, y, z, out);
}

void SurfaceSystem::build(ChunkBlocks& blocks, int chunk_x, int chunk_z, ChunkFill& fill, const std::function<int(int, int, int)>* lookup) const {
    const Impl& I = *p_;
    const int min_y = I.min_y;
    SCtx c;
    c.sys = &I;
    c.fill = &fill;
    c.blocks = &blocks;
    c.chunk_x = chunk_x;
    c.chunk_z = chunk_z;
    c.lookup = lookup;
    c.caches.assign(size_t(I.tree->slots), SCtx::Cache());
    auto get = [&](int x, int y, int z) { return blocks.get(x, y, z); };
    auto set = [&](int x, int y, int z, uint16_t st) { blocks.set(x, y, z, st); };
    const int min_bx = chunk_x * 16, min_bz = chunk_z * 16;
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++) {
            int bx = min_bx + x, bz = min_bz + z;
            int starting = blocks.hm[WORLD_SURFACE_WG][z * 16 + x];
            int surface_biome = lookup ? I.get_biome(bx, I.legacy ? 0 : starting, bz, *lookup) : I.get_biome(bx, I.legacy ? 0 : starting, bz);
            if (surface_biome == I.eroded_badlands && I.eroded_badlands >= 0) {
                // erodedBadlandsExtension
                double pillar_buffer = std::min(std::fabs(I.badlands_surface->get_value(bx, 0.0, bz) * 8.25), I.pillar->get_value(bx * 0.2, 0.0, bz * 0.2) * 15.0);
                if (!(pillar_buffer <= 0.0)) {
                    double pillar_floor = std::fabs(I.pillar_roof->get_value(bx * 0.75, 0.0, bz * 0.75) * 1.5);
                    double top = 64.0 + std::min(pillar_buffer * pillar_buffer * 2.5, std::ceil(pillar_floor * 50.0) + 24.0);
                    int start_y = mth_floor(top);
                    if (starting <= start_y) {
                        bool water_hit = false;
                        for (int y = start_y; y >= min_y; y--) {
                            uint16_t old = get(bx, y, bz);
                            if (old == I.default_block) break;
                            if (old == I.blocks.water) { water_hit = true; break; }
                        }
                        if (!water_hit)
                            for (int y = start_y; y >= min_y && I.air(get(bx, y, bz)); y--) set(bx, y, bz, I.default_block);
                    }
                }
            }
            int height = blocks.hm[WORLD_SURFACE_WG][z * 16 + x];
            c.block_x = bx;
            c.block_z = bz;
            c.xz_epoch++;
            c.y_epoch++;
            c.surface_depth = I.get_surface_depth(bx, bz);
            int stone_above = 0, water_height = INT_MIN, next_ceiling = INT_MAX;
            for (int y = height; y >= min_y; y--) {
                uint16_t old = get(bx, y, bz);
                if (std::getenv("PAINITE_DBG") && bx == 15 && bz == 8 && chunk_x == 0 && chunk_z == 0) fprintf(stderr, "col y=%d id=%d air=%d fluid=%d def=%d\n", y, int(old), int(I.air(old)), int(I.fluid(old)), int(old == I.default_block));
                if (I.air(old)) { stone_above = 0; water_height = INT_MIN; }
                else if (I.fluid(old)) { if (water_height == INT_MIN) water_height = y + 1; }
                else {
                    if (next_ceiling >= y) {
                        next_ceiling = kWayBelowMinY;
                        for (int look = y - 1; look >= min_y - 1; look--) {
                            uint16_t next = get(bx, look, bz);
                            if (I.air(next) || I.fluid(next)) { next_ceiling = look + 1; break; }
                        }
                    }
                    stone_above++;
                    int stone_below = y - next_ceiling + 1;
                    c.y_epoch++;
                    c.have_biome = false;
                    c.block_y = y;
                    c.water_height = water_height;
                    c.stone_depth_below = stone_below;
                    c.stone_depth_above = stone_above;
                    if (old == I.default_block) {
                        uint16_t st;
                        bool applied = I.tree->root->apply(c, bx, y, bz, st);
                        if (std::getenv("PAINITE_DBG") && bx == 15 && bz == 8 && chunk_x == 0 && chunk_z == 0) fprintf(stderr, "y=%d above=%d below=%d water=%d depth=%d applied=%d st=%d\n", y, stone_above, stone_below, water_height, c.surface_depth, int(applied), int(st));
                        if (applied) set(bx, y, bz, st);
                    }
                }
            }
            if (std::find(I.frozen_ocean_biomes.begin(), I.frozen_ocean_biomes.end(), surface_biome) != I.frozen_ocean_biomes.end()) {
                // frozenOceanExtension
                double iceberg = std::min(std::fabs(I.iceberg_surface->get_value(bx, 0.0, bz) * 8.25), I.iceberg_pillar->get_value(bx * 1.28, 0.0, bz * 1.28) * 15.0);
                if (!(iceberg <= 1.8)) {
                    double roof = std::fabs(I.iceberg_roof->get_value(bx * 1.17, 0.0, bz * 1.17) * 1.5);
                    double top = std::min(iceberg * iceberg * 1.2, std::ceil(roof * 40.0) + 14.0);
                    if (I.temperature_at(surface_biome, bx, I.sea_level, bz) > 0.1f) top -= 2.0;
                    double ext_bottom;
                    if (top > 2.0) { ext_bottom = I.sea_level - top - 7.0; top += I.sea_level; }
                    else { top = 0.0; ext_bottom = 0.0; }
                    double ext_top = top;
                    auto random = I.state.random().at(bx, 0, bz);
                    int max_snow = 2 + random->next_int(4);
                    int min_snow_height = I.sea_level + 18 + random->next_int(10);
                    int snow_depth = 0;
                    for (int y = std::max(starting, int(top) + 1); y >= min_surface_level(c); y--) {
                        uint16_t b = get(bx, y, bz);
                        if ((I.air(b) && y < int(ext_top) && random->next_double() > 0.01) ||
                            (b == I.blocks.water && y > int(ext_bottom) && y < I.sea_level && ext_bottom != 0.0 && random->next_double() > 0.15)) {
                            if (snow_depth <= max_snow && y > min_snow_height) { set(bx, y, bz, I.snow_block); snow_depth++; }
                            else set(bx, y, bz, I.packed_ice);
                        }
                    }
                }
            }
        }
}

}  // namespace pn::vg
