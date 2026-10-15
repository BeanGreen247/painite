// Structure sets, placement and start creation (ChunkGenerator.createStructures / createReferences, ChunkGeneratorStructureState).
#include "structure.hpp"
#include "template.hpp"
#include <algorithm>
#include <cmath>

namespace pn::vg {

std::map<std::string, StructureFactory>& structure_factories() {
    static std::map<std::string, StructureFactory> f;
    return f;
}

namespace {

int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

// a biome list as the data files write it: "#tag", "minecraft:biome" or a list of those
void collect_biomes(const Registry& reg, const Json& j, std::set<std::string>& out, int depth = 0) {
    if (depth > 16) return;
    if (j.is_arr()) { for (auto& e : j.arr) collect_biomes(reg, e, out, depth + 1); return; }
    if (j.is_obj()) { if (const Json* v = j.find("values")) collect_biomes(reg, *v, out, depth + 1); return; }
    if (!j.is_str()) return;
    const std::string& s = j.str;
    if (s.empty()) return;
    if (s[0] == '#') {
        std::string id = Registry::strip(s.substr(1));
        try { collect_biomes(reg, reg.tag("biome/" + id), out, depth + 1); } catch (...) {}
    } else out.insert(s.rfind("minecraft:", 0) == 0 ? s : "minecraft:" + s);
}

std::set<int> biome_indices(const Registry& reg, const Json& j, const std::vector<std::string>& names) {
    std::set<std::string> ids;
    collect_biomes(reg, j, ids);
    std::set<int> out;
    for (size_t i = 0; i < names.size(); i++) if (ids.count(names[i])) out.insert(int(i));
    return out;
}

int step_of(const std::string& s) {
    static const char* names[] = {"raw_generation", "lakes", "local_modifications", "underground_structures", "surface_structures", "strongholds",
                                  "underground_ores", "underground_decoration", "fluid_springs", "vegetal_decoration", "top_layer_modification"};
    for (int i = 0; i < 11; i++) if (s == names[i]) return i;
    return 4;
}

int terrain_of(const std::string& s) {
    if (s == "beard_thin") return 1;
    if (s == "beard_box") return 2;
    if (s == "bury") return 3;
    if (s == "encapsulate") return 4;
    return 0;
}

bool frequency_reducer(int method, int64_t seed, int salt, int x, int z, float probability) {
    WorldRandom r;
    switch (method) {
        case 1: {   // legacy_type_1 (pillager outposts)
            int cx = x >> 4, cz = z >> 4;
            r.set_seed(int64_t(cx ^ (cz << 4)) ^ seed);
            r.next_int();
            return r.next_int(int(1.0f / probability)) == 0;
        }
        case 2: r.set_large_feature_with_salt(seed, x, z, 10387320); return r.next_float() < probability;
        case 3: r.set_large_feature_seed(seed, x, z); return r.next_double() < double(probability);
        default: r.set_large_feature_with_salt(seed, salt, x, z); return r.next_float() < probability;
    }
}

ChunkPos potential_chunk(const StructurePlacement& p, int64_t seed, int sx, int sz) {
    int gx = floor_div(sx, p.spacing), gz = floor_div(sz, p.spacing);
    WorldRandom r;
    r.set_large_feature_with_salt(seed, gx, gz, p.salt);
    int limit = p.spacing - p.separation;
    auto eval = [&]() { return p.triangular ? (r.next_int(limit) + r.next_int(limit)) / 2 : r.next_int(limit); };
    int a = eval();
    int b = eval();
    return {gx * p.spacing + a, gz * p.spacing + b};
}

}  // namespace

std::string StructurePiece::describe() const {
    char buf[256];
    snprintf(buf, sizeof buf, "%s BB[%d,%d,%d,%d,%d,%d]", type.c_str(), bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1);
    return buf;
}

bool StructureContext::could_structure_exist_in_column(int x, int z, int min_y, int max_y) const {
    int qx = x >> 2, qz = z >> 2;
    for (int qy = min_y >> 2; qy <= (max_y >> 2); qy++)
        if (valid_biome(env.biome_at(qx, qy, qz))) return true;
    return false;
}

StructureState::StructureState(const StructureEnv& env, const std::vector<std::string>& climate_biomes) : env_(env) {
    const Registry& reg = *env_.reg;
    templates_ = std::make_unique<TemplateManager>(reg, *env_.bd);
    env_.templates = templates_.get();
    // the generator's possible biomes decide which sets exist here
    std::set<int> possible;
    for (size_t i = 0; i < climate_biomes.size(); i++) possible.insert(int(i));
    by_step_.assign(11, {});
    all_by_step_.assign(11, {});
    std::vector<std::string> structure_ids = reg.list("structure");
    std::sort(structure_ids.begin(), structure_ids.end());
    for (auto& id : structure_ids) {
        const Json& j = reg.get("structure", id);
        all_by_step_[size_t(step_of(j.get_string("step", "surface_structures")))].push_back("minecraft:" + Registry::strip(id));
        std::string type = j.get_string("type");
        auto it = structure_factories().find(type);
        if (it == structure_factories().end()) continue;
        std::unique_ptr<Structure> s = it->second(j, reg);
        if (!s) continue;
        s->id = "minecraft:" + Registry::strip(id);
        s->biomes = biome_indices(reg, j.at("biomes"), *env_.biome_names);
        s->step = step_of(j.get_string("step", "surface_structures"));
        s->terrain_adaptation = terrain_of(j.get_string("terrain_adaptation", "none"));
        // Structure.biomes() must overlap this dimension's biomes for the structure to be placeable
        bool any = false;
        for (int b : s->biomes) if (possible.count(b)) any = true;
        if (!any) continue;
        by_step_[size_t(s->step)].push_back(s->id);
        structures_[s->id] = std::move(s);
    }
    std::vector<std::string> set_ids = reg.list("structure_set");
    std::sort(set_ids.begin(), set_ids.end());
    for (auto& id : set_ids) {
        const Json& j = reg.get("structure_set", id);
        StructureSetData set;
        set.id = "minecraft:" + Registry::strip(id);
        for (auto& e : j.at("structures").arr) {
            std::string sid = e.at("structure").str;
            if (sid.rfind("minecraft:", 0) != 0) sid = "minecraft:" + sid;
            if (!structures_.count(sid)) continue;
            set.structures.push_back({sid, int(e.get_int("weight", 1))});
        }
        if (set.structures.empty()) continue;
        const Json& pj = j.at("placement");
        StructurePlacement& p = set.placement;
        std::string t = pj.get_string("type");
        p.type = t == "minecraft:concentric_rings" ? StructurePlacement::CONCENTRIC_RINGS : StructurePlacement::RANDOM_SPREAD;
        p.salt = int(pj.get_int("salt", 0));
        p.frequency = pj.has("frequency") ? pj.at("frequency").float_value() : 1.0f;
        std::string fm = pj.get_string("frequency_reduction_method", "default");
        p.frequency_method = fm == "legacy_type_1" ? 1 : fm == "legacy_type_2" ? 2 : fm == "legacy_type_3" ? 3 : 0;
        if (const Json* lo = pj.find("locate_offset")) { p.locate_x = int(lo->arr[0].int_value()); p.locate_y = int(lo->arr[1].int_value()); p.locate_z = int(lo->arr[2].int_value()); }
        if (const Json* ez = pj.find("exclusion_zone")) {
            p.exclusion_set = ez->at("other_set").str;
            if (p.exclusion_set.rfind("minecraft:", 0) != 0) p.exclusion_set = "minecraft:" + p.exclusion_set;
            p.exclusion_chunks = int(ez->get_int("chunk_count", 0));
        }
        p.spacing = int(pj.get_int("spacing", 0));
        p.separation = int(pj.get_int("separation", 0));
        p.triangular = pj.get_string("spread_type") == "triangular";
        p.distance = int(pj.get_int("distance", 0));
        p.spread = int(pj.get_int("spread", 0));
        p.count = int(pj.get_int("count", 0));
        if (const Json* pb = pj.find("preferred_biomes")) p.preferred_biomes = biome_indices(reg, *pb, *env_.biome_names);
        if (p.type == StructurePlacement::CONCENTRIC_RINGS) p.rings = ring_positions(p);
        sets_.push_back(std::move(set));
    }
}

StructureState::~StructureState() = default;

// ChunkGeneratorStructureState.generateRingPositions
std::vector<ChunkPos> StructureState::ring_positions(const StructurePlacement& pl) {
    std::vector<ChunkPos> out;
    if (pl.count == 0) return out;
    int distance = pl.distance, count = pl.count, spread = pl.spread;
    LegacyRandom random(0);
    random.set_seed(env_.seed);
    double angle = random.next_double() * M_PI * 2.0;
    int in_circle = 0, circle = 0;
    for (int i = 0; i < count; i++) {
        double dist = double(4 * distance + distance * circle * 6) + (random.next_double() - 0.5) * (double(distance) * 2.5);
        int ix = int(std::llround(std::cos(angle) * dist));
        int iz = int(std::llround(std::sin(angle) * dist));
        std::unique_ptr<Rng> search = random.fork();
        // BiomeSource.findBiomeHorizontal(x, 0, z, 112, preferred, search, state): every cell of the square, one picked at random
        int ox = ix * 16 + 8, oz = iz * 16 + 8;
        int ncx = ox >> 2, ncz = oz >> 2, nr = 112 >> 2, ny = 0;
        bool have = false;
        int rx = 0, rz = 0, found = 0;
        for (int z = -nr; z <= nr; z++)
            for (int x = -nr; x <= nr; x++) {
                int qx = ncx + x, qz = ncz + z;
                if (!pl.preferred_biomes.count(env_.biome_at(qx, ny, qz))) continue;
                if (!have || search->next_int(found + 1) == 0) { rx = qx * 4; rz = qz * 4; have = true; }
                found++;
            }
        if (have) out.push_back({rx >> 4, rz >> 4});
        else out.push_back({ix, iz});
        angle += M_PI * 2.0 / double(spread);
        if (++in_circle != spread) continue;
        in_circle = 0;
        spread += 2 * spread / (++circle + 1);
        spread = std::min(spread, count - i);
        angle += random.next_double() * M_PI * 2.0;
    }
    return out;
}

bool StructureState::is_structure_chunk(const StructureSetData& set, int cx, int cz) {
    const StructurePlacement& p = set.placement;
    bool placement_chunk;
    if (p.type == StructurePlacement::RANDOM_SPREAD) {
        ChunkPos c = potential_chunk(p, env_.seed, cx, cz);
        placement_chunk = c.x == cx && c.z == cz;
    } else {
        placement_chunk = std::find(p.rings.begin(), p.rings.end(), ChunkPos{cx, cz}) != p.rings.end();
    }
    if (!placement_chunk) return false;
    if (p.frequency < 1.0f && !frequency_reducer(p.frequency_method, env_.seed, p.salt, cx, cz, p.frequency)) return false;
    if (!p.exclusion_set.empty()) {
        for (auto& other : sets_)
            if (other.id == p.exclusion_set) {
                for (int tx = cx - p.exclusion_chunks; tx <= cx + p.exclusion_chunks; tx++)
                    for (int tz = cz - p.exclusion_chunks; tz <= cz + p.exclusion_chunks; tz++)
                        if (is_structure_chunk(other, tx, tz)) return false;
                break;
            }
    }
    return true;
}

bool StructureState::try_generate(const std::string& sid, int cx, int cz, std::vector<std::shared_ptr<const StructureStart>>& out) {
    Structure& s = *structures_[sid];
    WorldRandom random;
    random.set_large_feature_seed(env_.seed, cx, cz);
    StructureContext ctx{env_, random, {cx, cz}, [&s](int b) { return s.biomes.count(b) != 0; }};
    std::optional<GenerationStub> stub = s.find_generation_point(ctx);
    if (!stub) return false;
    // Structure.findValidGenerationPoint: the biome at the start position must be allowed
    if (!s.biomes.count(env_.biome_at(stub->position.x >> 2, stub->position.y >> 2, stub->position.z >> 2))) return false;
    auto start = std::make_shared<StructureStart>();
    start->structure = s.id;
    start->pos = {cx, cz};
    start->step = s.step;
    start->terrain_adaptation = s.terrain_adaptation;
    stub->generate(start->pieces);
    if (!start->valid()) return false;
    out.push_back(std::move(start));
    return true;
}

std::vector<std::shared_ptr<const StructureStart>> StructureState::starts_at(int cx, int cz) {
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = cache_.find({cx, cz});
        if (it != cache_.end()) return it->second;
    }
    std::vector<std::shared_ptr<const StructureStart>> out;
    for (auto& set : sets_) {
        if (!is_structure_chunk(set, cx, cz)) continue;
        if (set.structures.size() == 1) { try_generate(set.structures[0].structure, cx, cz, out); continue; }
        std::vector<StructureSelection> options = set.structures;
        WorldRandom random;
        random.set_large_feature_seed(env_.seed, cx, cz);
        int total = 0;
        for (auto& o : options) total += o.weight;
        while (!options.empty()) {
            int choice = random.next_int(total);
            size_t index = 0;
            for (size_t k = 0; k < options.size(); k++) {
                choice -= options[k].weight;
                if (choice < 0) break;
                index++;
            }
            StructureSelection selected = options[index];
            if (try_generate(selected.structure, cx, cz, out)) break;
            options.erase(options.begin() + long(index));
            total -= selected.weight;
        }
    }
    std::lock_guard<std::mutex> l(mu_);
    return cache_.emplace(ChunkPos{cx, cz}, std::move(out)).first->second;
}

void StructureState::place_step(int step, const std::vector<std::shared_ptr<const StructureStart>>& touching, FeatureEnv& fenv, Rng& random, int64_t decoration_seed, int cx, int cz) {
    if (step < 0 || size_t(step) >= all_by_step_.size() || touching.empty()) return;
    const BoundingBox chunk_bb(cx * 16, env_.min_y + 1, cz * 16, cx * 16 + 15, env_.min_y + env_.height - 1, cz * 16 + 15);
    PieceCtx ctx{fenv, fenv.level, env_, *templates_, random, chunk_bb, {cx, cz}};
    const auto& ids = all_by_step_[size_t(step)];
    for (size_t index = 0; index < ids.size(); index++) {
        bool any = false;
        for (auto& st : touching) if (st->structure == ids[index]) { any = true; break; }
        if (!any) continue;
        random.set_seed(decoration_seed + int64_t(index) + int64_t(10000) * int64_t(step));
        for (auto& st : touching) {
            if (st->structure != ids[index] || st->pieces.empty()) continue;
            const BoundingBox center_bb = st->pieces[0]->bb;
            BlockPos center = center_bb.center();
            BlockPos reference(center.x, center_bb.y0, center.z);
            for (auto& piece : st->pieces)
                if (piece->bb.intersects(chunk_bb)) piece->post_process(ctx, reference);
        }
    }
}

std::vector<std::shared_ptr<const StructureStart>> StructureState::starts_touching(int cx, int cz) {
    std::vector<std::shared_ptr<const StructureStart>> out;
    if (sets_.empty()) return out;
    int bx = cx * 16, bz = cz * 16;
    for (int sx = cx - 8; sx <= cx + 8; sx++)
        for (int sz = cz - 8; sz <= cz + 8; sz++)
            for (auto& st : starts_at(sx, sz))
                if (st->bounds().intersects(bx, bz, bx + 15, bz + 15)) out.push_back(st);
    return out;
}

}  // namespace pn::vg
