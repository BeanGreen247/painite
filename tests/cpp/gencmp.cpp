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
#include <iostream>
#include <set>
#include <algorithm>
#include <tuple>
#include <string>

namespace pn { int64_t now_ms() { return 0; } }   // World wants a clock; the comparison does not
using namespace pn;

int main(int argc, char** argv) {
    std::vector<int> bh_x, bh_z;
    bool has_only = false, dump_features = false, list_structures = false, starts_mode = false;
    int sx0 = 0, sz0 = 0, sx1 = 0, sz1 = 0, scan_x0 = -16, scan_z0 = -16, scan_x1 = 15, scan_z1 = 15;
    int layer = INT32_MIN;
    bool probe = false;
    bool hist = false, has_col = false;
    int col_x = 0, col_z = 0, col_lo = 0, col_hi = 0;
    int smoke = -1;
    std::string histy;
    std::string vfilter;
    int only_x = 0, only_z = 0;
    std::string order;
    std::string overlay, climate, world_dir, version = "26.2", stage = "noise", dim = "overworld", type = "normal";
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
        else if (a == "--overlay") overlay = val();
        else if (a == "--order") order = val();
        else if (a == "--probe") probe = true;
        else if (a == "--scan") { scan_x0 = std::stoi(val()); scan_z0 = std::stoi(val()); scan_x1 = std::stoi(val()); scan_z1 = std::stoi(val()); }
        else if (a == "--bh") { bh_x.push_back(std::stoi(val())); bh_z.push_back(std::stoi(val())); }
        else if (a == "--starts") starts_mode = true;
        else if (a == "--structures" || a == "--range") { list_structures = a == "--structures"; sx0 = std::stoi(val()); sz0 = std::stoi(val()); sx1 = std::stoi(val()); sz1 = std::stoi(val()); }
        else if (a == "--dump-features") dump_features = true;
        else if (a == "--hist") hist = true;
        else if (a == "--column") { col_x = std::stoi(val()); col_z = std::stoi(val()); has_col = true; col_lo = std::stoi(val()); col_hi = std::stoi(val()); }
        else if (a == "--smoke") smoke = std::stoi(val());
        else if (a == "--histy") histy = val();
        else if (a == "--vname") vfilter = val();
        else if (a == "--layer") layer = std::stoi(val());
        else if (a == "--only") { only_x = std::stoi(val()); only_z = std::stoi(val()); has_only = true; }
        else if (a == "-v") verbose++;
        else if (a == "-vv") verbose += 2;
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
    opt.overlay = overlay;
    vg::VanillaGen gen("data", opt, vd.biomes);
    if (!bh_x.empty()) {
        for (size_t i = 0; i < bh_x.size(); i++) printf("baseheight %d %d %d\n", bh_x[i], bh_z[i], gen.debug_base_height(bh_x[i], bh_z[i], 0));
        return 0;
    }
    if (starts_mode) {   // structure starts of the reference world against ours, over the --structures chunk range
        int checked = 0, bad = 0, total_pieces = 0, bad_pieces = 0;
        // pieces adjust themselves while they are placed (heights, boxes): place the structures of the range first, as Mojang's saved chunks have been placed
        for (int cz = sz0; cz <= sz1; cz++)
            for (int cx = sx0; cx <= sx1; cx++) gen.place_structures_of(cx, cz);
        for (int cz = sz0; cz <= sz1; cz++)
            for (int cx = sx0; cx <= sx1; cx++) {
                std::shared_ptr<Chunk> c = src->chunk(cx, cz);
                std::map<std::string, std::vector<std::string>> vanilla;
                if (const Nbt* st = c->extra.get("structures"))
                    if (const Nbt* starts = st->get("starts"))
                        for (auto& kv : starts->map) {
                            std::vector<std::string> pieces;
                            if (const Nbt* ch = kv.second.get("Children"))
                                for (auto& pc : ch->list) {
                                    char buf[512];
                                    const Nbt* bb = pc.get("BB");
                                    std::string id = pc.gets("id");
                                    if (bb && bb->ia.size() == 6) {
                                        if (pc.get("PosX")) {
                                            const Nbt* pe = pc.get("pool_element");
                                            std::string loc = pe ? pe->gets("location") : "";
                                            std::string et = pe ? pe->gets("element_type") : "";
                                            if (et == "minecraft:list_pool_element") loc = "list";
                                            else if (et == "minecraft:feature_pool_element") loc = pe->gets("feature");
                                            else if (et == "minecraft:empty_pool_element") loc = "empty";
                                            snprintf(buf, sizeof buf, "%s BB[%d,%d,%d,%d,%d,%d] pos(%d,%d,%d) gld=%d rot=%s el=%s", id.c_str(), bb->ia[0], bb->ia[1], bb->ia[2], bb->ia[3], bb->ia[4], bb->ia[5],
                                                     int(pc.geti("PosX")), int(pc.geti("PosY")), int(pc.geti("PosZ")), int(pc.geti("ground_level_delta")), pc.gets("rotation").c_str(), loc.c_str());
                                        } else snprintf(buf, sizeof buf, "%s BB[%d,%d,%d,%d,%d,%d]", id.c_str(), bb->ia[0], bb->ia[1], bb->ia[2], bb->ia[3], bb->ia[4], bb->ia[5]);
                                        pieces.push_back(buf);
                                    }
                                }
                            if (kv.second.gets("id") != "INVALID" && kv.second.get("Children") && !kv.second.get("Children")->list.empty()) vanilla[kv.first] = pieces;
                        }
                std::map<std::string, std::vector<std::string>> ours;
                for (auto& e : gen.structure_starts_detail(cx, cz)) ours[e.first] = e.second;
                if (vanilla.empty() && ours.empty()) continue;
                checked++;
                bool ok = true;
                for (auto& kv : vanilla) {
                    auto it = ours.find(kv.first);
                    if (it == ours.end()) { printf("chunk %d,%d: vanilla has %s (%zu pieces), we have none\n", cx, cz, kv.first.c_str(), kv.second.size()); ok = false; continue; }
                    total_pieces += int(kv.second.size());
                    size_t n = std::max(kv.second.size(), it->second.size());
                    int shown = 0;
                    if (kv.second.size() != it->second.size()) { printf("chunk %d,%d %s: %zu pieces vs ours %zu\n", cx, cz, kv.first.c_str(), kv.second.size(), it->second.size()); ok = false; }
                    for (size_t i = 0; i < n; i++) {
                        std::string a = i < kv.second.size() ? kv.second[i] : "-", b = i < it->second.size() ? it->second[i] : "-";
                        if (verbose >= 2 && a == b) printf("  piece %zu same: %s\n", i, a.c_str());
                        if (a != b) { bad_pieces++; ok = false; if (shown++ < (verbose ? 100 : 3)) printf("  piece %zu\n    vanilla %s\n    ours    %s\n", i, a.c_str(), b.c_str()); }
                    }
                }
                for (auto& kv : ours) if (!vanilla.count(kv.first)) { printf("chunk %d,%d: we have %s (%zu pieces), vanilla has none\n", cx, cz, kv.first.c_str(), kv.second.size()); ok = false; }
                if (!ok) bad++;
            }
        printf("starts: %d chunks checked, %d differ; %d pieces, %d differ\n", checked, bad, total_pieces, bad_pieces);
        return bad ? 1 : 0;
    }
    if (list_structures) {
        for (auto& l : gen.describe_structures(sx0, sz0, sx1, sz1)) printf("%s\n", l.c_str());
        return 0;
    }
    if (smoke >= 0) {
        std::map<std::string, int> errors;
        int ok = 0;
        for (int cz = -smoke; cz <= smoke; cz++)
            for (int cx = -smoke; cx <= smoke; cx++) {
                Chunk c;
                c.x = cx; c.z = cz;
                try { gen.generate(c); ok++; } catch (const std::exception& e) { errors[e.what()]++; }
            }
        printf("%d chunks generated\n", ok);
        for (auto& kv : errors) printf("  %5d x %s\n", kv.second, kv.first.c_str());
        return errors.empty() ? 0 : 1;
    }
    if (dump_features) {
        for (size_t st = 0; st < gen.decorator_steps().size(); st++) {
            printf("step %zu:", st);
            for (size_t i = 0; i < gen.decorator_steps()[st].size(); i++) printf(" %zu=%s", i, gen.decorator_steps()[st][i].c_str());
            printf("\n");
        }
        return 0;
    }
    // chunks present in the reference world
    std::string region = src->region_dir;
    std::vector<std::pair<int, int>> chunks;
    for (int cz = scan_z0; cz <= scan_z1; cz++)
        for (int cx = scan_x0; cx <= scan_x1; cx++) {
            Nbt root;
            if (anvil_read(region, cx, cz, root)) {
                const Nbt* st = root.get("Status");
                if (st && st->type == Nbt::String && st->s.find("full") != std::string::npos) chunks.emplace_back(cx, cz);
            }
        }
    printf("%zu chunks to compare in %s\n", chunks.size(), region.c_str());
    if (!order.empty()) {   // decorate every chunk that vanilla decorated (the 3x3 around each full chunk) in a chosen global order
        std::set<std::pair<int, int>> todo;
        for (auto [cx, cz] : chunks) for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) todo.insert({cx + dx, cz + dz});
        std::vector<std::pair<int, int>> seq(todo.begin(), todo.end());   // sorted by x then z
        auto dist = [](std::pair<int, int> c) { return int64_t(c.first) * c.first + int64_t(c.second) * c.second; };
        if (order == "rowmajor") std::sort(seq.begin(), seq.end(), [](auto a, auto b) { return std::tie(a.second, a.first) < std::tie(b.second, b.first); });
        else if (order == "rowmajor_desc") std::sort(seq.begin(), seq.end(), [](auto a, auto b) { return std::tie(a.second, a.first) > std::tie(b.second, b.first); });
        else if (order == "colmajor_desc") std::reverse(seq.begin(), seq.end());
        else if (order == "dist") std::stable_sort(seq.begin(), seq.end(), [&](auto a, auto b) { return dist(a) < dist(b); });
        else if (order == "dist_desc") std::stable_sort(seq.begin(), seq.end(), [&](auto a, auto b) { return dist(a) > dist(b); });
        else if (order != "colmajor") { fprintf(stderr, "unknown order\n"); return 1; }
        for (auto [cx, cz] : seq) gen.decorate(cx, cz);
    }
    auto norm = [&](uint16_t id) {
        const BlockInfo& bi = B.info(id);
        if (bi.name == "minecraft:water" || bi.name == "minecraft:lava") return bi.name;
        return bi.props.empty() ? bi.name : bi.name + "[" + bi.props + "]";
    };
    std::map<std::pair<std::string, std::string>, long> pairs;
    long shown = 0, total = 0, diff = 0, biome_total = 0, biome_diff = 0, bad_chunks = 0;
    std::map<std::pair<std::string, std::string>, long> biome_pairs;
    for (auto [cx, cz] : chunks) {
        if (has_only && (cx != only_x || cz != only_z)) continue;
        auto ref = src->chunk(cx, cz);
        Chunk mine;
        mine.x = cx;
        mine.z = cz;
        gen.generate(mine);
        if (layer != INT32_MIN) {
            auto ch = [&](uint16_t id) {
                const std::string& n = B.info(id).name;
                if (n == "minecraft:air" || n == "minecraft:cave_air") return '.';
                if (n == "minecraft:stone") return 's'; if (n == "minecraft:granite") return 'g'; if (n == "minecraft:diorite") return 'd'; if (n == "minecraft:andesite") return 'a';
                if (n == "minecraft:tuff") return 't'; if (n == "minecraft:deepslate") return 'D'; if (n == "minecraft:clay") return 'c'; if (n == "minecraft:gravel") return 'r';
                if (n == "minecraft:dirt") return 'i'; if (n == "minecraft:sand") return 'S'; if (n == "minecraft:water") return '~'; if (n == "minecraft:grass_block") return 'G';
                if (n.find("_ore") != std::string::npos) return 'O'; return '?';
            };
            printf("layer %d of chunk %d %d (ours | vanilla)\n", layer, cx, cz);
            for (int z = 0; z < 16; z++) {
                for (int x = 0; x < 16; x++) putchar(ch(mine.get(x, layer, z)));
                printf("   ");
                for (int x = 0; x < 16; x++) putchar(ch(ref->get(x, layer, z)));
                putchar('\n');
            }
        }
        if (probe) {   // stdin: "x y z radius name" -> blocks whose name contains `name` in the box (clipped to this chunk), ours and vanilla
            std::string line;
            while (std::getline(std::cin, line)) {
                int px, py, pz, pr;
                char nm[128];
                if (sscanf(line.c_str(), "%d %d %d %d %127s", &px, &py, &pz, &pr, nm) != 5) continue;
                long a = 0, b = 0;
                for (int y = py - pr; y <= py + pr; y++)
                    for (int z = std::max(pz - pr, cz * 16); z <= std::min(pz + pr, cz * 16 + 15); z++)
                        for (int x = std::max(px - pr, cx * 16); x <= std::min(px + pr, cx * 16 + 15); x++) {
                            if (y < kMinY || y >= kMinY + kHeight) continue;
                            if (B.info(mine.get(x & 15, y, z & 15)).name.find(nm) != std::string::npos) a++;
                            if (B.info(ref->get(x & 15, y, z & 15)).name.find(nm) != std::string::npos) b++;
                        }
                printf("probe %d %d %d r%d %s: ours %ld vanilla %ld\n", px, py, pz, pr, nm, a, b);
            }
        }
        if (hist) {
            std::map<std::string, std::pair<long, long>> h;
            for (int y = kMinY; y < kMinY + kHeight; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) { h[norm(mine.get(x, y, z))].first++; h[norm(ref->get(x, y, z))].second++; }
            for (auto& kv : h) printf("  %-50s ours %7ld vanilla %7ld\n", kv.first.c_str(), kv.second.first, kv.second.second);
        }
        if (!histy.empty()) {
            for (int y = kMinY; y < kMinY + kHeight; y++) {
                long a = 0, b = 0;
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) { if (norm(mine.get(x, y, z)).find(histy) != std::string::npos) a++; if (norm(ref->get(x, y, z)).find(histy) != std::string::npos) b++; }
                if (a || b) printf("  y=%d ours %ld vanilla %ld\n", y, a, b);
            }
        }
        if (has_col && (col_x >> 4) == cx && (col_z >> 4) == cz) {
            printf("column %d %d (ours | vanilla)\n", col_x, col_z);
            for (int y = col_hi; y >= col_lo; y--) printf("  %4d %-45s %s\n", y, norm(mine.get(col_x & 15, y, col_z & 15)).c_str(), norm(ref->get(col_x & 15, y, col_z & 15)).c_str());
        }
        long cd = 0;
        for (int s = 4 * (dim_id != 0); s < (dim_id == 0 ? kSections : 12); s++) {
            for (int y = 0; y < 16; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        int wy = kMinY + s * 16 + y;
                        uint16_t a = mine.get(x, wy, z), b = ref->get(x, wy, z);
                        total++;
                        if (a != b && norm(a) != norm(b)) { diff++; cd++; pairs[{norm(a), norm(b)}]++; if (verbose > 1 && (vfilter.empty() || norm(b).find(vfilter) != std::string::npos || norm(a).find(vfilter) != std::string::npos) && shown++ < 40) printf("  %d %d %d: ours %s vanilla %s\n", cx * 16 + x, wy, cz * 16 + z, norm(a).c_str(), norm(b).c_str()); }
                    }
            const uint8_t* mb = mine.sec[s] ? mine.sec[s]->biome : mine.air_biomes[s];
            const uint8_t* rb = ref->sec[s] ? ref->sec[s]->biome : ref->air_biomes[s];
            for (int i = 0; i < 64; i++) {
                biome_total++;
                if (mb[i] != rb[i]) { if (verbose > 1) printf("  biome cell chunk %d %d section %d cell %d: ours %s vanilla %s\n", cx, cz, s, i, vd.biomes[mb[i]].c_str(), vd.biomes[rb[i]].c_str()); biome_diff++; biome_pairs[{vd.biomes[mb[i]], vd.biomes[rb[i]]}]++; }
            }
        }
        if (cd) bad_chunks++;
        if (verbose && cd) printf("chunk %d %d: %ld differing blocks\n", cx, cz, cd);
    }
    printf("blocks: %ld of %ld differ (%.4f%%), %ld of %zu chunks have differences\n", diff, total, 100.0 * double(diff) / double(std::max(1L, total)), bad_chunks, chunks.size());
    std::vector<std::pair<long, std::pair<std::string, std::string>>> top;
    for (auto& kv : pairs) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < std::min<size_t>(verbose ? 60 : 12, top.size()); i++) printf("  ours %-40s vanilla %-40s %ld\n", top[i].second.first.c_str(), top[i].second.second.c_str(), top[i].first);
    printf("biome cells: %ld of %ld differ\n", biome_diff, biome_total);
    top.clear();
    for (auto& kv : biome_pairs) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < std::min<size_t>(8, top.size()); i++) printf("  ours %-28s vanilla %-28s %ld\n", top[i].second.first.c_str(), top[i].second.second.c_str(), top[i].first);
    return diff == 0 && biome_diff == 0 ? 0 : 1;
}
