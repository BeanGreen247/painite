#include "worldgen.hpp"
#include "perf.hpp"
#include <algorithm>
#include <cmath>
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

WorldGen::WorldGen(GenKind kind, uint64_t seed, const std::vector<std::string>& biome_names) : kind_(kind), seed_(seed), names_(biome_names) {
    uint64_t s = seed;
    for (int ch = 0; ch < 4; ch++) {
        uint8_t p[256];
        for (int i = 0; i < 256; i++) p[i] = static_cast<uint8_t>(i);
        for (int i = 255; i > 0; i--) std::swap(p[i], p[splitmix(s) % static_cast<uint64_t>(i + 1)]);
        for (int i = 0; i < 512; i++) perm_[ch][i] = p[i & 255];
    }
    auto bio = [&](const char* n) -> uint8_t {
        for (size_t i = 0; i < biome_names.size(); i++) if (biome_names[i] == n) return static_cast<uint8_t>(i);
        for (size_t i = 0; i < biome_names.size(); i++) if (biome_names[i] == "minecraft:plains") return static_cast<uint8_t>(i);
        return 0;
    };
    b_plains_ = bio("minecraft:plains"); b_forest_ = bio("minecraft:forest"); b_desert_ = bio("minecraft:desert"); b_snowy_ = bio("minecraft:snowy_plains");
    b_ocean_ = bio("minecraft:ocean"); b_deep_ocean_ = bio("minecraft:deep_ocean"); b_beach_ = bio("minecraft:beach"); b_cold_ocean_ = bio("minecraft:cold_ocean");
    const Blocks& B = Blocks::get();
    auto d = [&](const char* n) { return B.default_state(n); };
    air_ = 0; bedrock_ = d("minecraft:bedrock"); stone_ = d("minecraft:stone"); deepslate_ = B.find("minecraft:deepslate", "axis=y"); dirt_ = d("minecraft:dirt");
    grass_ = B.find("minecraft:grass_block", "snowy=false"); sand_ = d("minecraft:sand"); sandstone_ = d("minecraft:sandstone"); gravel_ = d("minecraft:gravel");
    water_ = B.find("minecraft:water", "level=0"); lava_ = B.find("minecraft:lava", "level=0"); snow_ = B.find("minecraft:snow", "layers=1"); snow_block_ = d("minecraft:snow_block");
    log_ = B.find("minecraft:oak_log", "axis=y"); leaves_ = B.find("minecraft:oak_leaves", "distance=1,persistent=true,waterlogged=false");
    short_grass_ = d("minecraft:short_grass"); dandelion_ = d("minecraft:dandelion"); poppy_ = d("minecraft:poppy");
    coal_ = d("minecraft:coal_ore"); iron_ = d("minecraft:iron_ore"); copper_ = d("minecraft:copper_ore"); gold_ = d("minecraft:gold_ore");
    redstone_ = B.find("minecraft:redstone_ore", "lit=false"); diamond_ = d("minecraft:diamond_ore"); lapis_ = d("minecraft:lapis_ore");
    deep_coal_ = d("minecraft:deepslate_coal_ore"); deep_iron_ = d("minecraft:deepslate_iron_ore"); deep_gold_ = d("minecraft:deepslate_gold_ore");
    deep_diamond_ = d("minecraft:deepslate_diamond_ore"); deep_redstone_ = B.find("minecraft:deepslate_redstone_ore", "lit=false");
    deep_lapis_ = d("minecraft:deepslate_lapis_ore"); deep_copper_ = d("minecraft:deepslate_copper_ore");
    cactus_ = B.find("minecraft:cactus", "age=0"); red_sand_ = d("minecraft:red_sand"); clay_ = d("minecraft:clay");
}

// Gradient noise (improved Perlin), channel selects an independent permutation.
static double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
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

WorldGen::Column WorldGen::column(int x, int z) const {
    double bs = kind_ == GenKind::LargeBiomes ? 4.0 : 1.0;       // large biomes: the same climate stretched out
    double cont = fbm2(x / (600.0 * bs / 1.0), z / (600.0 * bs), 4, 0);       // oceans and land masses
    double hills = fbm2(x / 70.0, z / 70.0, 4, 1);
    double ridge = std::fabs(fbm2(x / 160.0, z / 160.0, 3, 2));
    double temp = fbm2(x / (900.0 * bs), z / (900.0 * bs), 3, 3);
    double humid = fbm2((x + 5000) / (700.0 * bs), (z - 3000) / (700.0 * bs), 3, 2);
    double base = 66 + cont * 38;
    double h = base + hills * 10 + (cont > 0.0 ? ridge * 24 * std::min(1.0, cont * 3) : 0);
    if (kind_ == GenKind::Amplified) h = 64 + (h - 64) * 2.6 + std::max(0.0, cont) * 40;
    int hi = static_cast<int>(std::floor(h));
    Column c{hi, 0, temp, humid};
    if (hi < 58) c.biome = hi < 40 ? b_deep_ocean_ : (temp < -0.35 ? b_cold_ocean_ : b_ocean_);
    else if (hi <= 64) c.biome = b_beach_;
    else if (temp > 0.28 && humid < 0.05) c.biome = b_desert_;
    else if (temp < -0.3) c.biome = b_snowy_;
    else if (humid > 0.12) c.biome = b_forest_;
    else c.biome = b_plains_;
    return c;
}

int WorldGen::surface_height(int x, int z) const {
    if (kind_ == GenKind::Flat) return -61;
    if (kind_ == GenKind::Void) return kMinY - 1;
    return column(x, z).height;
}

bool WorldGen::find_spawn(int& sx, int& sz) const {
    if (kind_ == GenKind::Flat || kind_ == GenKind::Void) { sx = sz = 0; return true; }
    for (int r = 0; r <= 1024; r += 8)
        for (int a = 0; a < std::max(1, r / 4); a++) {
            double ang = a * 6.2831853 / std::max(1, r / 4);
            int x = static_cast<int>(std::lround(std::cos(ang) * r)), z = static_cast<int>(std::lround(std::sin(ang) * r));
            Column c = column(x, z);
            if (c.height >= 66 && c.height <= 90 && c.biome != b_beach_) { sx = x; sz = z; return true; }
        }
    sx = sz = 0;
    return false;
}

void WorldGen::flat(Chunk& c) const {
    uint16_t layers[4] = {bedrock_, dirt_, dirt_, grass_};
    c.sec[0] = std::make_unique<Section>();
    memset(c.sec[0]->biome, b_plains_, 64);
    for (int y = 0; y < 4; y++) for (int i = 0; i < 256; i++) c.sec[0]->b[(y << 8) | i] = layers[y];
    memset(c.air_biomes, b_plains_, sizeof c.air_biomes);
}

void WorldGen::terrain(Chunk& c) const {
    PN_PERF("chunk.generate");
    memset(c.air_biomes, b_plains_, sizeof c.air_biomes);
    Column cols[16][16];
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) cols[z][x] = column(c.x * 16 + x, c.z * 16 + z);
    auto sec_for = [&](int y) -> Section& {
        int s = (y - kMinY) >> 4;
        if (!c.sec[s]) { c.sec[s] = std::make_unique<Section>(); memset(c.sec[s]->biome, b_plains_, 64); }
        return *c.sec[s];
    };
    auto put = [&](int x, int y, int z, uint16_t b) { if (y >= kMinY && y < kMinY + kHeight) sec_for(y).b[((y & 15) << 8) | (z << 4) | x] = b; };
    auto get = [&](int x, int y, int z) -> uint16_t { return c.get(x, y, z); };
    uint64_t rng = hash2(seed_, c.x, c.z);
    auto rnd = [&]() { return static_cast<double>(splitmix(rng) >> 11) / 9007199254740992.0; };
    const int SEA = 62;
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            const Column& col = cols[z][x];
            int wx = c.x * 16 + x, wz = c.z * 16 + z;
            int h = std::min(col.height, kMinY + kHeight - 2);
            bool desert = col.biome == b_desert_, snowy = col.biome == b_snowy_;
            bool beach = col.biome == b_beach_, ocean = col.biome == b_ocean_ || col.biome == b_deep_ocean_ || col.biome == b_cold_ocean_;
            for (int y = kMinY; y <= h; y++) {
                uint16_t b;
                if (y < kMinY + 1 + static_cast<int>(rnd() * 4)) b = bedrock_;
                else if (y < static_cast<int>(rnd() * 16) - 8) b = deepslate_;
                else b = stone_;
                int depth = h - y;
                if (b == stone_) {
                    if (ocean || beach) { if (depth < 4) b = (ocean && col.humid < -0.2 && depth < 2) ? gravel_ : sand_; }
                    else if (desert) { if (depth < 4) b = sand_; else if (depth < 8) b = sandstone_; }
                    else if (depth == 0) b = grass_;
                    else if (depth < 4) b = dirt_;
                }
                put(x, y, z, b);
            }
            // caves: 3D noise carved out below the surface (never through the top layers, to keep the ground intact)
            for (int y = std::max(kMinY + 5, -58); y < h - 5 && y < 56; y++) {
                double n = noise3(wx / 28.0, y / 18.0, wz / 28.0, 1) + 0.5 * noise3(wx / 11.0, y / 8.0, wz / 11.0, 2);
                if (n > 0.9) put(x, y, z, y < -54 ? lava_ : air_);
            }
            // sea
            for (int y = h + 1; y <= SEA; y++) put(x, y, z, water_);
            if (ocean && h < SEA && snowy) put(x, SEA, z, air_);
            // surface decoration
            if (h >= SEA && h > 0 && !ocean) {
                uint16_t top = get(x, h, z);
                if (snowy && top == grass_) put(x, h + 1, z, snow_);
                else if (top == grass_ && rnd() < (col.biome == b_forest_ ? 0.08 : 0.14)) put(x, h + 1, z, short_grass_);
                else if (top == grass_ && rnd() < 0.012) put(x, h + 1, z, rnd() < 0.5 ? dandelion_ : poppy_);
                else if (desert && top == sand_ && rnd() < 0.006) { int k = 1 + static_cast<int>(rnd() * 3); for (int i = 1; i <= k; i++) put(x, h + i, z, cactus_); }
            }
        }
    // biome cells (4x4x4) follow the climate at the middle of each 4x4 column group
    for (int cz = 0; cz < 4; cz++)
        for (int cx = 0; cx < 4; cx++) {
            uint8_t bm = static_cast<uint8_t>(cols[cz * 4 + 1][cx * 4 + 1].biome);
            for (int s = 0; s < kSections; s++)
                for (int cy = 0; cy < 4; cy++) {
                    int i = cy * 16 + cz * 4 + cx;
                    c.air_biomes[s][i] = bm;
                    if (c.sec[s]) c.sec[s]->biome[i] = bm;
                }
        }
    // ores: blobs at random spots, more of the cheap ones high up
    struct Ore { uint16_t stone, deep; int tries, minY, maxY, size; };
    Ore ores[] = {{coal_, deep_coal_, 14, 0, 128, 7}, {iron_, deep_iron_, 12, -32, 64, 5}, {copper_, deep_copper_, 8, -16, 80, 6}, {gold_, deep_gold_, 3, -60, 30, 5},
                  {redstone_, deep_redstone_, 6, -60, 14, 5}, {lapis_, deep_lapis_, 2, -50, 30, 4}, {diamond_, deep_diamond_, 3, -60, 12, 3}};
    for (auto& o : ores)
        for (int t = 0; t < o.tries; t++) {
            int ox = static_cast<int>(rnd() * 16), oz = static_cast<int>(rnd() * 16), oy = o.minY + static_cast<int>(rnd() * (o.maxY - o.minY));
            for (int k = 0; k < o.size; k++) {
                int x = ox + static_cast<int>(rnd() * 3) - 1, y = oy + static_cast<int>(rnd() * 3) - 1, z = oz + static_cast<int>(rnd() * 3) - 1;
                if (x < 0 || x > 15 || z < 0 || z > 15 || y < kMinY + 5) continue;
                uint16_t cur = get(x, y, z);
                if (cur == stone_) put(x, y, z, o.stone);
                else if (cur == deepslate_) put(x, y, z, o.deep);
            }
        }
    // trees (kept inside the chunk so no neighbour is needed)
    for (int t = 0; t < 4; t++) {
        int x = 3 + static_cast<int>(rnd() * 10), z = 3 + static_cast<int>(rnd() * 10);
        const Column& col = cols[z][x];
        if (col.biome != b_forest_ && !(col.biome == b_plains_ && rnd() < 0.15)) continue;
        if (col.biome == b_forest_ && rnd() > 0.7) continue;
        int h = col.height;
        if (h <= SEA || get(x, h, z) != grass_ || get(x, h + 1, z) != air_) continue;
        int th = 4 + static_cast<int>(rnd() * 3);
        for (int dy = th - 2; dy <= th + 1; dy++) {
            int r = dy >= th ? 1 : 2;
            for (int dx = -r; dx <= r; dx++) for (int dz = -r; dz <= r; dz++) {
                if (std::abs(dx) == r && std::abs(dz) == r && (dy >= th || rnd() < 0.5)) continue;
                int lx = x + dx, lz = z + dz;
                if (lx < 0 || lx > 15 || lz < 0 || lz > 15) continue;
                if (get(lx, h + 1 + dy, lz) == air_) put(lx, h + 1 + dy, lz, leaves_);
            }
        }
        for (int dy = 1; dy <= th; dy++) put(x, h + dy, z, log_);
        put(x, h, z, dirt_);
    }
}

void WorldGen::generate(Chunk& c) const {
    c.extra = Nbt::compound();
    c.extra.put("Status", Nbt::str("minecraft:full"));
    if (kind_ == GenKind::Void) { memset(c.air_biomes, b_plains_, sizeof c.air_biomes); return; }
    if (kind_ == GenKind::Flat) { flat(c); return; }
    terrain(c);
}

}  // namespace pn
