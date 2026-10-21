// Structure sets, placement and start creation (ChunkGenerator.createStructures / createReferences, ChunkGeneratorStructureState).
#include "structure.hpp"
#include "template.hpp"
#include "jcompat.hpp"
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

// VerticalAnchor.resolveY
static int resolve_anchor(const Json& a, int min_y, int height) {
    if (const Json* v = a.find("absolute")) return int(v->int_value());
    if (const Json* v = a.find("above_bottom")) return min_y + int(v->int_value());
    if (const Json* v = a.find("below_top")) return (height - 1 + min_y) - int(v->int_value());
    return 0;
}

std::function<int(Rng&, int, int)> make_height_provider(const Json& sh) {
    std::string type = Registry::strip(sh.get_string("type"));
    if (type == "uniform") {
        Json lo = sh.at("min_inclusive"), hi = sh.at("max_inclusive");
        return [lo, hi](Rng& r, int min_y, int height) {
            int a = resolve_anchor(lo, min_y, height), b = resolve_anchor(hi, min_y, height);
            if (a > b) return a;
            return r.next_int(b - a + 1) + a;
        };
    }
    Json anchor = type == "constant" ? sh.at("value") : sh;
    return [anchor](Rng&, int min_y, int height) { return resolve_anchor(anchor, min_y, height); };
}

std::optional<GenerationStub> on_top_of_chunk_center_without_biome_check(const StructureContext& ctx, HeightmapType heightmap, std::function<void(std::vector<std::shared_ptr<StructurePiece>>&)> generator) {
    int x = ctx.middle_x(), z = ctx.middle_z();
    int y = ctx.first_occupied_height(x, z, heightmap);
    GenerationStub stub;
    stub.position = BlockPos(x, y, z);
    stub.generate = std::move(generator);
    return stub;
}

std::optional<GenerationStub> on_top_of_chunk_center(const StructureContext& ctx, HeightmapType heightmap, std::function<void(std::vector<std::shared_ptr<StructurePiece>>&)> generator) {
    if (!ctx.could_valid_biome_exist_on_top_of_chunk_center()) return std::nullopt;
    return on_top_of_chunk_center_without_biome_check(ctx, heightmap, std::move(generator));
}

int structure_lowest_y(const StructureContext& ctx, int min_x, int min_z, int size_x, int size_z) {
    int a = ctx.first_occupied_height(min_x, min_z, WORLD_SURFACE_WG), b = ctx.first_occupied_height(min_x, min_z + size_z, WORLD_SURFACE_WG);
    int c = ctx.first_occupied_height(min_x + size_x, min_z, WORLD_SURFACE_WG), d = ctx.first_occupied_height(min_x + size_x, min_z + size_z, WORLD_SURFACE_WG);
    return std::min(std::min(a, b), std::min(c, d));
}

int structure_lowest_y(const StructureContext& ctx, int size_x, int size_z) { return structure_lowest_y(ctx, ctx.chunk.x * 16, ctx.chunk.z * 16, size_x, size_z); }

BlockPos structure_lowest_y_in_5by5_box_offset_7(const StructureContext& ctx, Rotation rotation) {
    int offset_x = 5, offset_z = 5;
    if (rotation == Rotation::CLOCKWISE_90) offset_x = -5;
    else if (rotation == Rotation::CLOCKWISE_180) { offset_x = -5; offset_z = -5; }
    else if (rotation == Rotation::COUNTERCLOCKWISE_90) offset_z = -5;
    int block_x = ctx.chunk.x * 16 + 7, block_z = ctx.chunk.z * 16 + 7;
    return BlockPos(block_x, structure_lowest_y(ctx, block_x, block_z, offset_x, offset_z), block_z);
}

int structure_mean_first_occupied_height(const StructureContext& ctx, int min_x, int size_x, int min_z, int size_z) {
    int a = ctx.first_occupied_height(min_x, min_z, WORLD_SURFACE_WG), b = ctx.first_occupied_height(min_x, min_z + size_z, WORLD_SURFACE_WG);
    int c = ctx.first_occupied_height(min_x + size_x, min_z, WORLD_SURFACE_WG), d = ctx.first_occupied_height(min_x + size_x, min_z + size_z, WORLD_SURFACE_WG);
    return (a + b + c + d) / 4;
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
    jc::set_bd(env_.bd);
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
    jc::set_bd(env_.bd);
    Structure& s = *structures_[sid];
    WorldRandom random;
    random.set_large_feature_seed(env_.seed, cx, cz);
    StructureContext ctx{env_, random, {cx, cz}, [&s](int b) { return s.biomes.count(b) != 0; }};
    std::optional<GenerationStub> stub = s.find_generation_point(ctx);
    if (std::getenv("DBGST")) fprintf(stderr, "try %s at %d %d: stub %d\n", sid.c_str(), cx, cz, int(bool(stub)));
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


namespace {

// The order Mojang visits the starts of one structure in a chunk: its references are a fastutil LongOpenHashSet of start chunk positions
// (default capacity 16, load factor .75, linear probing), iterated from the last slot down, a zero key first.
std::vector<std::shared_ptr<const StructureStart>> reference_order(const std::vector<std::shared_ptr<const StructureStart>>& touching, const std::string& id) {
    std::vector<std::shared_ptr<const StructureStart>> mine;
    for (auto& st : touching) if (st->structure == id) mine.push_back(st);
    if (mine.size() < 2) return mine;
    auto key_of = [](const StructureStart& st) { return (uint64_t(uint32_t(st.pos.x))) | (uint64_t(uint32_t(st.pos.z)) << 32); };
    auto mix = [](uint64_t x) { uint64_t h = x * 0x9E3779B97F4A7C15ULL; h ^= h >> 32; return uint32_t(h ^ (h >> 16)); };
    size_t n = 32;
    std::vector<uint64_t> slots(n, 0);
    std::vector<std::shared_ptr<const StructureStart>> where(n);
    bool has_zero = false;
    std::shared_ptr<const StructureStart> zero_start;
    size_t size = 0;
    auto place = [&](std::vector<uint64_t>& sl, std::vector<std::shared_ptr<const StructureStart>>& wh, uint64_t k, const std::shared_ptr<const StructureStart>& st) {
        size_t mask = sl.size() - 1, pos = mix(k) & mask;
        while (sl[pos] != 0) pos = (pos + 1) & mask;
        sl[pos] = k;
        wh[pos] = st;
    };
    for (auto& st : mine) {
        uint64_t k = key_of(*st);
        if (k == 0) { if (!has_zero) { has_zero = true; zero_start = st; ++size; } continue; }
        size_t mask = n - 1, pos = mix(k) & mask;
        bool dup = false;
        while (slots[pos] != 0) { if (slots[pos] == k) { dup = true; break; } pos = (pos + 1) & mask; }
        if (dup) continue;
        slots[pos] = k;
        where[pos] = st;
        if (size++ >= n * 3 / 4) {   // maxFill reached: rehash into twice the table, visiting the old one from the end
            size_t nn = n * 2;
            std::vector<uint64_t> ns(nn, 0);
            std::vector<std::shared_ptr<const StructureStart>> nw(nn);
            for (size_t i = n; i-- > 0;) if (slots[i] != 0) place(ns, nw, slots[i], where[i]);
            slots.swap(ns);
            where.swap(nw);
            n = nn;
        }
    }
    std::vector<std::shared_ptr<const StructureStart>> out;
    if (has_zero) out.push_back(zero_start);
    for (size_t i = n; i-- > 0;) if (slots[i] != 0) out.push_back(where[i]);
    return out;
}

}  // namespace

void StructureState::place_step(int step, const std::vector<std::shared_ptr<const StructureStart>>& touching, FeatureEnv& fenv, Rng& random, int64_t decoration_seed, int cx, int cz) {
    if (step < 0 || size_t(step) >= all_by_step_.size() || touching.empty()) return;
    jc::set_bd(env_.bd);
    const BoundingBox chunk_bb(cx * 16, env_.min_y + 1, cz * 16, cx * 16 + 15, env_.min_y + env_.height - 1, cz * 16 + 15);
    PieceCtx ctx{fenv, fenv.level, env_, *templates_, random, chunk_bb, {cx, cz}};   // chunk_bb is renewed for every start: a piece may widen it
    const auto& ids = all_by_step_[size_t(step)];
    for (size_t index = 0; index < ids.size(); index++) {
        bool any = false;
        for (auto& st : touching) if (st->structure == ids[index]) { any = true; break; }
        if (!any) continue;
        random.set_seed(decoration_seed + int64_t(index) + int64_t(10000) * int64_t(step));
        for (auto& st : reference_order(touching, ids[index])) {
            if (st->pieces.empty()) continue;
            ctx.chunk_bb = chunk_bb;
            const BoundingBox center_bb = st->pieces[0]->bb;
            BlockPos center = center_bb.center();
            BlockPos reference(center.x, center_bb.y0, center.z);
            for (auto& piece : st->pieces)
                if (piece->bb.intersects(chunk_bb)) piece->post_process(ctx, reference);
            auto sit = structures_.find(st->structure);
            if (sit != structures_.end()) sit->second->after_place(ctx, *st);
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

namespace pn::vg {

std::vector<std::string> StructureState::structure_ids() const {
    std::vector<std::string> out;
    for (auto& kv : structures_) out.push_back(kv.first);
    return out;
}

bool StructureState::locate(const std::set<std::string>& ids, int bx, int bz, int max_radius, int& out_x, int& out_z) {
    auto has_wanted = [&](int cx, int cz) {
        for (auto& st : starts_at(cx, cz)) if (ids.count(st->structure)) return true;
        return false;
    };
    // the placements of the sets that hold a wanted structure
    std::vector<const StructureSetData*> sets;
    for (auto& set : sets_) {
        bool any = false;
        for (auto& e : set.structures) if (ids.count(e.structure)) any = true;
        if (any) sets.push_back(&set);
    }
    if (sets.empty()) return false;
    bool found = false;
    double best = 1e300;
    auto dist2 = [&](int x, int z) { double dx = double(x - bx), dz = double(z - bz); return dx * dx + dz * dz; };
    auto locate_pos = [](const StructurePlacement& p, int cx, int cz, int& x, int& z) { x = cx * 16 + p.locate_x; z = cz * 16 + p.locate_z; };
    // concentric rings: every ring position that could be closer and holds a wanted start
    for (auto* set : sets) {
        const StructurePlacement& p = set->placement;
        if (p.type != StructurePlacement::CONCENTRIC_RINGS) continue;
        double closest = 1e300;
        bool have = false;
        int rx = 0, rz = 0;
        for (auto& c : p.rings) {
            double d = dist2(c.x * 16 + 8, c.z * 16 + 8);
            bool is_closest = !have || d < closest;
            if (!is_closest || !has_wanted(c.x, c.z)) continue;
            locate_pos(p, c.x, c.z, rx, rz);
            closest = d;
            have = true;
        }
        if (have) {
            double nd = dist2(rx, rz);
            if (nd < best) { best = nd; out_x = rx; out_z = rz; found = true; }
        }
    }
    // random spread: ring by ring of grid cells, nearest ring first
    std::vector<const StructureSetData*> spread;
    for (auto* set : sets) if (set->placement.type == StructurePlacement::RANDOM_SPREAD) spread.push_back(set);
    if (!spread.empty()) {
        int origin_x = bx >> 4, origin_z = bz >> 4;
        for (int radius = 0; radius <= max_radius; ++radius) {
            bool found_something = false;
            for (auto* set : spread) {
                const StructurePlacement& p = set->placement;
                int spacing = p.spacing;
                bool hit = false;
                int hx = 0, hz = 0;
                for (int x = -radius; x <= radius && !hit; ++x) {
                    bool x_edge = x == -radius || x == radius;
                    for (int z = -radius; z <= radius && !hit; ++z) {
                        bool z_edge = z == -radius || z == radius;
                        if (!x_edge && !z_edge) continue;
                        ChunkPos target = potential_chunk(p, env_.seed, origin_x + spacing * x, origin_z + spacing * z);
                        if (!is_structure_chunk(*set, target.x, target.z)) continue;
                        bool wanted = false;
                        for (auto& st : starts_at(target.x, target.z)) if (ids.count(st->structure)) wanted = true;
                        if (!wanted) continue;
                        locate_pos(p, target.x, target.z, hx, hz);
                        hit = true;
                    }
                }
                if (!hit) continue;
                found_something = true;
                double nd = dist2(hx, hz);
                if (nd < best) { best = nd; out_x = hx; out_z = hz; found = true; }
            }
            if (found_something) return found;
        }
    }
    return found;
}

}  // namespace pn::vg
