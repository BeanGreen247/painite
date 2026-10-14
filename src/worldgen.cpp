#include "worldgen.hpp"
#include "perf.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <random>

namespace pn {

uint64_t seed_from_string(const std::string& s) {
    if (s.empty()) { std::random_device rd; return (static_cast<uint64_t>(rd()) << 32) ^ rd(); }
    char* end = nullptr;
    long long v = strtoll(s.c_str(), &end, 10);
    if (end && !*end) return static_cast<uint64_t>(v);
    int32_t h = 0;
    for (unsigned char c : s) h = static_cast<int32_t>(31u * static_cast<uint32_t>(h) + c);
    return static_cast<uint64_t>(static_cast<int64_t>(h));
}

static uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t hash2(uint64_t seed, int64_t a, int64_t b) {
    uint64_t x = seed ^ (static_cast<uint64_t>(a) * 0x9e3779b97f4a7c15ull) ^ (static_cast<uint64_t>(b) * 0xc2b2ae3d27d4eb4full);
    return splitmix(x);
}

static double rnd01(uint64_t& rs) { return static_cast<double>(splitmix(rs) >> 11) / 9007199254740992.0; }
static int rndi(uint64_t& rs, int n) { return static_cast<int>(rnd01(rs) * n); }

static constexpr int SEA = 62;

// Biomes the generator can place. The order matches kBmNames.
enum Bm {
    PLAINS, FOREST, FLOWER_FOREST, BIRCH_FOREST, OLD_BIRCH, DARK_FOREST, TAIGA, SNOWY_TAIGA, OLD_SPRUCE, OLD_PINE, SNOWY_PLAINS, DESERT, SAVANNA, WINDSWEPT_SAVANNA,
    JUNGLE, SPARSE_JUNGLE, SWAMP, MANGROVE, BADLANDS, ERODED_BADLANDS, WOODED_BADLANDS, WINDSWEPT_HILLS, WINDSWEPT_GRAVELLY, WINDSWEPT_FOREST, MEADOW, CHERRY, GROVE,
    SNOWY_SLOPES, JAGGED, FROZEN_PEAKS, STONY_PEAKS, BEACH, SNOWY_BEACH, STONY_SHORE, RIVER, FROZEN_RIVER, OCEAN, DEEP_OCEAN, COLD_OCEAN, DEEP_COLD, FROZEN_OCEAN,
    DEEP_FROZEN, LUKEWARM, DEEP_LUKEWARM, WARM, MUSHROOM, BM_COUNT
};
static const char* const kBmNames[BM_COUNT] = {
    "plains", "forest", "flower_forest", "birch_forest", "old_growth_birch_forest", "dark_forest", "taiga", "snowy_taiga", "old_growth_spruce_taiga", "old_growth_pine_taiga",
    "snowy_plains", "desert", "savanna", "windswept_savanna", "jungle", "sparse_jungle", "swamp", "mangrove_swamp", "badlands", "eroded_badlands", "wooded_badlands",
    "windswept_hills", "windswept_gravelly_hills", "windswept_forest", "meadow", "cherry_grove", "grove", "snowy_slopes", "jagged_peaks", "frozen_peaks", "stony_peaks",
    "beach", "snowy_beach", "stony_shore", "river", "frozen_river", "ocean", "deep_ocean", "cold_ocean", "deep_cold_ocean", "frozen_ocean", "deep_frozen_ocean",
    "lukewarm_ocean", "deep_lukewarm_ocean", "warm_ocean", "mushroom_fields"};

static bool is_ocean(int b) { return b >= OCEAN && b <= WARM; }
static bool is_frozen_water(int b) { return b == FROZEN_OCEAN || b == DEEP_FROZEN || b == FROZEN_RIVER; }
static bool is_snowy(int b) { return b == SNOWY_PLAINS || b == SNOWY_TAIGA || b == SNOWY_BEACH; }
static bool is_badlands(int b) { return b == BADLANDS || b == ERODED_BADLANDS || b == WOODED_BADLANDS; }

WorldGen::WorldGen(GenKind kind, uint64_t seed, const std::vector<std::string>& biome_names) : kind_(kind), seed_(seed), names_(biome_names) {
    uint64_t s = seed;
    for (int ch = 0; ch < 6; ch++) {
        uint8_t p[256];
        for (int i = 0; i < 256; i++) p[i] = static_cast<uint8_t>(i);
        for (int i = 255; i > 0; i--) std::swap(p[i], p[splitmix(s) % static_cast<uint64_t>(i + 1)]);
        for (int i = 0; i < 512; i++) perm_[ch][i] = p[i & 255];
    }
    auto plains = [&]() -> uint8_t {
        for (size_t i = 0; i < biome_names.size(); i++) if (biome_names[i] == "minecraft:plains") return static_cast<uint8_t>(i);
        return 0;
    };
    for (int i = 0; i < BM_COUNT; i++) {
        std::string n = std::string("minecraft:") + kBmNames[i];
        bi_[i] = plains();
        for (size_t k = 0; k < biome_names.size(); k++) if (biome_names[k] == n) bi_[i] = static_cast<uint8_t>(k);
    }
    const Blocks& B = Blocks::get();
    auto d = [&](const char* n) { return B.default_state(n); };
    air_ = 0; bedrock_ = d("minecraft:bedrock"); stone_ = d("minecraft:stone"); deepslate_ = B.find("minecraft:deepslate", "axis=y"); dirt_ = d("minecraft:dirt");
    grass_ = B.find("minecraft:grass_block", "snowy=false"); sand_ = d("minecraft:sand"); sandstone_ = d("minecraft:sandstone"); gravel_ = d("minecraft:gravel");
    water_ = B.find("minecraft:water", "level=0"); lava_ = B.find("minecraft:lava", "level=0"); snow_ = B.find("minecraft:snow", "layers=1"); snow_block_ = d("minecraft:snow_block");
    short_grass_ = d("minecraft:short_grass"); fern_ = d("minecraft:fern");
    tall_grass_lo_ = B.find("minecraft:tall_grass", "half=lower"); tall_grass_hi_ = B.find("minecraft:tall_grass", "half=upper");
    large_fern_lo_ = B.find("minecraft:large_fern", "half=lower"); large_fern_hi_ = B.find("minecraft:large_fern", "half=upper");
    dandelion_ = d("minecraft:dandelion"); poppy_ = d("minecraft:poppy"); azure_ = d("minecraft:azure_bluet"); oxeye_ = d("minecraft:oxeye_daisy");
    cornflower_ = d("minecraft:cornflower"); allium_ = d("minecraft:allium"); orchid_ = d("minecraft:blue_orchid"); lily_valley_ = d("minecraft:lily_of_the_valley");
    lilac_lo_ = B.find("minecraft:lilac", "half=lower"); lilac_hi_ = B.find("minecraft:lilac", "half=upper");
    rose_lo_ = B.find("minecraft:rose_bush", "half=lower"); rose_hi_ = B.find("minecraft:rose_bush", "half=upper");
    peony_lo_ = B.find("minecraft:peony", "half=lower"); peony_hi_ = B.find("minecraft:peony", "half=upper");
    sunflower_lo_ = B.find("minecraft:sunflower", "half=lower"); sunflower_hi_ = B.find("minecraft:sunflower", "half=upper");
    coal_ = d("minecraft:coal_ore"); iron_ = d("minecraft:iron_ore"); copper_ = d("minecraft:copper_ore"); gold_ = d("minecraft:gold_ore");
    redstone_ = B.find("minecraft:redstone_ore", "lit=false"); diamond_ = d("minecraft:diamond_ore"); lapis_ = d("minecraft:lapis_ore");
    deep_coal_ = d("minecraft:deepslate_coal_ore"); deep_iron_ = d("minecraft:deepslate_iron_ore"); deep_gold_ = d("minecraft:deepslate_gold_ore");
    deep_diamond_ = d("minecraft:deepslate_diamond_ore"); deep_redstone_ = B.find("minecraft:deepslate_redstone_ore", "lit=false");
    deep_lapis_ = d("minecraft:deepslate_lapis_ore"); deep_copper_ = d("minecraft:deepslate_copper_ore");
    cactus_ = B.find("minecraft:cactus", "age=0"); red_sand_ = d("minecraft:red_sand"); clay_ = d("minecraft:clay");
    podzol_ = B.find("minecraft:podzol", "snowy=false"); mycelium_ = B.find("minecraft:mycelium", "snowy=false"); coarse_dirt_ = d("minecraft:coarse_dirt"); mud_ = d("minecraft:mud");
    ice_ = d("minecraft:ice"); packed_ice_ = d("minecraft:packed_ice"); blue_ice_ = d("minecraft:blue_ice");
    sugar_cane_ = B.find("minecraft:sugar_cane", "age=0"); lily_pad_ = d("minecraft:lily_pad");
    kelp_ = B.find("minecraft:kelp", "age=25"); kelp_plant_ = d("minecraft:kelp_plant"); seagrass_ = d("minecraft:seagrass");
    tall_seagrass_lo_ = B.find("minecraft:tall_seagrass", "half=lower"); tall_seagrass_hi_ = B.find("minecraft:tall_seagrass", "half=upper");
    dead_bush_ = d("minecraft:dead_bush"); pumpkin_ = d("minecraft:pumpkin"); melon_ = d("minecraft:melon");
    granite_ = d("minecraft:granite"); diorite_ = d("minecraft:diorite"); andesite_ = d("minecraft:andesite"); tuff_ = d("minecraft:tuff"); calcite_ = d("minecraft:calcite");
    const char* tc[8] = {"minecraft:terracotta", "minecraft:orange_terracotta", "minecraft:white_terracotta", "minecraft:yellow_terracotta", "minecraft:brown_terracotta",
                         "minecraft:red_terracotta", "minecraft:light_gray_terracotta", "minecraft:terracotta"};
    for (int i = 0; i < 8; i++) terracotta_[i] = d(tc[i]);
    brown_mushroom_ = d("minecraft:brown_mushroom"); red_mushroom_ = d("minecraft:red_mushroom"); moss_block_ = d("minecraft:moss_block"); bamboo_ = d("minecraft:bamboo");
    auto wood = [&](const char* n) {
        std::string w = n;
        return Wood{B.find("minecraft:" + w + "_log", "axis=y"), B.find("minecraft:" + w + "_leaves", "distance=1,persistent=true,waterlogged=false")};
    };
    oak_ = wood("oak"); birch_ = wood("birch"); spruce_ = wood("spruce"); jungle_ = wood("jungle"); acacia_ = wood("acacia"); dark_oak_ = wood("dark_oak");
    cherry_ = wood("cherry"); mangrove_ = wood("mangrove");
    // terracotta bands: runs of one to three layers of the same colour, fixed by the seed
    {
        uint64_t r = seed ^ 0xbadc0ffee;
        for (int i = 0; i < 64;) {
            uint8_t col = static_cast<uint8_t>(rndi(r, 7));
            int run = 1 + rndi(r, 3);
            for (int k = 0; k < run && i < 64; k++, i++) bands_[i] = col;
        }
    }
    soft_.assign(B.count(), 0);
    for (size_t i = 0; i < B.count(); i++) {
        const BlockInfo& bi = B.info(static_cast<uint16_t>(i));
        bool leaves = bi.name.size() > 7 && bi.name.compare(bi.name.size() - 7, 7, "_leaves") == 0;
        soft_[i] = (bi.air || leaves || ((bi.replaceable || bi.needs_floor) && !bi.fluid)) ? 1 : 0;
    }
    soft_[0] = 1;
}

// Gradient noise (improved Perlin), channel selects an independent permutation.
static double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static double smooth(double e0, double e1, double x) { double t = clamp((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }
static double grad2(int h, double x, double z) { switch (h & 7) { case 0: return x + z; case 1: return -x + z; case 2: return x - z; case 3: return -x - z; case 4: return x; case 5: return -x; case 6: return z; default: return -z; } }
static double grad3(int h, double x, double y, double z) {
    h &= 15;
    double u = h < 8 ? x : y, v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

double WorldGen::noise2(double x, double z, int ch) const {
    int xi = static_cast<int>(std::floor(x)), zi = static_cast<int>(std::floor(z));
    double xf = x - xi, zf = z - zi;
    xi &= 255; zi &= 255;
    const uint8_t* p = perm_[ch];
    int aa = p[p[xi] + zi], ab = p[p[xi] + zi + 1], ba = p[p[xi + 1] + zi], bb = p[p[xi + 1] + zi + 1];
    double u = fade(xf), v = fade(zf);
    return lerp(lerp(grad2(aa, xf, zf), grad2(ba, xf - 1, zf), u), lerp(grad2(ab, xf, zf - 1), grad2(bb, xf - 1, zf - 1), u), v) * 1.4;
}

double WorldGen::noise3(double x, double y, double z, int ch) const {
    int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y)), zi = static_cast<int>(std::floor(z));
    double xf = x - xi, yf = y - yi, zf = z - zi;
    xi &= 255; yi &= 255; zi &= 255;
    const uint8_t* p = perm_[ch];
    int a = p[xi] + yi, aa = p[a] + zi, ab = p[a + 1] + zi, b = p[xi + 1] + yi, ba = p[b] + zi, bb = p[b + 1] + zi;
    double u = fade(xf), v = fade(yf), w = fade(zf);
    return lerp(lerp(lerp(grad3(p[aa], xf, yf, zf), grad3(p[ba], xf - 1, yf, zf), u), lerp(grad3(p[ab], xf, yf - 1, zf), grad3(p[bb], xf - 1, yf - 1, zf), u), v),
                lerp(lerp(grad3(p[aa + 1], xf, yf, zf - 1), grad3(p[ba + 1], xf - 1, yf, zf - 1), u), lerp(grad3(p[ab + 1], xf, yf - 1, zf - 1), grad3(p[bb + 1], xf - 1, yf - 1, zf - 1), u), v), w);
}

double WorldGen::fbm2(double x, double z, int oct, int ch) const {
    double sum = 0, amp = 1, norm = 0, f = 1;
    for (int i = 0; i < oct; i++) { sum += amp * noise2(x * f, z * f, ch); norm += amp; amp *= 0.5; f *= 2; }
    return sum / norm;
}

// The climate parameters follow vanilla's idea: continentalness (ocean to deep inland), erosion (flat to rugged), weirdness (folded into
// peaks and valleys; rivers run along its zero line), temperature and humidity. Height and biome both come from them.
WorldGen::Column WorldGen::column(int x, int z) const {
    double bs = kind_ == GenKind::LargeBiomes ? 4.0 : 1.0;
    auto g = [](double v, double gain) { return clamp(v * gain, -1.0, 1.0); };
    double cont = g(fbm2(x / (1500.0 * bs), z / (1500.0 * bs), 5, 0), 2.0);
    double eros = g(fbm2((x + 9000) / (800.0 * bs), (z - 4000) / (800.0 * bs), 4, 4), 2.0);
    double weird = g(fbm2((x - 7000) / (500.0 * bs), (z + 2500) / (500.0 * bs), 3, 5), 2.6);
    double temp = g(fbm2(x / (1800.0 * bs), z / (1800.0 * bs), 3, 3), 2.0);
    double humid = g(fbm2((x + 5000) / (1300.0 * bs), (z - 3000) / (1300.0 * bs), 3, 2), 2.0);
    double pv = 1.0 - 3.0 * std::fabs(std::fabs(weird) - 2.0 / 3.0);
    // base height from continentalness
    static const double pts[][2] = {{-1.0, 22}, {-0.6, 34}, {-0.455, 44}, {-0.19, 56}, {-0.11, 63}, {0.03, 68}, {0.3, 78}, {1.0, 90}};
    double base = pts[7][1];
    for (int i = 1; i < 8; i++)
        if (cont <= pts[i][0]) { base = lerp(pts[i - 1][1], pts[i][1], (cont - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0])); break; }
    double landf = smooth(-0.15, 0.25, cont);
    double amp = lerp(4.0, 34.0, (1.0 - eros) / 2.0);
    double mount = smooth(0.3, 0.85, pv) * smooth(0.05, -0.4, eros) * smooth(0.05, 0.4, cont);
    double detail = fbm2(x / 45.0, z / 45.0, 3, 1) * (1.5 + 3.5 * (1.0 - eros) / 2.0);
    double h = base + pv * amp * landf + mount * 120.0 + detail * (0.3 + 0.7 * landf);
    double river = (1.0 - smooth(0.0, 0.07, std::fabs(weird))) * smooth(-0.2, -0.08, cont) * (1.0 - smooth(0.15, 0.45, mount));
    if (h > 57) h = lerp(h, 57.0, river);
    if (kind_ == GenKind::Amplified && h > 64) h = 64 + (h - 64) * 2.4;
    int hi = static_cast<int>(std::floor(clamp(h, kMinY + 8, kMinY + kHeight - 4)));
    Column c{hi, 0, cont, eros, weird, pv, temp, humid, mount, river};
    c.bm = pick_biome(c, hi);
    return c;
}

int WorldGen::pick_biome(const Column& c, int h) const {
    static const double TT[4] = {-0.45, -0.15, 0.2, 0.55}, HT[4] = {-0.35, -0.1, 0.1, 0.3};
    int T = 0, H = 0;
    while (T < 4 && c.temp >= TT[T]) T++;
    while (H < 4 && c.humid >= HT[H]) H++;
    auto ocean = [&](bool deep) -> int {
        switch (T) {
            case 0: return deep ? DEEP_FROZEN : FROZEN_OCEAN;
            case 1: return deep ? DEEP_COLD : COLD_OCEAN;
            case 2: return deep ? DEEP_OCEAN : OCEAN;
            case 3: return deep ? DEEP_LUKEWARM : LUKEWARM;
            default: return deep ? DEEP_LUKEWARM : WARM;
        }
    };
    if (c.cont < -0.19) {
        if (c.cont < -0.88 && c.humid > 0.1 && T >= 1 && T <= 3) return MUSHROOM;
        return ocean(c.cont < -0.45);
    }
    int river = T == 0 ? FROZEN_RIVER : RIVER;
    if (h < SEA) return c.cont > -0.12 ? river : ocean(false);
    if (c.river > 0.3 && h <= SEA + 1) return river;
    if (h <= SEA + 3 && c.cont < -0.05) return T == 0 ? SNOWY_BEACH : (c.eros < -0.3 ? STONY_SHORE : BEACH);
    if (c.mount > 0.6) {
        if (T == 0) return c.weird > 0 ? JAGGED : FROZEN_PEAKS;
        if (T <= 2) return JAGGED;
        return STONY_PEAKS;
    }
    if (c.mount > 0.25) {
        if (T <= 1) return H >= 2 ? GROVE : SNOWY_SLOPES;
        if (T <= 3) return H >= 3 && T == 2 ? CHERRY : MEADOW;
    }
    if (c.eros < -0.35 && c.cont > 0.1 && T < 4) {
        if (T == 3) return WINDSWEPT_SAVANNA;
        if (H >= 3) return WINDSWEPT_FOREST;
        return c.weird > 0.45 ? WINDSWEPT_GRAVELLY : WINDSWEPT_HILLS;
    }
    if (c.eros > 0.2 && h <= SEA + 5 && (T == 2 || T == 3) && H >= 3) return T == 3 ? MANGROVE : SWAMP;
    if (T == 4) {
        if (H <= 1 && c.eros < 0.35) return c.eros < -0.25 ? ERODED_BADLANDS : (H == 1 && c.eros > 0 ? WOODED_BADLANDS : BADLANDS);
        return DESERT;
    }
    static const int mid[5][5] = {{SNOWY_PLAINS, SNOWY_PLAINS, SNOWY_PLAINS, SNOWY_TAIGA, TAIGA},
                                  {PLAINS, PLAINS, FOREST, TAIGA, OLD_SPRUCE},
                                  {FLOWER_FOREST, PLAINS, FOREST, BIRCH_FOREST, DARK_FOREST},
                                  {SAVANNA, SAVANNA, FOREST, JUNGLE, JUNGLE},
                                  {DESERT, DESERT, DESERT, DESERT, DESERT}};
    int b = mid[T][H];
    if (T == 2 && H == 3 && c.weird > 0.4) b = OLD_BIRCH;
    if (T == 1 && H == 4 && c.weird > 0.2) b = OLD_PINE;
    if (T == 3 && H == 3 && c.eros > 0.0) b = SPARSE_JUNGLE;
    return b;
}

int WorldGen::surface_height(int x, int z) const {
    if (kind_ == GenKind::Flat) return -61;
    if (kind_ == GenKind::Void) return kMinY - 1;
    return column(x, z).height;
}

int WorldGen::sample(int x, int z, int& biome) const {
    Column c = column(x, z);
    biome = c.bm;
    return c.height;
}

bool WorldGen::find_spawn(int& sx, int& sz) const {
    if (kind_ == GenKind::Flat || kind_ == GenKind::Void) { sx = sz = 0; return true; }
    for (int r = 0; r <= 2048; r += 8)
        for (int a = 0; a < std::max(1, r / 4); a++) {
            double ang = a * 6.2831853 / std::max(1, r / 4);
            int x = static_cast<int>(std::lround(std::cos(ang) * r)), z = static_cast<int>(std::lround(std::sin(ang) * r));
            Column c = column(x, z);
            int b = c.bm;
            if (c.height >= SEA + 2 && c.height <= 90 && !is_ocean(b) && b != BEACH && b != SNOWY_BEACH && b != STONY_SHORE && b != RIVER && b != FROZEN_RIVER && b != MUSHROOM) {
                sx = x; sz = z; return true;
            }
        }
    sx = sz = 0;
    return false;
}

void WorldGen::flat(Chunk& c) const {
    uint16_t layers[4] = {bedrock_, dirt_, dirt_, grass_};
    c.sec[0] = std::make_unique<Section>();
    memset(c.sec[0]->biome, bi_[PLAINS], 64);
    for (int y = 0; y < 4; y++) for (int i = 0; i < 256; i++) c.sec[0]->b[(y << 8) | i] = layers[y];
    memset(c.air_biomes, bi_[PLAINS], sizeof c.air_biomes);
}

// Block for a given depth below the surface of a column (0 = the top block); stone when the biome has nothing special there.
uint16_t WorldGen::surface_block(int bm, int depth, int y, int wx, int wz, double patch) const {
    switch (bm) {
        case DESERT: return depth < 4 ? sand_ : depth < 9 ? sandstone_ : stone_;
        case BEACH: case SNOWY_BEACH: return depth < 4 ? sand_ : depth < 7 ? sandstone_ : stone_;
        case STONY_SHORE: case STONY_PEAKS: return depth < 3 && patch > 0.55 ? gravel_ : stone_;
        case JAGGED: return depth < 2 ? snow_block_ : stone_;
        case FROZEN_PEAKS: return depth < 2 ? (patch > 0.35 ? packed_ice_ : snow_block_) : stone_;
        case SNOWY_SLOPES: case GROVE: return depth == 0 ? snow_block_ : depth < 4 ? dirt_ : stone_;
        case WINDSWEPT_HILLS: case WINDSWEPT_FOREST: case WINDSWEPT_SAVANNA:
            if (y > 95 && patch > -0.1) return stone_;
            return depth == 0 ? grass_ : depth < 4 ? dirt_ : stone_;
        case WINDSWEPT_GRAVELLY: return depth < 4 && patch > -0.2 ? gravel_ : (depth == 0 ? grass_ : depth < 4 ? dirt_ : stone_);
        case BADLANDS: case ERODED_BADLANDS: case WOODED_BADLANDS: {
            if (depth > 14) return stone_;
            if (bm == WOODED_BADLANDS && y > SEA + 8) return depth == 0 ? grass_ : depth < 3 ? dirt_ : terracotta_[bands_[(y + 640 + (wx >> 3) % 3) & 63]];
            if (depth == 0 && y > SEA + 1 && bm != ERODED_BADLANDS) return red_sand_;
            return terracotta_[bands_[(y + 640) & 63]];
        }
        case MUSHROOM: return depth == 0 ? mycelium_ : depth < 4 ? dirt_ : stone_;
        case OLD_SPRUCE: case OLD_PINE:
            if (depth == 0) return patch > 0.2 ? podzol_ : patch < -0.45 ? coarse_dirt_ : grass_;
            return depth < 4 ? dirt_ : stone_;
        case MANGROVE: return depth < 2 ? mud_ : depth < 4 ? dirt_ : stone_;
        case RIVER: case FROZEN_RIVER:
            if (depth >= 3) return stone_;
            return patch > 0.35 ? clay_ : patch < -0.3 ? gravel_ : sand_;
        case DEEP_OCEAN: case DEEP_COLD: case DEEP_FROZEN: case DEEP_LUKEWARM:
            if (depth >= 3) return stone_;
            return patch > 0.55 ? clay_ : gravel_;
        case OCEAN: case COLD_OCEAN: case FROZEN_OCEAN: case LUKEWARM: case WARM:
            if (depth >= 3) return stone_;
            if (patch > 0.6) return clay_;
            if (patch < -0.45 && bm != WARM && bm != LUKEWARM) return gravel_;
            return sand_;
        case SNOWY_PLAINS: case SNOWY_TAIGA: return depth == 0 ? grass_ : depth < 4 ? dirt_ : stone_;
        default: return depth == 0 ? grass_ : depth < 4 ? dirt_ : stone_;
    }
}

// Tree shapes, relative to the ground block under the trunk (dy 0). Leaves are listed before logs so logs win where they overlap.
enum TreeType { T_OAK, T_FANCY, T_BIRCH, T_TALL_BIRCH, T_SPRUCE, T_BIG_SPRUCE, T_JUNGLE, T_ACACIA, T_DARK_OAK, T_CHERRY, T_MANGROVE };

void WorldGen::build_tree(int kind, uint64_t& rs, std::vector<Placed>& out) const {
    auto add = [&](int dx, int dy, int dz, uint16_t b) { out.push_back(Placed{static_cast<int8_t>(dx), static_cast<int8_t>(dz), static_cast<int16_t>(dy), b}); };
    auto blob = [&](double cx, double cy, double cz, double rx, double ry, uint16_t leaf, double trim) {
        for (int dy = static_cast<int>(std::floor(cy - ry)); dy <= static_cast<int>(std::ceil(cy + ry)); dy++)
            for (int dx = static_cast<int>(std::floor(cx - rx)); dx <= static_cast<int>(std::ceil(cx + rx)); dx++)
                for (int dz = static_cast<int>(std::floor(cz - rx)); dz <= static_cast<int>(std::ceil(cz + rx)); dz++) {
                    double ex = (dx - cx) / (rx + 0.5), ey = (dy - cy) / (ry + 0.5), ez = (dz - cz) / (rx + 0.5);
                    double dist = ex * ex + ey * ey + ez * ez;
                    if (dist > 1.0 || (dist > 1.0 - trim && rnd01(rs) < 0.5)) continue;
                    add(dx, dy, dz, leaf);
                }
    };
    auto small = [&](Wood w, int th) {
        for (int ld = th - 2; ld <= th + 1; ld++) {
            int r = ld >= th ? 1 : 2;
            for (int dx = -r; dx <= r; dx++) for (int dz = -r; dz <= r; dz++) {
                if (std::abs(dx) == r && std::abs(dz) == r && (ld >= th || rnd01(rs) < 0.5)) continue;
                add(dx, 1 + ld, dz, w.leaf);
            }
        }
        for (int dy = 1; dy <= th; dy++) add(0, dy, 0, w.log);
    };
    auto cone = [&](Wood w, int th, int max_r, double cx, double cz) {
        static const int pat[10] = {0, 1, 0, 1, 2, 1, 2, 3, 2, 3};
        for (int k = 0; k <= th - 2; k++) {
            int r = std::min(max_r, k < 10 ? pat[k] : pat[6 + (k % 4)]);
            int y = th + 1 - k;
            for (int dx = -r - 1; dx <= r + 1; dx++) for (int dz = -r - 1; dz <= r + 1; dz++) {
                double ex = dx - cx, ez = dz - cz;
                if (ex * ex + ez * ez > (r + 0.35) * (r + 0.35)) continue;
                add(dx, y, dz, w.leaf);
            }
        }
    };
    switch (kind) {
        case T_OAK: small(oak_, 4 + rndi(rs, 3)); break;
        case T_BIRCH: small(birch_, 5 + rndi(rs, 3)); break;
        case T_TALL_BIRCH: small(birch_, 9 + rndi(rs, 4)); break;
        case T_FANCY: {
            int th = 7 + rndi(rs, 5);
            blob(0, th, 0, 4, 3, oak_.leaf, 0.35);
            for (int dy = 1; dy <= th + 1; dy++) add(0, dy, 0, oak_.log);
            break;
        }
        case T_SPRUCE: {
            int th = 6 + rndi(rs, 5);
            cone(spruce_, th, 2, 0, 0);
            for (int dy = 1; dy <= th; dy++) add(0, dy, 0, spruce_.log);
            break;
        }
        case T_BIG_SPRUCE: {
            int th = 14 + rndi(rs, 10);
            cone(spruce_, th, 5, 0.5, 0.5);
            for (int dy = 1; dy <= th; dy++) for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) add(a, dy, b, spruce_.log);
            add(0, 0, 0, dirt_); add(1, 0, 0, dirt_); add(0, 0, 1, dirt_); add(1, 0, 1, dirt_);
            return;
        }
        case T_JUNGLE: {
            int th = 8 + rndi(rs, 8);
            blob(0, th, 0, 3.2, 2, jungle_.leaf, 0.3);
            blob(0, th + 2, 0, 2, 1, jungle_.leaf, 0.2);
            for (int dy = 1; dy <= th + 1; dy++) add(0, dy, 0, jungle_.log);
            break;
        }
        case T_ACACIA: {
            int th = 5 + rndi(rs, 3);
            int dir = rndi(rs, 4), ddx = dir == 0 ? 1 : dir == 1 ? -1 : 0, ddz = dir == 2 ? 1 : dir == 3 ? -1 : 0;
            blob(ddx * 2, th + 1, ddz * 2, 3.2, 0.8, acacia_.leaf, 0.3);
            blob(ddx * 2, th + 2, ddz * 2, 1.8, 0.5, acacia_.leaf, 0.2);
            for (int dy = 1; dy <= th - 2; dy++) add(0, dy, 0, acacia_.log);
            for (int dy = th - 2; dy <= th - 1; dy++) add(ddx, dy, ddz, acacia_.log);
            add(ddx * 2, th, ddz * 2, acacia_.log);
            add(ddx, th - 2, ddz, acacia_.log);
            break;
        }
        case T_DARK_OAK: {
            int th = 6 + rndi(rs, 3);
            blob(0.5, th, 0.5, 4.0, 1.8, dark_oak_.leaf, 0.35);
            for (int dy = 1; dy <= th; dy++) for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) add(a, dy, b, dark_oak_.log);
            add(0, 0, 0, dirt_); add(1, 0, 0, dirt_); add(0, 0, 1, dirt_); add(1, 0, 1, dirt_);
            return;
        }
        case T_CHERRY: {
            int th = 5 + rndi(rs, 3);
            blob(0, th, 0, 3.5, 2.5, cherry_.leaf, 0.35);
            for (int dy = 1; dy <= th; dy++) add(0, dy, 0, cherry_.log);
            break;
        }
        case T_MANGROVE: {
            int th = 4 + rndi(rs, 4);
            blob(0, th + 1, 0, 3, 2, mangrove_.leaf, 0.3);
            for (int dy = 1; dy <= th + 1; dy++) add(0, dy, 0, mangrove_.log);
            break;
        }
    }
    add(0, 0, 0, dirt_);
}

namespace {
struct TreeRule { int bm; double prob; int kinds[3]; int weights[3]; };
const TreeRule kTreeRules[] = {
    {PLAINS, 0.015, {T_OAK, T_FANCY, T_OAK}, {90, 10, 0}},
    {FOREST, 0.32, {T_OAK, T_BIRCH, T_FANCY}, {68, 22, 10}},
    {FLOWER_FOREST, 0.2, {T_OAK, T_BIRCH, T_OAK}, {40, 60, 0}},
    {BIRCH_FOREST, 0.35, {T_BIRCH, T_TALL_BIRCH, T_BIRCH}, {85, 15, 0}},
    {OLD_BIRCH, 0.35, {T_TALL_BIRCH, T_BIRCH, T_BIRCH}, {70, 30, 0}},
    {DARK_FOREST, 0.55, {T_DARK_OAK, T_BIRCH, T_FANCY}, {85, 10, 5}},
    {TAIGA, 0.4, {T_SPRUCE, T_SPRUCE, T_SPRUCE}, {100, 0, 0}},
    {SNOWY_TAIGA, 0.3, {T_SPRUCE, T_SPRUCE, T_SPRUCE}, {100, 0, 0}},
    {OLD_SPRUCE, 0.35, {T_BIG_SPRUCE, T_SPRUCE, T_SPRUCE}, {40, 60, 0}},
    {OLD_PINE, 0.35, {T_BIG_SPRUCE, T_SPRUCE, T_SPRUCE}, {40, 60, 0}},
    {SAVANNA, 0.05, {T_ACACIA, T_OAK, T_ACACIA}, {80, 20, 0}},
    {WINDSWEPT_SAVANNA, 0.03, {T_ACACIA, T_ACACIA, T_ACACIA}, {100, 0, 0}},
    {JUNGLE, 1.0, {T_JUNGLE, T_FANCY, T_JUNGLE}, {80, 20, 0}},
    {SPARSE_JUNGLE, 0.2, {T_JUNGLE, T_FANCY, T_JUNGLE}, {70, 30, 0}},
    {SWAMP, 0.12, {T_OAK, T_OAK, T_OAK}, {100, 0, 0}},
    {MANGROVE, 0.5, {T_MANGROVE, T_MANGROVE, T_MANGROVE}, {100, 0, 0}},
    {WINDSWEPT_FOREST, 0.22, {T_SPRUCE, T_OAK, T_SPRUCE}, {70, 30, 0}},
    {WINDSWEPT_HILLS, 0.03, {T_SPRUCE, T_SPRUCE, T_SPRUCE}, {100, 0, 0}},
    {MEADOW, 0.01, {T_OAK, T_OAK, T_OAK}, {100, 0, 0}},
    {CHERRY, 0.14, {T_CHERRY, T_CHERRY, T_CHERRY}, {100, 0, 0}},
    {GROVE, 0.25, {T_SPRUCE, T_SPRUCE, T_SPRUCE}, {100, 0, 0}},
    {WOODED_BADLANDS, 0.12, {T_OAK, T_OAK, T_OAK}, {100, 0, 0}},
};
const TreeRule* tree_rule(int bm) {
    static const std::array<const TreeRule*, BM_COUNT> table = [] {
        std::array<const TreeRule*, BM_COUNT> t{};
        for (const auto& r : kTreeRules) t[static_cast<size_t>(r.bm)] = &r;
        return t;
    }();
    return table[static_cast<size_t>(bm)];
}
}  // namespace

// Trees come from the 4x4-block cells of this chunk and its eight neighbours, each cell holding at most one tree at a position fixed
// by the seed, so a tree that crosses a chunk border is drawn the same way from both sides.
void WorldGen::put_trees(Chunk& c, const Column (*cols)[18]) const {
    std::vector<Placed> shape;
    int x0 = c.x * 16, z0 = c.z * 16;
    for (int nz = -1; nz <= 1; nz++)
        for (int nx = -1; nx <= 1; nx++) {
            uint64_t rs = hash2(seed_ ^ 0x7ee5eed, c.x + nx, c.z + nz);
            for (int cell = 0; cell < 16; cell++) {
                int lx = (cell & 3) * 4 + rndi(rs, 4), lz = (cell >> 2) * 4 + rndi(rs, 4);
                double roll = rnd01(rs), kroll = rnd01(rs);
                uint64_t trs = splitmix(rs);
                int wx = (c.x + nx) * 16 + lx, wz = (c.z + nz) * 16 + lz;
                if (wx < x0 - 6 || wx > x0 + 21 || wz < z0 - 6 || wz > z0 + 21) continue;
                Column own;
                const Column* col;
                if (nx == 0 && nz == 0) col = &cols[lz + 1][lx + 1];
                else { own = column(wx, wz); col = &own; }
                const TreeRule* rule = tree_rule(col->bm);
                if (!rule || roll >= rule->prob) continue;
                int h = col->height;
                bool wet = col->bm == MANGROVE;
                if (wet ? (h < SEA - 3 || h > SEA + 1) : h <= SEA) continue;
                int k = rule->kinds[0], w = static_cast<int>(kroll * 100);
                if (w >= rule->weights[0]) k = rule->kinds[1];
                if (w >= rule->weights[0] + rule->weights[1]) k = rule->kinds[2];
                shape.clear();
                build_tree(k, trs, shape);
                for (const Placed& p : shape) {
                    int x = wx + p.dx - x0, z = wz + p.dz - z0, y = h + p.dy;
                    if (x < 0 || x > 15 || z < 0 || z > 15 || y < kMinY || y >= kMinY + kHeight) continue;
                    uint16_t cur = c.get(x, y, z);
                    if (p.dy == 0) { if (cur == grass_ || cur == podzol_ || cur == coarse_dirt_ || cur == mud_) { int s = (y - kMinY) >> 4; c.sec[s]->b[((y & 15) << 8) | (z << 4) | x] = p.b; } continue; }
                    bool ok = cur < soft_.size() && soft_[cur];
                    if (!ok && wet && cur == water_) ok = true;
                    if (!ok) continue;
                    int s = (y - kMinY) >> 4;
                    if (!c.sec[s]) { c.sec[s] = std::make_unique<Section>(); memcpy(c.sec[s]->biome, c.air_biomes[s], 64); }
                    c.sec[s]->b[((y & 15) << 8) | (z << 4) | x] = p.b;
                }
            }
        }
}

void WorldGen::terrain(Chunk& c) const {
    PN_PERF("chunk.generate");
    memset(c.air_biomes, bi_[PLAINS], sizeof c.air_biomes);
    // 18x18 columns: the chunk and a one block border (neighbour heights for shore decoration)
    Column cols[18][18];
    for (int z = 0; z < 18; z++) for (int x = 0; x < 18; x++) cols[z][x] = column(c.x * 16 + x - 1, c.z * 16 + z - 1);
    auto sec_for = [&](int y) -> Section& {
        int s = (y - kMinY) >> 4;
        if (!c.sec[s]) { c.sec[s] = std::make_unique<Section>(); memset(c.sec[s]->biome, bi_[PLAINS], 64); }
        return *c.sec[s];
    };
    auto put = [&](int x, int y, int z, uint16_t b) { if (y >= kMinY && y < kMinY + kHeight) sec_for(y).b[((y & 15) << 8) | (z << 4) | x] = b; };
    auto get = [&](int x, int y, int z) -> uint16_t { return c.get(x, y, z); };
    uint64_t rng = hash2(seed_, c.x, c.z);
    auto rnd = [&]() { return rnd01(rng); };

    // cave noise on a coarse grid (4 blocks horizontally and vertically), interpolated per block
    constexpr int NY = 31, CY0 = -60;
    static thread_local float cheese[5][5][NY], spa[5][5][NY], spb[5][5][NY];
    for (int gz = 0; gz < 5; gz++)
        for (int gx = 0; gx < 5; gx++) {
            double wx = c.x * 16 + gx * 4, wz = c.z * 16 + gz * 4;
            for (int gy = 0; gy < NY; gy++) {
                double y = CY0 + gy * 4;
                cheese[gz][gx][gy] = static_cast<float>(noise3(wx / 40.0, y / 26.0, wz / 40.0, 1) + 0.35 * noise3(wx / 16.0, y / 12.0, wz / 16.0, 3));
                spa[gz][gx][gy] = static_cast<float>(noise3(wx / 28.0, y / 20.0, wz / 28.0, 2));
                spb[gz][gx][gy] = static_cast<float>(noise3(wx / 28.0 + 100, y / 20.0, wz / 28.0 + 100, 4));
            }
        }

    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            const Column& col = cols[z + 1][x + 1];
            int wx = c.x * 16 + x, wz = c.z * 16 + z;
            int h = std::min(col.height, kMinY + kHeight - 2);
            int bm = col.bm;
            double patch = noise2(wx / 12.0, wz / 12.0, 1);
            for (int y = kMinY; y <= h; y++) {
                uint16_t b;
                if (y < kMinY + 5) b = y < kMinY + 1 + static_cast<int>(rnd() * 4) ? bedrock_ : (y < -8 ? deepslate_ : stone_);
                else if (y < -8) b = deepslate_;
                else if (y < 0) b = (rnd() * 8 < -y) ? deepslate_ : stone_;
                else b = stone_;
                if (b == stone_ || b == deepslate_) {
                    int depth = h - y;
                    if (depth < 15) {
                        uint16_t sb = surface_block(bm, depth, y, wx, wz, patch);
                        if (sb != stone_) b = sb;
                    }
                }
                put(x, y, z, b);
            }
            // caves, never through the top layers so the ground stays intact
            double fx = (x & 3) / 4.0, fz = (z & 3) / 4.0;
            int gx = x >> 2, gz = z >> 2;
            for (int y = std::max(kMinY + 5, CY0); y < h - 6 && y < CY0 + (NY - 1) * 4; y++) {
                int gy = (y - CY0) >> 2;
                double fy = ((y - CY0) & 3) / 4.0;
                auto tri = [&](float (*a)[5][NY]) {
                    double c00 = lerp(a[gz][gx][gy], a[gz][gx][gy + 1], fy), c10 = lerp(a[gz][gx + 1][gy], a[gz][gx + 1][gy + 1], fy);
                    double c01 = lerp(a[gz + 1][gx][gy], a[gz + 1][gx][gy + 1], fy), c11 = lerp(a[gz + 1][gx + 1][gy], a[gz + 1][gx + 1][gy + 1], fy);
                    return lerp(lerp(c00, c10, fx), lerp(c01, c11, fx), fz);
                };
                double near_top = smooth(h - 40, h - 7, y);        // caves get rarer towards the surface
                double ch = tri(cheese);
                bool carve = ch > 0.56 + 0.4 * near_top;
                if (!carve) {
                    double sw = 0.07 * (1.0 - 0.6 * near_top);
                    carve = std::fabs(tri(spa)) < sw && std::fabs(tri(spb)) < sw;
                }
                if (carve) put(x, y, z, y < -54 ? lava_ : air_);
            }
            // sea
            for (int y = h + 1; y <= SEA; y++) put(x, y, z, water_);
            if (h < SEA && is_frozen_water(bm)) put(x, SEA, z, ice_);
            // surface decoration
            if (h >= SEA && h > 0 && h < kMinY + kHeight - 4 && !is_ocean(bm)) {
                uint16_t top = get(x, h, z);
                double r = rnd();
                auto flower = [&](double r2) -> uint16_t {
                    static const int n = 7;
                    uint16_t f[n] = {dandelion_, poppy_, azure_, oxeye_, cornflower_, allium_, orchid_};
                    return f[static_cast<int>(r2 * n) % n];
                };
                auto tall = [&](uint16_t lo, uint16_t hi) { if (get(x, h + 2, z) == air_) { put(x, h + 1, z, lo); put(x, h + 2, z, hi); } };
                if (top == grass_ || top == podzol_ || top == coarse_dirt_ ) {
                    bool grassy = top == grass_;
                    if (is_snowy(bm) && grassy) put(x, h + 1, z, snow_);
                    else if (bm == PLAINS) {
                        if (r < 0.14) put(x, h + 1, z, short_grass_);
                        else if (r < 0.19) tall(tall_grass_lo_, tall_grass_hi_);
                        else if (r < 0.215) put(x, h + 1, z, flower(rnd()));
                        else if (r < 0.2155) put(x, h + 1, z, pumpkin_);
                        else if (patch > 0.55 && r < 0.26) tall(sunflower_lo_, sunflower_hi_);
                    } else if (bm == FOREST || bm == BIRCH_FOREST || bm == OLD_BIRCH || bm == DARK_FOREST || bm == WINDSWEPT_FOREST) {
                        if (r < 0.06) put(x, h + 1, z, short_grass_);
                        else if (r < 0.075) put(x, h + 1, z, fern_);
                        else if (r < 0.085) tall(tall_grass_lo_, tall_grass_hi_);
                        else if (r < 0.0865) put(x, h + 1, z, bm == DARK_FOREST ? (rnd() < 0.5 ? brown_mushroom_ : red_mushroom_) : lily_valley_);
                        else if (r < 0.092 && bm == FOREST) put(x, h + 1, z, rnd() < 0.5 ? dandelion_ : poppy_);
                    } else if (bm == FLOWER_FOREST) {
                        if (r < 0.04) put(x, h + 1, z, short_grass_);
                        else if (r < 0.11) put(x, h + 1, z, flower(rnd()));
                        else if (r < 0.125) { double k = rnd(); if (k < 0.34) tall(lilac_lo_, lilac_hi_); else if (k < 0.67) tall(rose_lo_, rose_hi_); else tall(peony_lo_, peony_hi_); }
                    } else if (bm == TAIGA || bm == SNOWY_TAIGA || bm == OLD_SPRUCE || bm == OLD_PINE || bm == GROVE) {
                        if (bm == SNOWY_TAIGA || bm == GROVE) { if (grassy) put(x, h + 1, z, snow_); }
                        else if (r < 0.05) put(x, h + 1, z, short_grass_);
                        else if (r < 0.14) put(x, h + 1, z, fern_);
                        else if (r < 0.16) tall(large_fern_lo_, large_fern_hi_);
                        else if (r < 0.1625) put(x, h + 1, z, rnd() < 0.5 ? brown_mushroom_ : red_mushroom_);
                    } else if (bm == SAVANNA || bm == WINDSWEPT_SAVANNA) {
                        if (r < 0.15) put(x, h + 1, z, short_grass_);
                        else if (r < 0.27) tall(tall_grass_lo_, tall_grass_hi_);
                    } else if (bm == JUNGLE || bm == SPARSE_JUNGLE) {
                        if (r < 0.08) put(x, h + 1, z, short_grass_);
                        else if (r < 0.16) put(x, h + 1, z, fern_);
                        else if (r < 0.22) tall(tall_grass_lo_, tall_grass_hi_);
                        else if (r < 0.255) tall(large_fern_lo_, large_fern_hi_);
                        else if (r < 0.2565 && bm == JUNGLE) put(x, h + 1, z, melon_);
                    } else if (bm == SWAMP) {
                        if (r < 0.1) put(x, h + 1, z, short_grass_);
                        else if (r < 0.115) put(x, h + 1, z, orchid_);
                        else if (r < 0.12) put(x, h + 1, z, rnd() < 0.5 ? brown_mushroom_ : red_mushroom_);
                    } else if (bm == MEADOW || bm == CHERRY) {
                        if (r < 0.13) put(x, h + 1, z, short_grass_);
                        else if (r < 0.17) tall(tall_grass_lo_, tall_grass_hi_);
                        else if (r < 0.24) { uint16_t f[4] = {azure_, cornflower_, dandelion_, allium_}; put(x, h + 1, z, f[static_cast<int>(rnd() * 4)]); }
                    } else if (bm == WINDSWEPT_HILLS || bm == WINDSWEPT_GRAVELLY) {
                        if (r < 0.05 && grassy) put(x, h + 1, z, short_grass_);
                    } else if (bm == WOODED_BADLANDS) {
                        if (r < 0.06) put(x, h + 1, z, short_grass_);
                    }
                } else if (top == sand_ || top == red_sand_) {
                    if (bm == DESERT && r < 0.012) put(x, h + 1, z, dead_bush_);
                    else if (bm == DESERT && r < 0.022) { int k = 1 + static_cast<int>(rnd() * 3); for (int i = 1; i <= k; i++) put(x, h + i, z, cactus_); }
                    else if (is_badlands(bm) && r < 0.02) put(x, h + 1, z, dead_bush_);
                    else if (is_badlands(bm) && r < 0.03) { int k = 1 + static_cast<int>(rnd() * 3); for (int i = 1; i <= k; i++) put(x, h + i, z, cactus_); }
                } else if (is_badlands(bm) && r < 0.012 && bm != WOODED_BADLANDS) put(x, h + 1, z, dead_bush_);
                // sugar cane beside water
                if (h == SEA && (top == grass_ || top == sand_ || top == dirt_ || top == red_sand_) && bm != SNOWY_BEACH && !is_snowy(bm)
                    && (cols[z + 1][x].height < SEA || cols[z + 1][x + 2].height < SEA || cols[z][x + 1].height < SEA || cols[z + 2][x + 1].height < SEA) && get(x, h + 1, z) == air_
                    && rnd() < 0.18) {
                    int k = 2 + static_cast<int>(rnd() * 3);
                    for (int i = 1; i <= k; i++) put(x, h + i, z, sugar_cane_);
                }
            } else if (h < SEA) {
                // underwater: seagrass, kelp, lily pads
                double r = rnd();
                uint16_t floor_b = get(x, h, z);
                if (bm == SWAMP || bm == MANGROVE) {
                    if (h >= SEA - 3 && r < 0.06 && floor_b != air_) put(x, SEA + 1, z, lily_pad_);
                } else if (!is_frozen_water(bm) && h > kMinY + 10 && floor_b != air_) {
                    bool deep = bm == DEEP_OCEAN || bm == DEEP_COLD || bm == DEEP_LUKEWARM;
                    if (r < (deep ? 0.04 : 0.28)) put(x, h + 1, z, seagrass_);
                    else if (r < (deep ? 0.045 : 0.33) && h + 2 < SEA) { put(x, h + 1, z, tall_seagrass_lo_); put(x, h + 2, z, tall_seagrass_hi_); }
                    else if (bm != WARM && bm != LUKEWARM && bm != DEEP_LUKEWARM && r < 0.33 + 0.045 && h < SEA - 4) {
                        int k = 3 + static_cast<int>(rnd() * 8);
                        int top_y = std::min(h + k, SEA - 1);
                        for (int y = h + 1; y < top_y; y++) put(x, y, z, kelp_plant_);
                        put(x, top_y, z, kelp_);
                    }
                }
            }
        }
    // biome cells (4x4x4) follow the climate at the middle of each 4x4 column group
    for (int cz = 0; cz < 4; cz++)
        for (int cx = 0; cx < 4; cx++) {
            uint8_t bm = bi_[cols[cz * 4 + 2][cx * 4 + 2].bm];
            for (int s = 0; s < kSections; s++)
                for (int cy = 0; cy < 4; cy++) {
                    int i = cy * 16 + cz * 4 + cx;
                    c.air_biomes[s][i] = bm;
                    if (c.sec[s]) c.sec[s]->biome[i] = bm;
                }
        }
    // ores and rock blobs: random walks inside the chunk, more of the cheap ones high up
    struct Ore { uint16_t stone, deep; int tries, minY, maxY, size; };
    Ore ores[] = {{coal_, deep_coal_, 14, 0, 128, 8}, {iron_, deep_iron_, 12, -32, 64, 6}, {copper_, deep_copper_, 8, -16, 80, 7}, {gold_, deep_gold_, 3, -60, 30, 5},
                  {redstone_, deep_redstone_, 6, -60, 14, 5}, {lapis_, deep_lapis_, 2, -50, 30, 4}, {diamond_, deep_diamond_, 3, -60, 12, 3}};
    auto blob_in_rock = [&](uint16_t stone_to, uint16_t deep_to, int tries, int minY, int maxY, int size) {
        for (int t = 0; t < tries; t++) {
            int ox = static_cast<int>(rnd() * 16), oz = static_cast<int>(rnd() * 16), oy = minY + static_cast<int>(rnd() * (maxY - minY));
            int x = ox, y = oy, z = oz;
            for (int k = 0; k < size; k++) {
                x += static_cast<int>(rnd() * 3) - 1; y += static_cast<int>(rnd() * 3) - 1; z += static_cast<int>(rnd() * 3) - 1;
                if (x < 0 || x > 15 || z < 0 || z > 15 || y < kMinY + 5) continue;
                uint16_t cur = get(x, y, z);
                if (cur == stone_) put(x, y, z, stone_to);
                else if (cur == deepslate_ && deep_to) put(x, y, z, deep_to);
            }
        }
    };
    for (auto& o : ores) blob_in_rock(o.stone, o.deep, o.tries, o.minY, o.maxY, o.size);
    blob_in_rock(granite_, 0, 4, -30, 90, 40); blob_in_rock(diorite_, 0, 4, -30, 90, 40); blob_in_rock(andesite_, 0, 4, -30, 90, 40);
    blob_in_rock(tuff_, tuff_, 3, -64, 0, 40); blob_in_rock(dirt_, 0, 3, 0, 120, 30); blob_in_rock(gravel_, gravel_, 3, -60, 120, 30);
    put_trees(c, cols);
}

void WorldGen::generate(Chunk& c) const {
    c.extra = Nbt::compound();
    c.extra.put("Status", Nbt::str("minecraft:full"));
    if (kind_ == GenKind::Void) { memset(c.air_biomes, bi_[PLAINS], sizeof c.air_biomes); return; }
    if (kind_ == GenKind::Flat) { flat(c); return; }
    terrain(c);
}

}  // namespace pn
