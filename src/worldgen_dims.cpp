// The Nether and the End. Blocks use real heights (the Nether and the End span y 0..255, so their chunks fill sections 4..19).
#include "perf.hpp"
#include "worldgen.hpp"
#include "worldgen_util.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pn {

namespace {

struct DimBlocks {
    uint16_t netherrack, quartz_ore, gold_ore, debris, soul_sand, soul_soil, crimson_nylium, warped_nylium, crimson_stem, warped_stem, wart_block, warped_wart, shroomlight,
        crimson_roots, warped_roots, sprouts, crimson_fungus, warped_fungus, basalt, blackstone, magma, glowstone, fire, soul_fire, gravel, lava, bedrock, air;
    uint16_t end_stone, obsidian, chorus_flower, chorus_mid, chorus_low, chorus_top, iron_bars, torch_n, torch_s, torch_e, torch_w, end_portal;
    DimBlocks() {
        const Blocks& B = Blocks::get();
        auto d = [&](const char* n) { return B.default_state(n); };
        netherrack = d("minecraft:netherrack"); quartz_ore = d("minecraft:nether_quartz_ore"); gold_ore = d("minecraft:nether_gold_ore"); debris = d("minecraft:ancient_debris");
        soul_sand = d("minecraft:soul_sand"); soul_soil = d("minecraft:soul_soil"); crimson_nylium = d("minecraft:crimson_nylium"); warped_nylium = d("minecraft:warped_nylium");
        crimson_stem = B.find("minecraft:crimson_stem", "axis=y"); warped_stem = B.find("minecraft:warped_stem", "axis=y");
        wart_block = d("minecraft:nether_wart_block"); warped_wart = d("minecraft:warped_wart_block"); shroomlight = d("minecraft:shroomlight");
        crimson_roots = d("minecraft:crimson_roots"); warped_roots = d("minecraft:warped_roots"); sprouts = d("minecraft:nether_sprouts");
        crimson_fungus = d("minecraft:crimson_fungus"); warped_fungus = d("minecraft:warped_fungus");
        basalt = B.find("minecraft:basalt", "axis=y"); blackstone = d("minecraft:blackstone"); magma = d("minecraft:magma_block"); glowstone = d("minecraft:glowstone");
        fire = d("minecraft:fire"); soul_fire = d("minecraft:soul_fire"); gravel = d("minecraft:gravel"); lava = B.find("minecraft:lava", "level=0");
        bedrock = d("minecraft:bedrock"); air = 0;
        end_stone = d("minecraft:end_stone"); obsidian = d("minecraft:obsidian"); chorus_flower = B.find("minecraft:chorus_flower", "age=0");
        chorus_low = B.find("minecraft:chorus_plant", "down=false,east=false,north=false,south=false,up=true,west=false");
        chorus_mid = B.find("minecraft:chorus_plant", "down=true,east=false,north=false,south=false,up=true,west=false");
        chorus_top = B.find("minecraft:chorus_plant", "down=true,east=false,north=false,south=false,up=true,west=false");
        iron_bars = d("minecraft:iron_bars");
        torch_n = B.find("minecraft:wall_torch", "facing=north"); torch_s = B.find("minecraft:wall_torch", "facing=south");
        torch_e = B.find("minecraft:wall_torch", "facing=east"); torch_w = B.find("minecraft:wall_torch", "facing=west");
        end_portal = d("minecraft:end_portal");
    }
};
const DimBlocks& db() { static const DimBlocks b; return b; }

inline void put_at(Chunk& c, int x, int y, int z, uint16_t b, const uint8_t* biome_fill) {
    if (y < kMinY || y >= kMinY + kHeight) return;
    int s = (y - kMinY) >> 4;
    if (!c.sec[s]) { c.sec[s] = std::make_unique<Section>(); memcpy(c.sec[s]->biome, c.air_biomes[s], 64); (void)biome_fill; }
    c.sec[s]->b[((y & 15) << 8) | (z << 4) | x] = b;
}

}  // namespace

// ---- The Nether --------------------------------------------------------------------------------------------

void WorldGen::nether(Chunk& c) const {
    PN_PERF("chunk.generate");
    const DimBlocks& N = db();
    int x0 = c.x * 16, z0 = c.z * 16;
    // biome of every column (and of the 4x4 cells): soul sand valleys, crimson and warped forests, basalt deltas, wastes between them
    uint8_t col_bm[16][16];
    auto biome_at = [&](int wx, int wz) -> int {
        double n1 = fbm2(wx / 260.0, wz / 260.0, 3, 3) * 2.0, n2 = fbm2((wx + 4000) / 260.0, (wz - 2000) / 260.0, 3, 2) * 2.0;
        if (n1 < -0.35) return SOUL_SAND_VALLEY;
        if (n1 > 0.4) return n2 > 0 ? CRIMSON_FOREST : WARPED_FOREST;
        if (n2 > 0.5) return BASALT_DELTAS;
        return NETHER_WASTES;
    };
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) col_bm[z][x] = static_cast<uint8_t>(biome_at(x0 + x, z0 + z));
    for (int cz = 0; cz < 4; cz++)
        for (int cx = 0; cx < 4; cx++) {
            uint8_t bm = bi_[col_bm[cz * 4 + 2][cx * 4 + 2]];
            for (int s = 0; s < kSections; s++) for (int cy = 0; cy < 4; cy++) c.air_biomes[s][cy * 16 + cz * 4 + cx] = bm;
        }
    // density on a coarse grid (4 blocks), interpolated per block: solid where positive. Floor and ceiling are pushed solid, the middle opens up.
    constexpr int GY = 33;
    static thread_local float dens[5][5][GY];
    for (int gz = 0; gz < 5; gz++)
        for (int gx = 0; gx < 5; gx++) {
            double wx = x0 + gx * 4, wz = z0 + gz * 4;
            for (int gy = 0; gy < GY; gy++) {
                double y = gy * 4;
                double n = noise3(wx / 48.0, y / 24.0, wz / 48.0, 1) + 0.5 * noise3(wx / 19.0, y / 10.0, wz / 19.0, 2) + 0.2 * noise3(wx / 8.0, y / 5.0, wz / 8.0, 4);
                double d = n * 2.3 + 1.8 * smooth(36, 4, y) + 1.8 * smooth(92, 124, y) - 0.45;
                dens[gz][gx][gy] = static_cast<float>(d);
            }
        }
    uint64_t rng = hash2(seed_ ^ 0x4e657468, c.x, c.z);
    auto rnd = [&]() { return rnd01(rng); };
    static thread_local bool solid[16][16][128];
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            double fx = (x & 3) / 4.0, fz = (z & 3) / 4.0;
            int gx = x >> 2, gz = z >> 2;
            for (int y = 0; y < 128; y++) {
                int gy = y >> 2;
                double fy = (y & 3) / 4.0;
                auto g = [&](int ax, int az, int ay) { return static_cast<double>(dens[gz + az][gx + ax][gy + ay]); };
                double c00 = lerp(g(0, 0, 0), g(0, 0, 1), fy), c10 = lerp(g(1, 0, 0), g(1, 0, 1), fy), c01 = lerp(g(0, 1, 0), g(0, 1, 1), fy), c11 = lerp(g(1, 1, 0), g(1, 1, 1), fy);
                bool sol = lerp(lerp(c00, c10, fx), lerp(c01, c11, fx), fz) > 0;
                if (y < 5 || y > 122) sol = true;
                solid[z][x][y] = sol;
                uint16_t b = sol ? N.netherrack : (y <= 31 ? N.lava : N.air);
                if (sol && (y == 0 || y == 127 || (y < 5 && rnd() * 5 > y) || (y > 122 && rnd() * 5 > 127 - y))) b = N.bedrock;
                if (b) put_at(c, x, y, z, b, nullptr);
            }
        }
    // surfaces: floors (solid with air above) and ceilings take the biome's materials; plants, fire and fungi go on the floors
    uint64_t pr = hash2(seed_ ^ 0x9a7c4, c.x, c.z);
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int wx = x0 + x, wz = z0 + z, bm = col_bm[z][x];
            double patch = noise2(wx / 9.0, wz / 9.0, 5);
            for (int y = 5; y < 122; y++) {
                if (!solid[z][x][y]) continue;
                bool air_above = !solid[z][x][y + 1] && c.get(x, y + 1, z) == N.air;
                bool air_below = !solid[z][x][y - 1] && c.get(x, y - 1, z) == N.air;
                if (air_above) {
                    uint16_t top = N.netherrack, plant = 0;
                    double r = rnd01(pr);
                    switch (bm) {
                        case SOUL_SAND_VALLEY:
                            top = patch > 0.1 ? N.soul_sand : N.soul_soil;
                            if (r < 0.01) plant = N.soul_fire;
                            break;
                        case CRIMSON_FOREST:
                            top = N.crimson_nylium;
                            plant = r < 0.18 ? N.crimson_roots : r < 0.21 ? N.crimson_fungus : 0;
                            break;
                        case WARPED_FOREST:
                            top = N.warped_nylium;
                            plant = r < 0.12 ? N.warped_roots : r < 0.2 ? N.sprouts : r < 0.23 ? N.warped_fungus : 0;
                            break;
                        case BASALT_DELTAS:
                            top = patch > 0.2 ? N.basalt : patch < -0.2 ? N.blackstone : N.netherrack;
                            if (r < 0.015) top = N.magma;
                            break;
                        default:
                            if (patch > 0.5) top = N.soul_sand;
                            else if (patch < -0.55) top = N.gravel;
                            if (r < 0.004 && top == N.netherrack) plant = N.fire;
                            break;
                    }
                    if (y <= 33 && (top == N.crimson_nylium || top == N.warped_nylium)) top = N.netherrack;   // nylium does not grow in lava
                    put_at(c, x, y, z, top, nullptr);
                    if (top == N.soul_sand || top == N.soul_soil) { if (solid[z][x][y - 1]) put_at(c, x, y - 1, z, top == N.soul_sand ? N.soul_sand : N.soul_soil, nullptr); }
                    if (plant && y > 32 && plant != N.fire && plant != N.soul_fire) put_at(c, x, y + 1, z, plant, nullptr);
                    else if (plant && y > 32) put_at(c, x, y + 1, z, plant, nullptr);
                    // basalt columns
                    if (bm == BASALT_DELTAS && y > 32 && rnd01(pr) < 0.04 && top != N.magma) {
                        int hgt = 1 + static_cast<int>(rnd01(pr) * 4);
                        for (int k = 1; k <= hgt && y + k < 120; k++) if (!solid[z][x][y + k]) put_at(c, x, y + k, z, N.basalt, nullptr);
                    }
                } else if (air_below && bm == WARPED_FOREST && rnd01(pr) < 0.0) {
                }
            }
        }
    // ores: random walks inside netherrack
    auto blob = [&](uint16_t to, int tries, int minY, int maxY, int size) {
        for (int t = 0; t < tries; t++) {
            int x = static_cast<int>(rnd() * 16), z = static_cast<int>(rnd() * 16), y = minY + static_cast<int>(rnd() * (maxY - minY));
            for (int k = 0; k < size; k++) {
                x += static_cast<int>(rnd() * 3) - 1; y += static_cast<int>(rnd() * 3) - 1; z += static_cast<int>(rnd() * 3) - 1;
                if (x < 0 || x > 15 || z < 0 || z > 15 || y < 6 || y > 120) continue;
                if (c.get(x, y, z) == N.netherrack) put_at(c, x, y, z, to, nullptr);
            }
        }
    };
    blob(N.quartz_ore, 16, 10, 117, 8);
    blob(N.gold_ore, 10, 10, 117, 7);
    blob(N.debris, 2, 8, 22, 3);
    blob(N.magma, 4, 27, 36, 8);
    // glowstone clusters hanging from ceilings
    for (int t = 0; t < 10; t++) {
        int x = static_cast<int>(rnd() * 16), z = static_cast<int>(rnd() * 16), y = 122;
        while (y > 50 && !(c.get(x, y, z) == N.netherrack && c.get(x, y - 1, z) == N.air)) y--;
        if (y <= 50) continue;
        int bx = x, by = y, bz = z;
        for (int k = 0; k < 14; k++) {
            put_at(c, bx, by, bz, N.glowstone, nullptr);
            bx = std::clamp(bx + static_cast<int>(rnd() * 3) - 1, 0, 15); bz = std::clamp(bz + static_cast<int>(rnd() * 3) - 1, 0, 15);
            by = std::clamp(by - static_cast<int>(rnd() * 2), 6, 121);
            if (c.get(bx, by, bz) != N.air && c.get(bx, by, bz) != N.netherrack) break;
        }
    }
    // huge fungi in the forests (kept inside the chunk)
    for (int t = 0; t < 3; t++) {
        int x = 4 + static_cast<int>(rnd() * 8), z = 4 + static_cast<int>(rnd() * 8);
        int bm = col_bm[z][x];
        if (bm != CRIMSON_FOREST && bm != WARPED_FOREST) continue;
        bool crimson = bm == CRIMSON_FOREST;
        int y = 100;
        while (y > 34 && !(c.get(x, y, z) == (crimson ? N.crimson_nylium : N.warped_nylium) && c.get(x, y + 1, z) == N.air)) y--;
        if (y <= 34) continue;
        int th = 4 + static_cast<int>(rnd() * 5);
        bool clear = true;
        for (int k = 1; k <= th + 2 && clear; k++) if (c.get(x, y + k, z) != N.air) clear = false;
        if (!clear) continue;
        uint16_t stem = crimson ? N.crimson_stem : N.warped_stem, cap = crimson ? N.wart_block : N.warped_wart;
        for (int dy = th - 2; dy <= th; dy++) {
            int r = dy == th ? 2 : 3;
            for (int dx = -r; dx <= r; dx++) for (int dz = -r; dz <= r; dz++) {
                if (dx * dx + dz * dz > r * r + 1) continue;
                int lx = x + dx, lz = z + dz;
                if (lx < 0 || lx > 15 || lz < 0 || lz > 15) continue;
                if (c.get(lx, y + 1 + dy, lz) == N.air) put_at(c, lx, y + 1 + dy, lz, rnd() < 0.06 ? N.shroomlight : cap, nullptr);
            }
        }
        for (int k = 1; k <= th; k++) put_at(c, x, y + k, z, stem, nullptr);
    }
}

// ---- The End -----------------------------------------------------------------------------------------------

// Top and bottom of the end stone in a column (-1 when there is none) and the column's biome.
int WorldGen::end_column(int wx, int wz, int& bottom, int& bm) const {
    int top = -1;
    bottom = 0;
    bm = THE_END;
    double r = std::sqrt(static_cast<double>(wx) * wx + static_cast<double>(wz) * wz);
    if (r < 78) {
        double t = 1.0 - r / 78.0;
        top = 63 + static_cast<int>(std::floor(3.0 * t + noise2(wx / 9.0, wz / 9.0, 1) * 1.6));
        bottom = top - static_cast<int>(36.0 * std::pow(t, 0.6) + 2 + noise2(wx / 13.0, wz / 13.0, 2) * 2.5);
    } else if (r > 960) {
        double f = fbm2(wx / 170.0, wz / 170.0, 3, 3) * 2.0;
        double need = r < 1100 ? 0.5 : 0.28;
        if (f > need) {
            double s = f - need;
            top = 62 + static_cast<int>(std::floor(10.0 * noise2(wx / 60.0, wz / 60.0, 4)));
            bottom = top - static_cast<int>(std::sqrt(s) * 55.0) - 2;
            double e = fbm2((wx + 7000) / 230.0, (wz - 3000) / 230.0, 3, 5) * 2.0;
            bm = r < 1100 ? SMALL_END_ISLANDS : e > 0.25 ? END_HIGHLANDS : e > -0.2 ? END_MIDLANDS : END_BARRENS;
        }
    }
    return top;
}

void WorldGen::end(Chunk& c) const {
    PN_PERF("chunk.generate");
    const DimBlocks& E = db();
    int x0 = c.x * 16, z0 = c.z * 16;
    uint64_t rng = hash2(seed_ ^ 0x456e64, c.x, c.z);
    auto rnd = [&]() { return rnd01(rng); };
    int top_y[16][16];
    uint8_t col_bm[16][16];
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int wx = x0 + x, wz = z0 + z;
            int bottom = 0, bm = THE_END;
            int top = end_column(wx, wz, bottom, bm);
            top_y[z][x] = top;
            col_bm[z][x] = static_cast<uint8_t>(bm);
            if (top >= 0) for (int y = std::max(bottom, 1); y <= top; y++) put_at(c, x, y, z, E.end_stone, nullptr);
        }
    for (int cz = 0; cz < 4; cz++)
        for (int cx = 0; cx < 4; cx++) {
            uint8_t bm = bi_[col_bm[cz * 4 + 2][cx * 4 + 2]];
            for (int s = 0; s < kSections; s++) for (int cy = 0; cy < 4; cy++) c.air_biomes[s][cy * 16 + cz * 4 + cx] = bm;
        }
    // the ten obsidian pillars around the main island, sizes shuffled by the seed
    {
        int order[10];
        for (int i = 0; i < 10; i++) order[i] = i;
        uint64_t pr = seed_ ^ 0x70696c6c;
        for (int i = 9; i > 0; i--) std::swap(order[i], order[rndi(pr, i + 1)]);
        for (int i = 0; i < 10; i++) {
            int size = order[i];                      // 0..9
            int radius = 2 + size / 3, height = 76 + size * 3;
            int px = static_cast<int>(std::lround(std::cos(i * 0.6283185307) * 42.0)), pz = static_cast<int>(std::lround(std::sin(i * 0.6283185307) * 42.0));
            bool caged = size < 2;
            for (int z = std::max(z0, pz - radius - 2); z <= std::min(z0 + 15, pz + radius + 2); z++)
                for (int x = std::max(x0, px - radius - 2); x <= std::min(x0 + 15, px + radius + 2); x++) {
                    int lx = x - x0, lz = z - z0;
                    double dd = (x - px) * (x - px) + (z - pz) * (z - pz);
                    if (dd <= (radius + 0.5) * (radius + 0.5)) {
                        for (int y = std::max(top_y[lz][lx] - 3, 40); y <= height; y++) put_at(c, lx, y, lz, E.obsidian, nullptr);
                        if (dd <= 2.5) put_at(c, lx, height + 1, lz, E.bedrock, nullptr);
                    } else if (caged && dd <= (radius + 2.5) * (radius + 2.5) && dd >= (radius + 1.5) * (radius + 1.5) - 1) {
                        for (int y = height + 1; y <= height + 3; y++) put_at(c, lx, y, lz, E.iron_bars, nullptr);
                    }
                }
        }
    }
    // the exit fountain: a bedrock basin with a pillar and torches (the portal opens when the dragon dies)
    if (std::abs(c.x) <= 1 && std::abs(c.z) <= 1) {
        for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
            int wx = x0 + x, wz = z0 + z;
            int dd = wx * wx + wz * wz;
            if (dd > 20) continue;
            int T = 63 + 3;
            if (dd >= 5) { put_at(c, x, T, z, E.bedrock, nullptr); put_at(c, x, T - 1, z, E.bedrock, nullptr); if (dd <= 13) put_at(c, x, T + 1, z, E.bedrock, nullptr); }
            else put_at(c, x, T, z, (wx == 0 && wz == 0) ? E.bedrock : E.end_portal, nullptr);
            if (wx == 0 && wz == 0) for (int y = T + 1; y <= T + 4; y++) put_at(c, x, y, z, E.bedrock, nullptr);
        }
        auto tor = [&](int wx, int wz, uint16_t b) { if ((wx >> 4) == c.x && (wz >> 4) == c.z) put_at(c, wx & 15, 63 + 3 + 3, wz & 15, b, nullptr); };
        tor(1, 0, E.torch_e); tor(-1, 0, E.torch_w); tor(0, 1, E.torch_s); tor(0, -1, E.torch_n);
    }
    // chorus plants on the outer islands
    for (int t = 0; t < 8; t++) {
        int x = static_cast<int>(rnd() * 16), z = static_cast<int>(rnd() * 16);
        int bm = col_bm[z][x];
        if ((bm != END_HIGHLANDS && bm != END_MIDLANDS) || top_y[z][x] < 0 || rnd() > 0.5) continue;
        int h = 1 + static_cast<int>(rnd() * 4);
        int y0 = top_y[z][x];
        for (int k = 1; k <= h; k++) put_at(c, x, y0 + k, z, k == 1 ? E.chorus_low : E.chorus_mid, nullptr);
        put_at(c, x, y0 + h + 1, z, E.chorus_flower, nullptr);
    }
}

}  // namespace pn
