#include "feature.hpp"
#include "terrain3.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace pn::vg {

extern thread_local BlockPos g_trace_origin;

namespace {

std::string short_type(const Json& j) {
    const std::string& t = j.at("type").str;
    return t.rfind("minecraft:", 0) == 0 ? t.substr(10) : t;
}

BlockPos json_offset(const Json& j) {
    const Json* o = j.find("offset");
    if (!o || !o->is_arr() || o->arr.size() < 3) return {};
    return {int(o->arr[0].int_value()), int(o->arr[1].int_value()), int(o->arr[2].int_value())};
}

int dir_from_name(const std::string& n) {
    if (n == "down") return DOWN;
    if (n == "up") return UP;
    if (n == "north") return NORTH;
    if (n == "south") return SOUTH;
    if (n == "west") return WEST;
    return EAST;
}

}  // namespace

int heightmap_from_name(const std::string& name) {
    if (name == "WORLD_SURFACE_WG") return WORLD_SURFACE_WG;
    if (name == "WORLD_SURFACE") return WORLD_SURFACE;
    if (name == "OCEAN_FLOOR_WG") return OCEAN_FLOOR_WG;
    if (name == "OCEAN_FLOOR") return OCEAN_FLOOR;
    if (name == "MOTION_BLOCKING") return MOTION_BLOCKING;
    if (name == "MOTION_BLOCKING_NO_LEAVES") return MOTION_BLOCKING_NO_LEAVES;
    throw std::runtime_error("unknown heightmap " + name);
}

const NormalNoise& FeatureShared::noise_of(const Json* key, const Json& provider, bool slow) {
    std::lock_guard<std::mutex> l(mu);
    auto& table = slow ? slow_noises : noises;
    auto it = table.find(key);
    if (it != table.end()) return *it->second;
    const Json& params = provider.at(slow ? "slow_noise" : "noise");
    NoiseParameters np;
    np.first_octave = int(params.at("firstOctave").int_value());
    for (auto& a : params.at("amplitudes").arr) np.amplitudes.push_back(a.number());
    LegacyRandom r(provider.at("seed").int_value());
    return *(table[key] = std::make_unique<NormalNoise>(r, np));
}

const v3::NoiseStack& FeatureShared::noise3_of(const Json* key, const Json& provider, bool slow) {
    std::lock_guard<std::mutex> l(mu);
    auto& table = slow ? slow_noises3 : noises3;
    auto it = table.find(key);
    if (it != table.end()) return *it->second;
    const Json& j = provider.at(slow ? "slow_noise" : "noise");
    v3::NoiseParams np;
    np.base_amplitude = j.has("base_amplitude") ? j.at("base_amplitude").number() : 1.0;
    np.base_octave = int(j.at("base_octave").int_value());
    np.octave_count = int(j.get_int("octave_count", 1));
    if (const Json* n = j.find("normalize")) np.normalize = n->is_bool() ? (n->b ? v3::NoiseParams::ENABLED : v3::NoiseParams::DISABLED) : v3::NoiseParams::LEGACY;
    if (const Json* a = j.find("amplitude_modifiers")) for (auto& e : a->arr) np.amplitude_modifiers.push_back(e.number());
    LegacyRandom r(provider.at("seed").int_value());
    return *(table[key] = v3::create_normal_noise(np, r));
}

const Json& resolve_ref(FeatureEnv& env, const char* kind, const Json& ref) {
    if (ref.is_str()) return env.reg.get(kind, ref.str);
    return ref;
}

bool state_in_set(FeatureEnv& env, uint16_t state, const Json& blocks) {
    const BlockData& bd = env.bd();
    auto one = [&](const std::string& e) {
        if (!e.empty() && e[0] == '#') return bd.in_tag(state, e);
        return bd.name(state) == e;
    };
    if (blocks.is_arr()) { for (auto& e : blocks.arr) if (one(e.str)) return true; return false; }
    return one(blocks.str);
}

// ---- block predicates

bool test_predicate(FeatureEnv& env, const Json& p, const BlockPos& origin) {
    const std::string t = short_type(p);
    const Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (t == "true") return true;
    if (t == "not") return !test_predicate(env, p.at("predicate"), origin);
    if (t == "all_of") { for (auto& e : p.at("predicates").arr) if (!test_predicate(env, e, origin)) return false; return true; }
    if (t == "any_of") { for (auto& e : p.at("predicates").arr) if (test_predicate(env, e, origin)) return true; return false; }
    if (t == "volume_match") {   // 26.3: every block of the box around the origin matches
        const Json &mn = p.at("min"), &mx = p.at("max");
        for (int ox = int(mn.arr[0].int_value()); ox <= int(mx.arr[0].int_value()); ox++)
            for (int oz = int(mn.arr[2].int_value()); oz <= int(mx.arr[2].int_value()); oz++)
                for (int oy = int(mn.arr[1].int_value()); oy <= int(mx.arr[1].int_value()); oy++)
                    if (!test_predicate(env, p.at("match"), origin.offset(ox, oy, oz))) return false;
        return true;
    }
    if (t == "height_range") {
        int lo = resolve_anchor(p.at("min_inclusive"), lv.range()), hi = resolve_anchor(p.at("max_inclusive"), lv.range());
        return origin.y >= lo && origin.y <= hi;
    }
    BlockPos at = origin;
    {
        BlockPos o = json_offset(p);
        at = origin.offset(o.x, o.y, o.z);
    }
    if (t == "matching_blocks") return state_in_set(env, lv.get(at), p.at("blocks"));
    if (t == "matching_block_tag") return bd.in_tag(lv.get(at), p.at("tag").str);
    if (t == "matching_fluids") {
        uint16_t s = lv.get(at);
        const Json& f = p.at("fluids");
        auto one = [&](const std::string& e) {
            if (e == "#minecraft:water") return bd.water(s);
            if (e == "#minecraft:lava") return bd.lava(s);
            if (e == "minecraft:water") return bd.water(s) && (bd.flags(s) & BF_SOURCE);
            if (e == "minecraft:lava") return bd.lava(s) && (bd.flags(s) & BF_SOURCE);
            if (e == "minecraft:empty") return !bd.fluid(s);
            if (e == "minecraft:flowing_water") return bd.water(s) && !(bd.flags(s) & BF_SOURCE);
            if (e == "minecraft:flowing_lava") return bd.lava(s) && !(bd.flags(s) & BF_SOURCE);
            return false;
        };
        if (f.is_arr()) { for (auto& e : f.arr) if (one(e.str)) return true; return false; }
        return one(f.str);
    }
    if (t == "solid") return bd.flags(lv.get(at)) & BF_SOLID;
    if (t == "replaceable") return bd.replaceable(lv.get(at));
    if (t == "has_sturdy_face") return bd.sturdy(lv.get(at), dir_from_name(p.at("direction").str));
    if (t == "inside_world_bounds") return !lv.outside_build_height(at.y);
    if (t == "would_survive") {
        bool r = state_can_survive(env, bd.state(p.at("state")), at);
        if (std::getenv("PAINITE_DBG2") && at.x >= 184 && at.x <= 190 && at.z >= -82 && at.z <= -76) fprintf(stderr, "would_survive %d %d %d -> %d below=%s\n", at.x, at.y, at.z, int(r), bd.name(lv.get(at.below())).c_str());
        return r;
    }
    if (t == "unobstructed") return true;   // entities do not exist during generation
    if (t == "matching_biomes") {
        int b = lv.biome(at);
        (void)b;
        throw std::runtime_error("matching_biomes predicate is not used by the data");
    }
    throw std::runtime_error("unknown block predicate " + t);
}

// ---- rule tests (ore targets)

bool rule_test(FeatureEnv& env, const Json& t, uint16_t state, Rng& random, const BlockPos& pos) {
    const std::string type = t.at("predicate_type").str.substr(10);
    const BlockData& bd = env.bd();
    if (type == "always_true") return true;
    if (type == "all_of") { for (auto& r : t.at("rules").arr) if (!rule_test(env, r, state, random, pos)) return false; return true; }
    if (type == "any_of") { for (auto& r : t.at("rules").arr) if (rule_test(env, r, state, random, pos)) return true; return false; }
    if (type == "not") return !rule_test(env, t.at("rule"), state, random, pos);
    if (type == "height_match") return t.at("min_inclusive").int_value() <= pos.y && pos.y <= t.at("max_inclusive").int_value();
    if (type == "tag_match") return bd.in_tag(state, t.at("tag").str);
    if (type == "block_match") return bd.name(state) == t.at("block").str;
    if (type == "blockstate_match") return state == bd.state(t.at("block_state"));
    if (type == "random_block_match") return bd.name(state) == t.at("block").str && random.next_float() < t.at("probability").float_value();
    if (type == "random_blockstate_match") return state == bd.state(t.at("block_state")) && random.next_float() < t.at("probability").float_value();
    throw std::runtime_error("unknown rule test " + type);
}

// ---- block state providers

namespace {

uint16_t noise_pick(const std::vector<uint16_t>& states, double noise_value) {
    double placement = std::min(std::max((1.0 + noise_value) / 2.0, 0.0), 0.9999);
    return states[size_t(int(placement * double(states.size())))];
}

}  // namespace

uint16_t provide_state(FeatureEnv& env, const Json& p, Rng& random, const BlockPos& pos) {
    const std::string t = short_type(p);
    const BlockData& bd = env.bd();
    if (t == "simple_state_provider") return bd.state(p.at("state"));
    if (t == "weighted_state_provider") return bd.state(pick_weighted(p.at("entries"), random));
    if (t == "rule_based_state_provider") {
        for (auto& rule : p.at("rules").arr)
            if (test_predicate(env, rule.at("if_true"), pos)) return provide_state(env, rule.at("then"), random, pos);
        if (const Json* fb = p.find("fallback")) return provide_state(env, *fb, random, pos);
        return kNoState;
    }
    if (t == "randomized_int_state_provider") {
        uint16_t s = provide_state(env, p.at("source"), random, pos);
        const std::string& prop = p.at("property").str;
        if (!bd.has_prop(s, prop)) return s;
        return bd.with(s, prop, std::to_string(sample_int(p.at("values"), random)));
    }
    if (t == "random_block") {   // RandomBlockProvider: a random member of the block set, default state
        const Json& blocks = p.at("blocks");
        std::vector<std::string> members;
        auto add = [&](const std::string& e) {
            if (!e.empty() && e[0] == '#') { auto m = env.reg.tag_ordered("block", e); members.insert(members.end(), m.begin(), m.end()); }
            else members.push_back(e);
        };
        if (blocks.is_arr()) for (auto& e : blocks.arr) add(e.str); else add(blocks.str);
        if (members.empty()) return kNoState;
        return bd.default_state(members[size_t(random.next_int(int(members.size())))]);
    }
    if (t == "rotated_block_provider") {
        static const char* axes[3] = {"x", "y", "z"};
        int axis = random.next_int(3);
        return bd.with(bd.state(p.at("state")), "axis", axes[axis]);
    }
    if (env.v263 && (t == "noise_provider" || t == "noise_threshold_provider" || t == "dual_noise_provider")) {
        const v3::NoiseStack& noise = env.shared.noise3_of(&p, p);
        double scale = double(p.at("scale").float_value());
        auto value = [&](const v3::NoiseStack& n, int x, int y, int z, double sc) { return n.get(double(x) * sc, double(y) * sc, double(z) * sc); };
        auto pick_state = [&](const std::vector<uint16_t>& states, float noise_value) {
            float placement = v3::mth_clamp((1.0f + noise_value) / 2.0f, 0.0f, 0.9999f);
            return states[size_t(int(placement * float(states.size())))];
        };
        if (t == "noise_threshold_provider") {
            double local = double(value(noise, pos.x, pos.y, pos.z, scale));
            auto pick = [&](const Json& list) { return bd.state(list.arr[size_t(random.next_int(int(list.arr.size())))]); };
            if (local < double(p.at("threshold").float_value())) return pick(p.at("low_states"));
            float c = random.next_float();
            return c < p.at("high_chance").float_value() ? pick(p.at("high_states")) : bd.state(p.at("default_state"));
        }
        std::vector<uint16_t> states;
        for (auto& s : p.at("states").arr) states.push_back(bd.state(s));
        if (t == "noise_provider") return pick_state(states, value(noise, pos.x, pos.y, pos.z, scale));
        const v3::NoiseStack& slow = env.shared.noise3_of(&p, p, true);
        float slow_scale = p.at("slow_scale").float_value();
        auto slow_value = [&](int x, int y, int z) { return slow.get(double(float(x) * slow_scale), double(float(y) * slow_scale), double(float(z) * slow_scale)); };
        const Json& var = p.at("variety");
        double variety_noise = double(slow_value(pos.x, pos.y, pos.z));
        int lo = int((var.is_arr() ? var.arr.at(0) : var.at("min_inclusive")).int_value()), hi = int((var.is_arr() ? var.arr.at(1) : var.at("max_inclusive")).int_value());   // 26.2 writes {min_inclusive, max_inclusive}, 26.3 [min, max]
        int local_variety = int(clamped_map(variety_noise, -1.0, 1.0, double(lo), double(hi + 1)));
        std::vector<uint16_t> possible;
        for (int i = 0; i < local_variety; i++) possible.push_back(pick_state(states, slow_value(pos.x + i * 54545, pos.y, pos.z + i * 34234)));
        return pick_state(possible, value(noise, pos.x, pos.y, pos.z, scale));
    }
    if (t == "noise_provider" || t == "noise_threshold_provider" || t == "dual_noise_provider") {
        const NormalNoise& noise = env.shared.noise_of(&p, p);
        double scale = double(p.at("scale").float_value());
        auto value = [&](const NormalNoise& n, int x, int y, int z, double sc) { return n.get_value(x * sc, y * sc, z * sc); };
        if (t == "noise_threshold_provider") {
            double local = value(noise, pos.x, pos.y, pos.z, scale);
            auto pick = [&](const Json& list) { return bd.state(list.arr[size_t(random.next_int(int(list.arr.size())))]); };
            if (local < double(p.at("threshold").float_value())) return pick(p.at("low_states"));
            float c = random.next_float();
            return c < p.at("high_chance").float_value() ? pick(p.at("high_states")) : bd.state(p.at("default_state"));
        }
        std::vector<uint16_t> states;
        for (auto& s : p.at("states").arr) states.push_back(bd.state(s));
        if (t == "noise_provider") return noise_pick(states, value(noise, pos.x, pos.y, pos.z, scale));
        // dual noise
        const NormalNoise& slow = env.shared.noise_of(&p, p, true);
        double slow_scale = double(p.at("slow_scale").float_value());
        const Json& var = p.at("variety");
        double variety_noise = value(slow, pos.x, pos.y, pos.z, slow_scale);
        int lo = int((var.is_arr() ? var.arr.at(0) : var.at("min_inclusive")).int_value()), hi = int((var.is_arr() ? var.arr.at(1) : var.at("max_inclusive")).int_value());   // 26.2 writes {min_inclusive, max_inclusive}, 26.3 [min, max]
        int local_variety = int(clamped_map(variety_noise, -1.0, 1.0, double(lo), double(hi + 1)));
        std::vector<uint16_t> possible;
        for (int i = 0; i < local_variety; i++) possible.push_back(noise_pick(states, value(slow, pos.x + i * 54545, pos.y, pos.z + i * 34234, slow_scale)));
        return noise_pick(possible, value(noise, pos.x, pos.y, pos.z, scale));
    }
    throw std::runtime_error("unknown block state provider " + t);
}

// ---- placement modifiers

namespace {

struct PlaceRun {
    FeatureEnv& env;
    const Json& modifiers;
    const Json& configured;
    Rng& random;
    bool placed = false;

    void run(size_t i, const BlockPos& pos) {
        if (i == modifiers.arr.size()) {
            if (place_configured_feature(env, configured, random, pos)) placed = true;
            return;
        }
        apply(modifiers.arr[i], i, pos);
    }

    // one placement modifier, then the rest of the chain from i + 1
    void apply(const Json& m, size_t i, const BlockPos& pos) {
        const std::string t = short_type(m);
        Level& lv = env.level;
        if (std::getenv("PAINITE_DBG2") && env.top_feature && pos.x >= 184 && pos.x <= 190 && pos.z >= -82 && pos.z <= -76) fprintf(stderr, "%s modifier %zu %s at %d %d %d\n", env.top_feature->c_str(), i, t.c_str(), pos.x, pos.y, pos.z);
        if (t == "count") {
            int n = sample_int(m.at("count"), random);
            for (int k = 0; k < n; k++) {
                run(i + 1, pos);
            }
        } else if (t == "in_square") {
            int x = random.next_int(16) + pos.x;
            int z = random.next_int(16) + pos.z;
            run(i + 1, BlockPos(x, pos.y, z));
        } else if (t == "height_range") {
            run(i + 1, BlockPos(pos.x, sample_height(m.at("height"), random, lv.range()), pos.z));
        } else if (t == "heightmap") {
            int h = lv.height(HeightmapType(heightmap_from_name(m.at("heightmap").str)), pos.x, pos.z);
            if (h > lv.min_y()) run(i + 1, BlockPos(pos.x, h, pos.z));
        } else if (t == "biome") {
            int b = lv.biome(pos);
            bool ok = env.biome_has_feature(b, *env.top_feature);
            if (const char* tr = std::getenv("PAINITE_TRACE")) if (env.top_feature->find(tr) != std::string::npos) fprintf(stderr, "trace-biome %s at %d %d %d biome %d %s\n", env.top_feature->c_str(), pos.x, pos.y, pos.z, b, ok ? "ok" : "REJECT");
            if (ok) run(i + 1, pos);
        } else if (t == "rarity_filter") {
            if (random.next_float() < 1.0f / float(m.at("chance").int_value())) run(i + 1, pos);
        } else if (t == "block_predicate_filter") {
            if (test_predicate(env, m.at("predicate"), pos)) run(i + 1, pos);
        } else if (t == "offset") {
            int sx = sample_int(m.at("x"), random);
            int sy = sample_int(m.at("y"), random);
            int sz = sample_int(m.at("z"), random);
            run(i + 1, BlockPos(pos.x + sx, pos.y + sy, pos.z + sz));
        } else if (t == "randomly_selected") {   // RandomlySelectedPlacement: one modifier of the list stands in for this one
            const Json& list = m.at("placements");
            apply(list.arr[size_t(random.next_int(int(list.arr.size())))], i, pos);
        } else if (t == "cuboid") {
            int height = sample_int(m.at("y_size"), random);
            int width = sample_int(m.at("xz_size"), random);
            int length = sample_int(m.at("xz_size"), random);
            bool edges = m.get_bool("include_edges", true), interior = m.get_bool("include_interior", true);
            std::vector<BlockPos> out;
            for (int x = 0; x <= width; x++)
                for (int y = 0; y <= height; y++)
                    for (int z = 0; z <= length; z++) {
                        if (!((edges || (x != 0 && x != width) || (y != 0 && y != height)) && (edges || (z != 0 && z != length) || (y != 0 && y != height)) &&
                              (edges || (x != 0 && x != width) || (z != 0 && z != length)) && (interior || x == 0 || x == width || y == 0 || y == height || z == 0 || z == length))) continue;
                        out.push_back(BlockPos(x + pos.x, y + pos.y, z + pos.z));
                    }
            for (auto& q : out) run(i + 1, q);
        } else if (t == "random_chance") {
            if (random.next_float() < m.at("chance").float_value()) run(i + 1, pos);
        } else if (t == "random_offset") {
            int sx = sample_int(m.at("xz_spread"), random);
            int sy = sample_int(m.at("y_spread"), random);
            int sz = sample_int(m.at("xz_spread"), random);
            run(i + 1, BlockPos(pos.x + sx, pos.y + sy, pos.z + sz));
        } else if (t == "surface_water_depth_filter") {
            int floor = lv.height(OCEAN_FLOOR, pos.x, pos.z), surf = lv.height(WORLD_SURFACE, pos.x, pos.z);
            if (surf - floor <= int(m.at("max_water_depth").int_value())) run(i + 1, pos);
        } else if (t == "surface_relative_threshold_filter") {
            int64_t surface = lv.height(HeightmapType(heightmap_from_name(m.at("heightmap").str)), pos.x, pos.z);
            int64_t lo = surface + m.get_int("min_inclusive", INT32_MIN), hi = surface + m.get_int("max_inclusive", INT32_MAX);
            if (lo <= pos.y && pos.y <= hi) run(i + 1, pos);
        } else if (t == "noise_based_count") {
            double v = env.v263 ? double(v3::biome_info_noise3(double(pos.x) / m.at("noise_factor").number(), double(pos.z) / m.at("noise_factor").number()))
                                : biome_info_noise().get_value(double(pos.x) / m.at("noise_factor").number(), double(pos.z) / m.at("noise_factor").number(), false);
            int n = int(std::ceil((v + m.get_number("noise_offset", 0.0)) * double(m.at("noise_to_count_ratio").int_value())));
            for (int k = 0; k < n; k++) run(i + 1, pos);
        } else if (t == "noise_threshold_count") {
            double v = env.v263 ? double(v3::biome_info_noise3(double(pos.x) / 200.0, double(pos.z) / 200.0)) : biome_info_noise().get_value(double(pos.x) / 200.0, double(pos.z) / 200.0, false);
            int n = v < m.at("noise_level").number() ? int(m.at("below_noise").int_value()) : int(m.at("above_noise").int_value());
            for (int k = 0; k < n; k++) run(i + 1, pos);
        } else if (t == "count_on_every_layer") {
            std::vector<BlockPos> list;
            int layer = 0;
            bool found;
            do {
                found = false;
                for (int k = 0; k < sample_int(m.at("count"), random); k++) {
                    int x = random.next_int(16) + pos.x;
                    int z = random.next_int(16) + pos.z;
                    int start_y = lv.height(MOTION_BLOCKING, x, z);
                    int y = on_ground_y(x, start_y, z, layer);
                    if (y != INT32_MAX) { list.emplace_back(x, y, z); found = true; }
                }
                layer++;
            } while (found);
            for (auto& p : list) run(i + 1, p);
        } else if (t == "environment_scan") {
            bool down = m.at("direction_of_search").str == "down";
            int steps = int(m.at("max_steps").int_value());
            const Json* allowed = m.find("allowed_search_condition");
            const Json& target = m.at("target_condition");
            BlockPos p = pos;
            auto allowed_at = [&](const BlockPos& q) { return !allowed || test_predicate(env, *allowed, q); };
            if (!allowed_at(p)) return;
            for (int s = 0; s < steps; s++) {
                if (test_predicate(env, target, p)) { run(i + 1, p); return; }
                p = p.offset(0, down ? -1 : 1, 0);
                if (lv.outside_build_height(p.y)) return;
                if (!allowed_at(p)) break;
            }
            if (test_predicate(env, target, p)) run(i + 1, p);
        } else if (t == "fixed_placement") {
            int cx = pos.x >> 4, cz = pos.z >> 4;
            for (auto& q : m.at("positions").arr) {
                BlockPos fp(int(q.arr[0].int_value()), int(q.arr[1].int_value()), int(q.arr[2].int_value()));
                if ((fp.x >> 4) == cx && (fp.z >> 4) == cz) run(i + 1, fp);
            }
        } else {
            throw std::runtime_error("unknown placement modifier " + t);
        }
    }

    bool empty(uint16_t s) const {
        const BlockData& bd = env.bd();
        return bd.air(s) || bd.name(s) == "minecraft:water" || bd.name(s) == "minecraft:lava";
    }
    int on_ground_y(int x, int y_start, int z, int layer_to_place_on) {
        Level& lv = env.level;
        int current_layer = 0;
        uint16_t current = lv.get(x, y_start, z);
        for (int y = y_start; y >= lv.min_y() + 1; y--) {
            uint16_t below = lv.get(x, y - 1, z);
            if (!empty(below) && empty(current) && env.bd().name(below) != "minecraft:bedrock") {
                if (current_layer == layer_to_place_on) return y;
                current_layer++;
            }
            current = below;
        }
        return INT32_MAX;
    }
};

}  // namespace

thread_local BlockPos g_trace_origin;
bool place_placed_feature(FeatureEnv& env, const Json& placed, Rng& random, const BlockPos& origin, const std::string* top_feature_id) {
    if (top_feature_id) g_trace_origin = origin;
    const std::string* saved = env.top_feature;
    env.top_feature = top_feature_id;
    PlaceRun run{env, placed.at("placement"), resolve_ref(env, "configured_feature", placed.at("feature")), random};
    try {
        run.run(0, origin);
    } catch (std::exception& e) {
        if (top_feature_id) throw std::runtime_error(std::string(e.what()) + " (in feature " + *top_feature_id + ")");
        throw;
    }
    env.top_feature = saved;
    return run.placed;
}

bool place_configured_feature(FeatureEnv& env, const Json& cf, Rng& random, const BlockPos& origin) {
    if (!env.level.can_write(origin)) return false;   // Feature.place: ensureCanWrite(origin)
    std::string type = cf.at("type").str;
    if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
    if (const char* tr = std::getenv("PAINITE_TRACE")) if (env.top_feature && env.top_feature->find(tr) != std::string::npos) fprintf(stderr, "trace %s at %d %d %d (chunk %d %d)\n", env.top_feature->c_str(), origin.x, origin.y, origin.z, g_trace_origin.x >> 4, g_trace_origin.z >> 4);
    return place_feature_type(env, type, cf.at("config"), random, origin);
}

}  // namespace pn::vg
