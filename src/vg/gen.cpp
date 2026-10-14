#include "gen.hpp"
#include "biomes.hpp"
#include "blockdata.hpp"
#include "carver.hpp"
#include "climate.hpp"
#include "noisegen.hpp"
#include "surface.hpp"
#include <algorithm>
#include <cstring>
#include <map>
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
        bd = std::make_unique<BlockData>(data_dir + "/worldgen/" + o.version + "/blockflags.txt", reg);
        std::vector<SurfaceBiome> sb;
        for (auto& name : climate.biomes) {
            SurfaceBiome b;
            b.name = "minecraft:" + name;
            const Json& bj = reg.get("biome", name);
            b.temperature = bj.at("temperature").float_value();
            b.frozen = bj.get_string("temperature_modifier") == "frozen";
            sb.push_back(b);
        }
        SurfaceBlocks sbl;
        sbl.resolve = [this](const Json& j) { return bd->state(j); };
        sbl.by_name = [&](const std::string& n) { return BB.default_state(n); };
        sbl.flags = bd->chunk_flags();
        sbl.water = pal.water;
        surface = std::make_unique<SurfaceSystem>(*state, settings->at("surface_rule"), sbl, pal.default_block, ns.sea_level, std::max(ns.min_y, dim_min_y),
                                                  std::min(ns.min_y + ns.height, dim_min_y + dim_height) - std::max(ns.min_y, dim_min_y), settings->get_bool("legacy_random_source", false), std::move(sb),
                                                  [this](int qx, int qy, int qz) { return biome_value(qx, qy, qz); });
        if (opt.dim != 2) make_carvers();
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
};

VanillaGen::VanillaGen(const std::string& data_dir, const GenOptions& opt, const std::vector<std::string>& biome_names) : p_(std::make_unique<Impl>(data_dir, opt, biome_names)) {}
VanillaGen::~VanillaGen() = default;
int VanillaGen::min_y() const { return p_->dim_min_y; }
int VanillaGen::height() const { return p_->dim_height; }

void VanillaGen::generate(Chunk& c) const {
    Impl& I = *p_;
    const int first_section = (I.dim_min_y - kMinY) >> 4, sections = I.dim_height >> 4;
    // biomes
    for (int s = first_section; s < first_section + sections; s++) {
        int quart_y0 = (kMinY + s * 16) >> 2;
        for (int cy = 0; cy < 4; cy++)
            for (int cz = 0; cz < 4; cz++)
                for (int cx = 0; cx < 4; cx++) {
                    uint8_t b = I.biome_at(c.x * 4 + cx, quart_y0 + cy, c.z * 4 + cz);
                    c.air_biomes[s][cy * 16 + cz * 4 + cx] = b;
                }
    }
    // terrain
    ChunkFill fill(*I.state, I.ns, I.pal, c.x, c.z, I.dim_min_y, I.dim_height);
    ChunkBlocks cb;
    {
        std::vector<uint16_t> blocks;
        fill.fill(blocks);
        cb.init(fill.min_y(), fill.height(), std::move(blocks), I.bd->chunk_flags());
    }
    if (I.opt.stage != "noise") I.surface->build(cb, c.x, c.z, fill);
    if (I.carvers && I.opt.stage != "noise" && I.opt.stage != "surface")
        I.carvers->carve(cb, c.x, c.z, fill, [&I](int qx, int qz) { return I.biome_value(qx, 0, qz); });
    for (int s = first_section; s < first_section + sections; s++) {
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
}

}  // namespace pn::vg
