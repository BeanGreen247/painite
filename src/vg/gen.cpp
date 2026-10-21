#include "gen.hpp"
#include "biomes.hpp"
#include "blockdata.hpp"
#include "carver.hpp"
#include "climate.hpp"
#include "decorate.hpp"
#include "structure.hpp"
#include "template.hpp"
#include "beardifier.hpp"
#include "shape.hpp"
#include "noisegen.hpp"
#include "surface.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace pn::vg {

struct VanillaGen::Impl {
    GenOptions opt;
    Registry reg;
    const Json* settings;
    std::unique_ptr<WorldState> state;
    NoiseSettingsData ns;
    FillPalette pal;
    int dim_min_y = -64, dim_height = 384;
    std::vector<uint8_t> biome_index;   // climate list value -> server biome index
    ClimateData climate;
    NodeP t, h, c, e, d, w;
    std::unique_ptr<Compiler> sampler_comp;
    uint8_t plains = 0;
    std::unique_ptr<BlockData> bd;
    std::unique_ptr<SurfaceSystem> surface;
    std::unique_ptr<Carvers> carvers;
    std::unique_ptr<Decorator> decorator;
    std::unique_ptr<StructureState> structures;
    std::vector<std::string> structure_biome_names;   // "minecraft:<name>" per climate value
    std::vector<BiomeInfo> biome_infos;
    FeatureShared feature_shared;
    std::unique_ptr<TemplateManager> feature_templates;
    size_t quart_rows = 0;                   // quart cells per column over the dimension's height

    // a chunk on its way through the generation steps
    struct Proto {
        int cx = 0, cz = 0;
        std::mutex mu;
        bool built = false, decorated = false, emitted = false;
        int decorated_neighbors = 0;
        ChunkBlocks blocks;
        std::vector<uint8_t> biomes;       // climate value per quart cell: (qy * 4 + qz) * 4 + qx
        std::set<int> present;             // distinct values in biomes
    };
    std::mutex cache_mu, deco_mu;
    std::map<std::pair<int, int>, std::shared_ptr<Proto>> cache;

    std::shared_ptr<Proto> proto(int cx, int cz) {
        std::lock_guard<std::mutex> l(cache_mu);
        auto& slot = cache[{cx, cz}];
        if (!slot) { slot = std::make_shared<Proto>(); slot->cx = cx; slot->cz = cz; }
        return slot;
    }

    Impl(const std::string& data_dir, const GenOptions& o, const std::vector<std::string>& biome_names) : opt(o), reg(data_dir + "/worldgen/" + o.version) {
        if (!o.overlay.empty()) reg.set_overlay(o.overlay);
        std::string sname = o.dim == 0 ? (o.level_type == "large_biomes" ? "large_biomes" : o.level_type == "amplified" ? "amplified" : "overworld") : o.dim == 1 ? "nether" : "end";
        settings = &reg.get("noise_settings", sname);
        state = std::make_unique<WorldState>(reg, *settings, o.seed);
        ns = NoiseSettingsData::from_json(*settings);
        if (o.dim == 0) { dim_min_y = -64; dim_height = 384; } else { dim_min_y = 0; dim_height = 256; }
        const Blocks& B = Blocks::get();
        auto block_of = [&](const Json& j) {
            std::string name = j.at("Name").str, props;
            if (const Json* pr = j.find("Properties")) {
                std::vector<std::pair<std::string, std::string>> kv;
                for (auto& m : pr->obj) kv.emplace_back(m.first, m.second.str);
                std::sort(kv.begin(), kv.end());
                for (auto& p : kv) props += (props.empty() ? "" : ",") + p.first + "=" + p.second;
            }
            return B.find(name, props);
        };
        pal.air = 0;
        pal.default_block = block_of(settings->at("default_block"));
        pal.default_fluid = block_of(settings->at("default_fluid"));
        pal.lava = B.find("minecraft:lava", "level=0");
        pal.water = B.find("minecraft:water", "level=0");
        pal.copper_ore = B.default_state("minecraft:copper_ore");
        pal.raw_copper_block = B.default_state("minecraft:raw_copper_block");
        pal.granite = B.default_state("minecraft:granite");
        pal.deepslate_iron_ore = B.find("minecraft:deepslate_iron_ore", "");
        pal.raw_iron_block = B.default_state("minecraft:raw_iron_block");
        pal.tuff = B.default_state("minecraft:tuff");
        for (size_t i = 0; i < biome_names.size(); i++) if (biome_names[i] == "minecraft:plains") plains = uint8_t(i);
        // biomes: multi-noise climate search (the end uses its own source below)
        sampler_comp = std::make_unique<Compiler>(*state, nullptr);
        const Json& r = settings->at("noise_router");
        t = sampler_comp->compile(r.at("temperature"));
        h = sampler_comp->compile(r.at("vegetation"));
        c = sampler_comp->compile(r.at("continents"));
        e = sampler_comp->compile(r.at("erosion"));
        d = sampler_comp->compile(r.at("depth"));
        w = sampler_comp->compile(r.at("ridges"));
        if (o.dim == 2) climate.biomes = {"the_end", "end_highlands", "end_midlands", "small_end_islands", "end_barrens"};
        else if (!o.climate_dump.empty()) climate = load_climate_dump(o.climate_dump, o.dim == 1 ? "nether" : "overworld");
        else climate = o.dim == 1 ? nether_climate() : overworld_climate();
        for (auto& name : climate.biomes) {
            uint8_t idx = plains;
            for (size_t i = 0; i < biome_names.size(); i++) if (biome_names[i] == "minecraft:" + name) idx = uint8_t(i);
            biome_index.push_back(idx);
        }

        // surface rules
        const Blocks& BB = Blocks::get();
        bd = std::make_unique<BlockData>(data_dir + "/worldgen/" + o.version + "/blockflags.txt", data_dir + "/worldgen/" + o.version + "/blockclasses.txt", reg);
        feature_templates = std::make_unique<TemplateManager>(reg, *bd);
        std::vector<SurfaceBiome> sb;
        for (auto& name : climate.biomes) {
            SurfaceBiome b;
            b.name = "minecraft:" + name;
            const Json& bj = reg.get("biome", name);
            b.temperature = bj.at("temperature").float_value();
            b.frozen = bj.get_string("temperature_modifier") == "frozen";
            b.has_precipitation = bj.get_bool("has_precipitation", true);
            sb.push_back(b);
        }
        biome_infos = sb;
        SurfaceBlocks sbl;
        sbl.resolve = [this](const Json& j) { return bd->state(j); };
        sbl.by_name = [&](const std::string& n) { return BB.default_state(n); };
        sbl.flags = bd->chunk_flags();
        sbl.water = pal.water;
        surface = std::make_unique<SurfaceSystem>(*state, settings->at("surface_rule"), sbl, pal.default_block, ns.sea_level, std::max(ns.min_y, dim_min_y),
                                                  std::min(ns.min_y + ns.height, dim_min_y + dim_height) - std::max(ns.min_y, dim_min_y), settings->get_bool("legacy_random_source", false), std::move(sb),
                                                  [this](int qx, int qy, int qz) { return biome_value(qx, qy, qz); });
        if (opt.dim != 2) make_carvers();
        quart_rows = size_t(dim_height / 4);
        if (opt.dim != 2) check_y_independence();
        decorator = std::make_unique<Decorator>(reg, o.seed, climate.biomes);
        make_structures();
    }

    // terrain height of a column without building the chunk (NoiseBasedChunkGenerator.getBaseHeight / getBaseColumn)
    int base_height(int x, int z, HeightmapType t) const {
        ChunkFill col(*state, ns, pal, x, z, dim_min_y, dim_height, ChunkFill::ColumnTag{});
        const uint8_t* flags = bd->chunk_flags();
        auto tester = [flags, t](uint16_t id) { return t == WORLD_SURFACE_WG ? !(flags[id] & kFlagAir) : (flags[id] & kFlagBlocksMotion) != 0; };
        return col.column(x, z, tester);
    }
    void base_column(int x, int z, std::vector<uint16_t>& out) const {
        ChunkFill col(*state, ns, pal, x, z, dim_min_y, dim_height, ChunkFill::ColumnTag{});
        col.column(x, z, nullptr, &out);
    }

    void make_structures() {
        if (std::getenv("PAINITE_NO_STRUCTURES")) return;
        for (auto& n : climate.biomes) structure_biome_names.push_back("minecraft:" + n);
        StructureEnv env;
        env.seed = opt.seed;
        env.dim = opt.dim;
        env.min_y = dim_min_y;
        env.height = dim_height;
        env.gen_min_y = std::max(dim_min_y, ns.min_y);
        env.gen_height = std::min(dim_height, ns.height);
        env.sea_level = ns.sea_level;
        env.biome_at = [this](int qx, int qy, int qz) { return biome_value(qx, qy, qz); };
        env.biome_names = &structure_biome_names;
        env.base_height = [this](int x, int z, HeightmapType t) { return base_height(x, z, t); };
        env.base_column = [this](int x, int z, std::vector<uint16_t>& out) { base_column(x, z, out); };
        env.biome_infos = &biome_infos;
        env.reg = &reg;
        env.bd = bd.get();
        structures = std::make_unique<StructureState>(env, climate.biomes);
    }

    void make_carvers() {
        const Blocks& BB = Blocks::get();
        CarveBlocks cbk;
        cbk.air = 0;
        cbk.cave_air = BB.default_state("minecraft:cave_air");
        cbk.lava = pal.lava;
        cbk.water = pal.water;
        cbk.grass_block = BB.default_state("minecraft:grass_block");
        cbk.mycelium = BB.default_state("minecraft:mycelium");
        cbk.dirt = BB.default_state("minecraft:dirt");
        std::vector<std::vector<std::string>> per_biome;
        for (auto& name : climate.biomes) {
            std::vector<std::string> list;
            if (const Json* cj = reg.get("biome", name).find("carvers")) {
                if (cj->is_arr()) for (auto& e : cj->arr) list.push_back(e.str);
                else if (cj->is_str()) list.push_back(cj->str);
            }
            per_biome.push_back(list);
        }
        carvers = std::make_unique<Carvers>(reg, *state, surface.get(), cbk, BB.count(), [&BB](uint16_t id) { return BB.info(id).name; },
                                            GenRange{std::max(ns.min_y, dim_min_y), std::min(ns.min_y + ns.height, dim_min_y + dim_height) - std::max(ns.min_y, dim_min_y)}, std::move(per_biome));
    }

    int biome_value(int qx, int qy, int qz) const {
        if (opt.dim == 2) {   // TheEndBiomeSource
            int bx = qx * 4, by = qy * 4, bz = qz * 4;
            int cx = bx >> 4, cz = bz >> 4;
            if (int64_t(cx) * cx + int64_t(cz) * cz <= 4096) return 0;
            double h = e->compute(Ctx{((bx >> 4) * 2 + 1) * 8, by, ((bz >> 4) * 2 + 1) * 8, nullptr});
            if (h > 0.25) return 1;
            if (h >= -0.0625) return 2;
            return h < -0.21875 ? 3 : 4;
        }
        Ctx ctx{qx * 4, qy * 4, qz * 4, nullptr};
        TargetPoint tp{quantize_coord(float(t->compute(ctx))), quantize_coord(float(h->compute(ctx))), quantize_coord(float(c->compute(ctx))),
                       quantize_coord(float(e->compute(ctx))), quantize_coord(float(d->compute(ctx))), quantize_coord(float(w->compute(ctx)))};
        return climate.list->find_value(tp);
    }
    uint8_t biome_at(int qx, int qy, int qz) const { return biome_index[size_t(biome_value(qx, qy, qz))]; }

    // the climate functions other than depth do not depend on y: one sample per column serves every height
    struct Climate2D { int64_t t, h, c, e, w; };
    bool y_independent = false;
    Climate2D climate_2d(int qx, int qz) const {
        Ctx ctx{qx * 4, 0, qz * 4, nullptr};
        return {quantize_coord(float(t->compute(ctx))), quantize_coord(float(h->compute(ctx))), quantize_coord(float(c->compute(ctx))), quantize_coord(float(e->compute(ctx))), quantize_coord(float(w->compute(ctx)))};
    }
    int biome_at_depth(const Climate2D& k, int qx, int qy, int qz) const {
        TargetPoint tp{k.t, k.h, k.c, k.e, quantize_coord(float(d->compute(Ctx{qx * 4, qy * 4, qz * 4, nullptr}))), k.w};
        return climate.list->find_value(tp);
    }
    void check_y_independence() {
        y_independent = opt.dim != 2;
        for (int i = 0; i < 6 && y_independent; i++) {
            int qx = i * 37 - 100, qz = i * 91 - 200;
            for (int qy : {-16, 20, 60}) {
                Ctx a{qx * 4, 0, qz * 4, nullptr}, b{qx * 4, qy * 4, qz * 4, nullptr};
                for (NodeP n : {t, h, c, e, w})
                    if (n->compute(a) != n->compute(b)) { y_independent = false; break; }
            }
        }
    }
    // carver lists come from the biome at the chunk's corner at y 0: remembered per chunk
    mutable std::mutex carver_mu;
    mutable std::map<std::pair<int, int>, int> carver_cache;
    int carver_biome(int qx, int qz) const {
        std::pair<int, int> key{qx, qz};
        {
            std::lock_guard<std::mutex> l(carver_mu);
            auto it = carver_cache.find(key);
            if (it != carver_cache.end()) return it->second;
        }
        int v = biome_value(qx, 0, qz);
        std::lock_guard<std::mutex> l(carver_mu);
        carver_cache[key] = v;
        return v;
    }

    // BIOMES + NOISE + SURFACE + CARVERS of one chunk
    void build(Proto& p) {
        std::lock_guard<std::mutex> l(p.mu);
        if (p.built) return;
        auto t0 = std::chrono::steady_clock::now();
        auto lap = [&](const char* what) { if (!std::getenv("PAINITE_PROF")) return; auto t1 = std::chrono::steady_clock::now(); fprintf(stderr, "  [prof] %s %.1f ms\n", what, std::chrono::duration<double, std::milli>(t1 - t0).count()); t0 = t1; };
        p.biomes.assign(quart_rows * 16, 0);
        for (int qz = 0; qz < 4; qz++)
            for (int qx = 0; qx < 4; qx++) {
                Climate2D k{};
                if (y_independent) k = climate_2d(p.cx * 4 + qx, p.cz * 4 + qz);
                for (size_t qy = 0; qy < quart_rows; qy++) {
                    int yq = int(qy) + (dim_min_y >> 2);
                    int v = y_independent ? biome_at_depth(k, p.cx * 4 + qx, yq, p.cz * 4 + qz) : biome_value(p.cx * 4 + qx, yq, p.cz * 4 + qz);
                    p.biomes[(qy * 4 + size_t(qz)) * 4 + size_t(qx)] = uint8_t(v);
                    p.present.insert(v);
                }
            }
        lap("biomes");
        ChunkFill fill(*state, ns, pal, p.cx, p.cz, dim_min_y, dim_height);
        std::unique_ptr<Beardifier> beard;
        if (structures && structures->has_structures()) {
            beard = std::make_unique<Beardifier>(structures->starts_touching(p.cx, p.cz), p.cx, p.cz);
            if (!beard->empty()) fill.set_beardifier(beard.get());
        }
        lap("fill setup");
        std::vector<uint16_t> blocks;
        fill.fill(blocks);
        lap("fill");
        // quart cells of this chunk and the border the position fuzz can reach, sampled once
        const int min_q = dim_min_y >> 2;
        std::vector<uint8_t> memo(size_t(6 * 6) * quart_rows, 255);
        std::vector<std::pair<Climate2D, bool>> cols(36, {Climate2D{}, false});
        std::function<int(int, int, int)> lookup = [&](int qx, int qy, int qz) {
            int dx = qx - (p.cx * 4 - 1), dz = qz - (p.cz * 4 - 1);
            if (dx < 0 || dx >= 6 || dz < 0 || dz >= 6) return biome_value(qx, qy, qz);
            if (dx >= 1 && dx <= 4 && dz >= 1 && dz <= 4) return int(p.biomes[(size_t(qy - min_q) * 4 + size_t(dz - 1)) * 4 + size_t(dx - 1)]);
            uint8_t& m = memo[(size_t(qy - min_q) * 6 + size_t(dz)) * 6 + size_t(dx)];
            if (m == 255) {
                if (y_independent) {
                    auto& col = cols[size_t(dz) * 6 + size_t(dx)];
                    if (!col.second) { col.first = climate_2d(qx, qz); col.second = true; }
                    m = uint8_t(biome_at_depth(col.first, qx, qy, qz));
                } else m = uint8_t(biome_value(qx, qy, qz));
            }
            return int(m);
        };
        p.blocks.init(fill.min_y(), fill.height(), std::move(blocks), bd->chunk_flags());
        if (opt.stage != "noise") surface->build(p.blocks, p.cx, p.cz, fill, std::getenv("PAINITE_NOMEMO") ? nullptr : &lookup);
        lap("surface");
        if (carvers && opt.stage != "noise" && opt.stage != "surface")
            carvers->carve(p.blocks, p.cx, p.cz, fill, [this](int qx, int qz) { return carver_biome(qx, qz); }, &lookup);
        lap("carve");
        p.built = true;
    }

    // biome value of a quart cell inside the 3x3 protos (they already hold their biomes)
    static int proto_biome(Proto* const (&grid)[9], int center_x, int center_z, int qx, int qy, int qz, int min_q) {
        int dx = (qx >> 2) - center_x + 1, dz = (qz >> 2) - center_z + 1;
        if (dx < 0 || dx > 2 || dz < 0 || dz > 2) return 0;
        Proto* p = grid[dz * 3 + dx];
        return p->biomes[size_t((qy - min_q) * 16 + (qz & 3) * 4 + (qx & 3))];
    }

    void decorate_one(int cx, int cz) {
        for (int dz = -2; dz <= 2; dz++)
            for (int dx = -2; dx <= 2; dx++) build(*proto(cx + dx, cz + dz));
        std::shared_ptr<Proto> holders[9];
        Proto* grid[9];
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) { holders[(dz + 1) * 3 + dx + 1] = proto(cx + dx, cz + dz); grid[(dz + 1) * 3 + dx + 1] = holders[(dz + 1) * 3 + dx + 1].get(); }
        std::lock_guard<std::mutex> l(deco_mu);
        Proto& center = *grid[4];
        if (center.decorated) return;
        ChunkBlocks* chunks[9];
        std::set<int> biomes;
        for (int i = 0; i < 9; i++) {
            chunks[i] = &grid[i]->blocks;
            chunks[i]->prime_final();
            biomes.insert(grid[i]->present.begin(), grid[i]->present.end());
        }
        const int min_q = dim_min_y >> 2;
        Proto* const (&g)[9] = reinterpret_cast<Proto* const (&)[9]>(grid);
        Level level(*bd, cx, cz, chunks, GenRange{grid[4]->blocks.min_y, grid[4]->blocks.height}, [&](int x, int y, int z) {
            return surface->biome_with(x, y, z, [&](int qx, int qy, int qz) { return proto_biome(g, cx, cz, qx, qy, qz, min_q); });
        });
        if (opt.stage == "structures") biomes.clear();   // structures only: no biome feature runs
        FeatureEnv env{level, reg, feature_shared, [this](int biome, const std::string& id) { return decorator->biome_has_feature(biome, id); }};
        env.biomes = &biome_infos;
        env.templates = feature_templates.get();
        env.sea_level = ns.sea_level;
        env.seed = opt.seed;
        std::vector<std::shared_ptr<const StructureStart>> touching;
        bool have_touching = false;
        Decorator::StructureHook hook = [&](int step, WorldgenRandomXoro& random, int64_t seed) {
            if (!structures || !structures->has_structures()) return;
            if (!have_touching) { touching = structures->starts_touching(cx, cz); have_touching = true; }
            structures->place_step(step, touching, env, random, seed, cx, cz);
        };
        decorator->decorate(env, cx, cz, biomes, hook);
        center.decorated = true;
        for (int i = 0; i < 9; i++)
            if (i != 4) grid[i]->decorated_neighbors++;
    }
    // LevelChunk.postProcessGeneration: the shapes of the blocks structures marked (fences, torches, ladders, iron bars) are fixed once the chunk and its surroundings are complete
    void post_process(int cx, int cz, Proto& center) {
        for (int dz = -2; dz <= 2; dz++)
            for (int dx = -2; dx <= 2; dx++) decorate_one(cx + dx, cz + dz);
        std::shared_ptr<Proto> holders[9];
        Proto* grid[9];
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) { holders[(dz + 1) * 3 + dx + 1] = proto(cx + dx, cz + dz); grid[(dz + 1) * 3 + dx + 1] = holders[(dz + 1) * 3 + dx + 1].get(); }
        std::lock_guard<std::mutex> l(deco_mu);
        ChunkBlocks* chunks[9];
        for (int i = 0; i < 9; i++) chunks[i] = &grid[i]->blocks;
        const int min_q = dim_min_y >> 2;
        Proto* const (&g)[9] = reinterpret_cast<Proto* const (&)[9]>(grid);
        Level level(*bd, cx, cz, chunks, GenRange{center.blocks.min_y, center.blocks.height}, [&](int x, int y, int z) {
            return surface->biome_with(x, y, z, [&](int qx, int qy, int qz) { return proto_biome(g, cx, cz, qx, qy, qz, min_q); });
        });
        std::vector<std::array<int, 3>> marks;
        marks.swap(center.blocks.post_marks);
        for (auto& m : marks) {
            uint16_t state = level.get(m[0], m[1], m[2]);
            uint16_t updated = update_from_neighbour_shapes(*bd, level, BlockPos(m[0], m[1], m[2]), state);
            if (updated != state) center.blocks.set_raw(m[0], m[1], m[2], updated);
        }
    }
};

VanillaGen::VanillaGen(const std::string& data_dir, const GenOptions& opt, const std::vector<std::string>& biome_names) : p_(std::make_unique<Impl>(data_dir, opt, biome_names)) {}
VanillaGen::~VanillaGen() = default;
const std::vector<std::vector<std::string>>& VanillaGen::decorator_steps() const { return p_->decorator->step_features(); }
std::vector<std::string> VanillaGen::describe_structures(int cx0, int cz0, int cx1, int cz1) const {
    std::vector<std::string> out;
    if (!p_->structures) return out;
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++)
            for (auto& st : p_->structures->starts_at(cx, cz)) {
                BoundingBox b = st->bounds();
                char buf[256];
                snprintf(buf, sizeof buf, "%s %d %d pieces=%zu bb=[%d,%d,%d -> %d,%d,%d]", st->structure.c_str(), cx, cz, st->pieces.size(), b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
                out.push_back(buf);
            }
    return out;
}

std::vector<std::pair<std::string, std::vector<std::string>>> VanillaGen::structure_starts_detail(int cx, int cz) const {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    if (!p_->structures) return out;
    for (auto& st : p_->structures->starts_at(cx, cz)) {
        std::vector<std::string> pieces;
        for (auto& pc : st->pieces) pieces.push_back(pc->describe());
        out.emplace_back(st->structure, std::move(pieces));
    }
    return out;
}

int VanillaGen::debug_base_height(int x, int z, int heightmap) const { return p_->base_height(x, z, HeightmapType(heightmap)); }

void VanillaGen::place_structures_of(int cx, int cz) const {
    if (!p_->structures) return;
    for (auto& st : p_->structures->starts_at(cx, cz)) {
        BoundingBox b = st->bounds();
        for (int z = b.z0 >> 4; z <= b.z1 >> 4; z++)
            for (int x = b.x0 >> 4; x <= b.x1 >> 4; x++) p_->decorate_one(x, z);
    }
}

std::vector<std::string> VanillaGen::structure_names() const { return p_->structures ? p_->structures->structure_ids() : std::vector<std::string>(); }

bool VanillaGen::locate(const std::vector<std::string>& wanted, int bx, int bz, int& out_x, int& out_z) const {
    if (!p_->structures) return false;
    std::set<std::string> ids;
    std::function<void(const std::string&, int)> add = [&](const std::string& id, int depth) {
        if (depth > 8) return;
        if (!id.empty() && id[0] == '#') {
            try {
                const Json& t = p_->reg.tag("structure/" + Registry::strip(id.substr(1)));
                if (const Json* v = t.find("values"))
                    for (auto& e : v->arr) add(e.is_str() ? e.str : e.get_string("id"), depth + 1);
            } catch (...) {}
        } else ids.insert(id.rfind("minecraft:", 0) == 0 ? id : "minecraft:" + id);
    };
    for (auto& w : wanted) add(w, 0);
    return p_->structures->locate(ids, bx, bz, 100, out_x, out_z);
}

int VanillaGen::min_y() const { return p_->dim_min_y; }
int VanillaGen::height() const { return p_->dim_height; }

void VanillaGen::decorate(int cx, int cz) const { p_->decorate_one(cx, cz); }

void VanillaGen::generate(Chunk& c) const {
    Impl& I = *p_;
    const bool decorate = I.opt.stage == "features" || I.opt.stage == "full" || I.opt.stage == "structures";
    if (decorate)
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) I.decorate_one(c.x + dx, c.z + dz);
    std::shared_ptr<Impl::Proto> pc = I.proto(c.x, c.z);
    I.build(*pc);
    if (decorate && !pc->blocks.post_marks.empty()) I.post_process(c.x, c.z, *pc);
    const ChunkBlocks& cb = pc->blocks;
    const int first_section = (I.dim_min_y - kMinY) >> 4, sections = I.dim_height >> 4;
    for (int s = first_section; s < first_section + sections; s++) {
        int quart_y0 = ((kMinY + s * 16) >> 2) - (I.dim_min_y >> 2);
        for (int cy = 0; cy < 4; cy++)
            for (int cz = 0; cz < 4; cz++)
                for (int cx = 0; cx < 4; cx++)
                    c.air_biomes[s][cy * 16 + cz * 4 + cx] = I.biome_index[pc->biomes[size_t((quart_y0 + cy) * 16 + cz * 4 + cx)]];
        int y0 = kMinY + s * 16 - cb.min_y;
        if (y0 + 16 <= 0 || y0 >= cb.height) continue;
        bool any = false;
        for (int y = 0; y < 16 && !any; y++) {
            int yy = y0 + y;
            if (yy < 0 || yy >= cb.height) continue;
            const uint16_t* row = &cb.b[size_t(yy) * 256];
            for (int i = 0; i < 256; i++) if (row[i]) { any = true; break; }
        }
        if (!any) continue;
        auto sec = std::make_unique<Section>();
        memcpy(sec->biome, c.air_biomes[s], 64);
        for (int y = 0; y < 16; y++) {
            int yy = y0 + y;
            if (yy < 0 || yy >= cb.height) continue;
            memcpy(&sec->b[y * 256], &cb.b[size_t(yy) * 256], 256 * sizeof(uint16_t));
        }
        c.sec[s] = std::move(sec);
    }
    // what generation attached to blocks and the entities it created
    if (!cb.block_entities.empty() || !cb.entities.empty()) {
        if (c.extra.type != pn::Nbt::Compound) c.extra = pn::Nbt::compound();
        if (!cb.block_entities.empty()) {
            pn::Nbt list = pn::Nbt::list_of(pn::Nbt::Compound);
            for (auto& kv : cb.block_entities) list.list.push_back(kv.second);
            c.extra.put("block_entities", std::move(list));
        }
        c.gen_entities = cb.entities;
    }
    // settled chunks are no longer needed by their neighbours
    {
        std::lock_guard<std::mutex> l(I.cache_mu);
        pc->emitted = true;
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                auto it = I.cache.find({c.x + dx, c.z + dz});
                if (it != I.cache.end() && it->second->emitted && it->second->decorated_neighbors >= 8 && it->second->decorated) I.cache.erase(it);
            }
    }
}

}  // namespace pn::vg
