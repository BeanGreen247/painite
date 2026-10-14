// Terrain generator checks: biome shares, timing, and a top-down map.
//   painite_genstats stats SEED [kind]            biome shares over a 16 km square
//   painite_genstats map SEED X Z SIZE STEP OUT   PPM map (one pixel per STEP blocks)
//   painite_genstats bench SEED                   chunk generation time and cave share
#include "blocks.hpp"
#include "data.hpp"
#include "world.hpp"
#include "worldgen.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
using namespace pn;

static const char* kNames[] = {"plains", "forest", "flower_forest", "birch_forest", "old_birch", "dark_forest", "taiga", "snowy_taiga", "old_spruce", "old_pine", "snowy_plains", "desert", "savanna",
    "windswept_savanna", "jungle", "sparse_jungle", "swamp", "mangrove", "badlands", "eroded_badlands", "wooded_badlands", "windswept_hills", "windswept_gravelly", "windswept_forest", "meadow",
    "cherry", "grove", "snowy_slopes", "jagged", "frozen_peaks", "stony_peaks", "beach", "snowy_beach", "stony_shore", "river", "frozen_river", "ocean", "deep_ocean", "cold_ocean", "deep_cold",
    "frozen_ocean", "deep_frozen", "lukewarm", "deep_lukewarm", "warm", "mushroom"};
static const uint8_t kColors[][3] = {{141, 179, 96}, {5, 102, 33}, {45, 142, 73}, {48, 116, 68}, {88, 156, 76}, {64, 81, 26}, {11, 102, 89}, {49, 85, 74}, {89, 102, 81}, {69, 7, 62},
    {255, 255, 255}, {250, 148, 24}, {189, 178, 95}, {231, 165, 119}, {83, 123, 9}, {98, 139, 23}, {7, 249, 178}, {97, 120, 70}, {217, 69, 21}, {255, 109, 61}, {176, 151, 101},
    {96, 96, 96}, {136, 136, 136}, {80, 112, 80}, {131, 210, 160}, {255, 183, 197}, {160, 200, 160}, {220, 235, 240}, {160, 160, 190}, {120, 160, 255}, {150, 150, 150},
    {250, 222, 85}, {240, 240, 200}, {162, 162, 132}, {0, 0, 255}, {160, 160, 255}, {0, 0, 112}, {0, 0, 48}, {32, 32, 112}, {32, 32, 56}, {112, 112, 214}, {64, 64, 144},
    {0, 0, 172}, {0, 0, 90}, {0, 0, 255}, {255, 0, 255}};

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    VanillaData v;
    std::string err;
    if (!load_vanilla_file("data/vanilla-26.3.bin", v, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (!Blocks::get().load({{v.protocol, v.blocks_path}}, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::string mode = argv[1];
    uint64_t seed = static_cast<uint64_t>(atoll(argv[2]));
    GenKind kind = GenKind::Normal;
    if (mode == "stats" && argc > 3) kind = !strcmp(argv[3], "large") ? GenKind::LargeBiomes : !strcmp(argv[3], "amp") ? GenKind::Amplified : GenKind::Normal;
    WorldGen g(kind, seed, v.biomes);
    if (mode == "stats") {
        std::map<int, long> cnt;
        long total = 0, land = 0;
        std::vector<int> hs;
        for (int z = -8192; z < 8192; z += 16)
            for (int x = -8192; x < 8192; x += 16) {
                int b, h = g.sample(x, z, b);
                cnt[b]++; total++;
                if (b < 31 || b == 32 || b == 33) { land++; hs.push_back(h); }
                (void)h;
            }
        for (auto& [b, n] : cnt) printf("%-20s %6.2f%%\n", kNames[b], 100.0 * static_cast<double>(n) / static_cast<double>(total));
        std::sort(hs.begin(), hs.end());
        if (!hs.empty()) printf("land %.1f%%; land height p5 %d p25 %d p50 %d p75 %d p95 %d p99 %d max %d\n", 100.0 * static_cast<double>(land) / static_cast<double>(total), hs[hs.size() / 20], hs[hs.size() / 4], hs[hs.size() / 2], hs[hs.size() * 3 / 4], hs[hs.size() * 19 / 20], hs[hs.size() * 99 / 100], hs.back());
        int sx, sz; g.find_spawn(sx, sz); printf("spawn %d %d\n", sx, sz);
    } else if (mode == "map" && argc >= 8) {
        int x0 = atoi(argv[3]), z0 = atoi(argv[4]), size = atoi(argv[5]), step = atoi(argv[6]);
        int n = size / step;
        FILE* f = fopen(argv[7], "wb");
        fprintf(f, "P6\n%d %d\n255\n", n, n);
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) {
                int b, h = g.sample(x0 + i * step, z0 + j * step, b);
                double sh = b >= 36 ? 1.0 : 0.65 + 0.35 * (h - 40) / 160.0;
                if (sh > 1.2) sh = 1.2;
                uint8_t px[3];
                for (int k = 0; k < 3; k++) px[k] = static_cast<uint8_t>(std::min(255.0, kColors[b][k] * sh));
                fwrite(px, 1, 3, f);
            }
        fclose(f);
    } else if ((mode == "top" || mode == "slice") && argc >= 8) {
        // top: PPM of the highest block per column; slice: PPM of a vertical cut along x at fixed z (y -64..200)
        int x0 = atoi(argv[3]), z0 = atoi(argv[4]), size = atoi(argv[5]);
        std::map<std::pair<int, int>, std::unique_ptr<Chunk>> cache;
        auto blk = [&](int x, int y, int z) -> uint16_t {
            int cx = x >> 4, cz = z >> 4;
            auto& p = cache[{cx, cz}];
            if (!p) { p = std::make_unique<Chunk>(); p->x = cx; p->z = cz; g.generate(*p); }
            return p->get(x & 15, y, z & 15);
        };
        auto color = [&](uint16_t id, uint8_t* px) {
            const std::string& n = Blocks::get().info(id).name;
            auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
            uint32_t h = 2166136261u;
            for (char ch : n) h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
            uint8_t r = 80 + (h & 127), gg = 80 + ((h >> 8) & 127), b = 80 + ((h >> 16) & 127);
            if (has("water")) { r = 30; gg = 60; b = 200; }
            else if (has("leaves")) { r = 20; gg = static_cast<uint8_t>(90 + (h & 63)); b = 20; }
            else if (has("_log")) { r = 90; gg = 60; b = 30; }
            else if (n == "minecraft:grass_block") { r = 100; gg = 170; b = 70; }
            else if (n == "minecraft:air") { r = gg = b = 0; }
            else if (has("snow") || has("ice")) { r = 240; gg = 245; b = 255; }
            else if (n == "minecraft:sand") { r = 230; gg = 215; b = 150; }
            else if (n == "minecraft:stone") { r = gg = b = 125; }
            else if (n == "minecraft:lava") { r = 255; gg = 120; b = 0; }
            else if (has("short_grass") || has("fern") || has("tall_grass")) { r = 60; gg = 140; b = 40; }
            px[0] = r; px[1] = gg; px[2] = b;
        };
        FILE* f = fopen(argv[7], "wb");
        if (mode == "top") {
            fprintf(f, "P6\n%d %d\n255\n", size, size);
            for (int j = 0; j < size; j++) for (int i = 0; i < size; i++) {
                int y = kMinY + kHeight - 1;
                while (y > kMinY && blk(x0 + i, y, z0 + j) == 0) y--;
                uint8_t px[3]; color(blk(x0 + i, y, z0 + j), px); fwrite(px, 1, 3, f);
            }
        } else {
            int H = 264;
            fprintf(f, "P6\n%d %d\n255\n", size, H);
            for (int yy = 0; yy < H; yy++) for (int i = 0; i < size; i++) {
                uint8_t px[3]; color(blk(x0 + i, kMinY + H - 1 - yy, z0), px); fwrite(px, 1, 3, f);
            }
        }
        fclose(f);
    } else if (mode == "bench") {
        long air_cave = 0, solid = 0;
        auto t0 = std::chrono::steady_clock::now();
        int N = 0;
        for (int cz = -8; cz < 8; cz++)
            for (int cx = -8; cx < 8; cx++) {
                Chunk c;
                c.x = cx; c.z = cz;
                g.generate(c);
                N++;
                for (int y = -50; y < 40; y++) for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
                    uint16_t b = c.get(x, y, z);
                    (b == 0 ? air_cave : solid)++;
                }
            }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("%d chunks, %.2f ms per chunk, air share y -50..40: %.1f%%\n", N, ms / N, 100.0 * static_cast<double>(air_cave) / static_cast<double>(air_cave + solid));
    }
}
