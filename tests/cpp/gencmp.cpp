// Compares Painite's world generation with a world made by Mojang's server (tests/cpp/refgen.cpp) block by block and biome by biome.
//   painite_gencmp --world DIR/world --version 26.2 --seed 12345 [--stage noise|surface|carvers|features] [--dim overworld] [--type normal]
#include "anvil.hpp"
#include "blocks.hpp"
#include "data.hpp"
#include "log.hpp"
#include "vg/gen.hpp"
#include "world.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

namespace pn { int64_t now_ms() { return 0; } }   // World wants a clock; the comparison does not
using namespace pn;

int main(int argc, char** argv) {
    std::string climate, world_dir, version = "26.2", stage = "noise", dim = "overworld", type = "normal";
    int64_t seed = 12345;
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--world") world_dir = val();
        else if (a == "--version") version = val();
        else if (a == "--seed") seed = std::stoll(val());
        else if (a == "--stage") stage = val();
        else if (a == "--dim") dim = val();
        else if (a == "--type") type = val();
        else if (a == "--climate") climate = val();
        else if (a == "-v") verbose++;
    }
    VanillaData vd;
    std::string err;
    if (!load_vanilla_file("data/vanilla-" + version + ".bin", vd, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (!Blocks::get().load({{vd.protocol, vd.blocks_path}}, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const Blocks& B = Blocks::get();
    World w;
    if (!w.open(world_dir, "void", "normal", std::to_string(seed), vd, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    World* src = &w;
    World other;
    int dim_id = dim == "overworld" ? 0 : dim == "the_nether" ? 1 : 2;
    if (dim_id) { other.open_dimension(w, dim_id, err); src = &other; }
    vg::GenOptions opt;
    opt.version = version;
    opt.dim = dim_id;
    opt.seed = seed;
    opt.level_type = type;
    opt.stage = stage;
    opt.climate_dump = climate;
    vg::VanillaGen gen("data", opt, vd.biomes);
    // chunks present in the reference world
    std::string region = src->region_dir;
    std::vector<std::pair<int, int>> chunks;
    for (int cz = -16; cz < 16; cz++)
        for (int cx = -16; cx < 16; cx++) {
            Nbt root;
            if (anvil_read(region, cx, cz, root)) {
                const Nbt* st = root.get("Status");
                if (st && st->type == Nbt::String && st->s.find("full") != std::string::npos) chunks.emplace_back(cx, cz);
            }
        }
    printf("%zu chunks to compare in %s\n", chunks.size(), region.c_str());
    auto norm = [&](uint16_t id) {
        const BlockInfo& bi = B.info(id);
        if (bi.name == "minecraft:water" || bi.name == "minecraft:lava") return bi.name;
        return bi.props.empty() ? bi.name : bi.name + "[" + bi.props + "]";
    };
    std::map<std::pair<std::string, std::string>, long> pairs;
    long total = 0, diff = 0, biome_total = 0, biome_diff = 0, bad_chunks = 0;
    std::map<std::pair<std::string, std::string>, long> biome_pairs;
    for (auto [cx, cz] : chunks) {
        auto ref = src->chunk(cx, cz);
        Chunk mine;
        mine.x = cx;
        mine.z = cz;
        gen.generate(mine);
        long cd = 0;
        for (int s = 4 * (dim_id != 0); s < (dim_id == 0 ? kSections : 12 + 4 * (dim_id == 2)); s++) {
            for (int y = 0; y < 16; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        int wy = kMinY + s * 16 + y;
                        uint16_t a = mine.get(x, wy, z), b = ref->get(x, wy, z);
                        total++;
                        if (a != b && norm(a) != norm(b)) { diff++; cd++; pairs[{norm(a), norm(b)}]++; }
                    }
            const uint8_t* mb = mine.sec[s] ? mine.sec[s]->biome : mine.air_biomes[s];
            const uint8_t* rb = ref->sec[s] ? ref->sec[s]->biome : ref->air_biomes[s];
            for (int i = 0; i < 64; i++) {
                biome_total++;
                if (mb[i] != rb[i]) { biome_diff++; biome_pairs[{vd.biomes[mb[i]], vd.biomes[rb[i]]}]++; }
            }
        }
        if (cd) bad_chunks++;
        if (verbose && cd) printf("chunk %d %d: %ld differing blocks\n", cx, cz, cd);
    }
    printf("blocks: %ld of %ld differ (%.4f%%), %ld of %zu chunks have differences\n", diff, total, 100.0 * double(diff) / double(std::max(1L, total)), bad_chunks, chunks.size());
    std::vector<std::pair<long, std::pair<std::string, std::string>>> top;
    for (auto& kv : pairs) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < std::min<size_t>(12, top.size()); i++) printf("  ours %-40s vanilla %-40s %ld\n", top[i].second.first.c_str(), top[i].second.second.c_str(), top[i].first);
    printf("biome cells: %ld of %ld differ\n", biome_diff, biome_total);
    top.clear();
    for (auto& kv : biome_pairs) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < std::min<size_t>(8, top.size()); i++) printf("  ours %-28s vanilla %-28s %ld\n", top[i].second.first.c_str(), top[i].second.second.c_str(), top[i].first);
    return diff == 0 && biome_diff == 0 ? 0 : 1;
}
