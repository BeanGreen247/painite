// Generates an area with Painite's vanilla generator and reports what it found: the biomes (with a chunk where each occurs) and counts of the blocks asked for.
// Finds seeds/places that exercise a feature, and shows crashes ("not ported" errors) without a reference world.
// usage: painite_gensweep [--version 26.2] [--seed N] [--dim 0|1|2] [--area CX0 CZ0 CX1 CZ1] [--count substring]...
#include "blocks.hpp"
#include "data.hpp"
#include "log.hpp"
#include "vg/gen.hpp"
#include "world.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace pn { int64_t now_ms() { return 0; } }   // World wants a clock; the sweep does not

using namespace pn;

int main(int argc, char** argv) {
    std::string version = "26.2";
    int64_t seed = 0;
    std::string stage = "full";
    int dim = 0, cx0 = -8, cz0 = -8, cx1 = 8, cz1 = 8;
    std::vector<std::string> counts;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&] { return std::string(argv[++i]); };
        if (a == "--version") version = val();
        else if (a == "--seed") seed = std::stoll(val());
        else if (a == "--dim") dim = std::stoi(val());
        else if (a == "--stage") stage = val();
        else if (a == "--area") { cx0 = std::stoi(val()); cz0 = std::stoi(val()); cx1 = std::stoi(val()); cz1 = std::stoi(val()); }
        else if (a == "--count") counts.push_back(val());
    }
    VanillaData vd;
    std::string err;
    if (!load_vanilla_file("data/vanilla-" + version + ".bin", vd, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (!Blocks::get().load({{vd.protocol, vd.blocks_path}}, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const Blocks& B = Blocks::get();
    vg::GenOptions opt;
    opt.version = version;
    opt.dim = dim;
    opt.stage = stage;
    opt.seed = seed;
    vg::VanillaGen gen("data", opt, vd.biomes);
    std::map<std::string, std::pair<long, std::string>> biomes;
    std::map<std::string, long> found;
    std::vector<uint8_t> name_hit;
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++) {
            Chunk c;
            c.x = cx;
            c.z = cz;
            try { gen.generate(c); } catch (const std::exception& e) { printf("chunk %d %d: %s\n", cx, cz, e.what()); return 2; }
            for (int s = 0; s < kSections; s++) {
                const uint8_t* bm = c.sec[s] ? c.sec[s]->biome : c.air_biomes[s];
                for (int i = 0; i < 64; i++) {
                    auto& e = biomes[vd.biomes[bm[i]]];
                    if (e.first++ == 0) e.second = std::to_string(cx) + " " + std::to_string(cz);
                }
            }
            if (counts.empty()) continue;
            for (int y = kMinY; y < kMinY + kHeight; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        const std::string& n = B.info(c.get(x, y, z)).name;
                        for (auto& want : counts) if (n.find(want) != std::string::npos) found[want + "@" + std::to_string(cx) + "," + std::to_string(cz)]++;
                    }
        }
    for (auto& kv : biomes) printf("biome %-45s %8ld cells, first chunk %s\n", kv.first.c_str(), kv.second.first, kv.second.second.c_str());
    for (auto& kv : found) printf("found %-50s %ld\n", kv.first.c_str(), kv.second);
    return 0;
}
