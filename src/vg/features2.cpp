// More feature types: springs, kelp and seagrass, columns, piles, bamboo, vines, vegetation patches and the top-layer snow and ice.
#include "feature.hpp"
#include "jhash.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pn::vg {

namespace {

uint16_t legacy_fluid_block(FeatureEnv& env, const Json& fluid_state) {
    const BlockData& bd = env.bd();
    std::string name = fluid_state.at("Name").str;
    bool falling = false;
    int level = 0;
    if (const Json* pr = fluid_state.find("Properties")) {
        if (const Json* f = pr->find("falling")) falling = f->str == "true";
        if (const Json* l = pr->find("level")) level = std::stoi(l->str);
    }
    int legacy = 8 - std::min(level > 0 ? level : 8, 8) + (falling ? 8 : 0);
    if (const Json* pr = fluid_state.find("Properties")) if (pr->find("level")) legacy = 8 - std::min(level, 8) + (falling ? 8 : 0);
    return bd.with(bd.default_state(name), "level", std::to_string(legacy));
}

bool place_spring(FeatureEnv& env, const Json& config, Rng&, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    const Json& valid = config.at("valid_blocks");
    auto is_valid = [&](const BlockPos& p) { return state_in_set(env, lv.get(p), valid); };
    bool requires_below = config.get_bool("requires_block_below", true);
    int rock_count = int(config.get_int("rock_count", 4)), hole_count = int(config.get_int("hole_count", 1));
    if (!is_valid(origin.above())) return false;
    if (requires_below && !is_valid(origin.below())) return false;
    uint16_t cur = lv.get(origin);
    if (!bd.air(cur) && !state_in_set(env, cur, valid)) return false;
    int rocks = 0, holes = 0;
    for (Dir d : {WEST, EAST, NORTH, SOUTH, DOWN}) if (is_valid(origin + d)) rocks++;
    for (Dir d : {WEST, EAST, NORTH, SOUTH, DOWN}) if (bd.air(lv.get(origin + d))) holes++;
    if (rocks == rock_count && holes == hole_count) {
        lv.set(origin, legacy_fluid_block(env, config.at("state")));
        return true;
    }
    return false;
}

bool is_water_block(const BlockData& bd, uint16_t s) { return bd.name(s) == "minecraft:water"; }

bool place_kelp(FeatureEnv& env, const Json&, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    int placed = 0;
    int y = lv.height(OCEAN_FLOOR, origin.x, origin.z);
    BlockPos kelp(origin.x, y, origin.z);
    if (!is_water_block(bd, lv.get(kelp))) return false;
    uint16_t top = bd.default_state("minecraft:kelp"), body = bd.default_state("minecraft:kelp_plant");
    int height = 1 + random.next_int(10);
    for (int h = 0; h <= height; h++) {
        if (is_water_block(bd, lv.get(kelp)) && is_water_block(bd, lv.get(kelp.above())) && state_can_survive(env, body, kelp)) {
            if (h == height) {
                int age = random.next_int(4) + 20;
                lv.set(kelp, bd.with(top, "age", std::to_string(age)));
                placed++;
            } else lv.set(kelp, body);
        } else if (h > 0) {
            BlockPos below = kelp.below();
            if (state_can_survive(env, top, below) && bd.name(lv.get(below.below())) != "minecraft:kelp") {
                int age = random.next_int(4) + 20;
                lv.set(below, bd.with(top, "age", std::to_string(age)));
                placed++;
            }
            break;
        }
        kelp = kelp.above();
    }
    return placed > 0;
}

bool place_seagrass(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    int a = random.next_int(8); int b = random.next_int(8);
    int x = a - b;
    int c = random.next_int(8); int d = random.next_int(8);
    int z = c - d;
    int y = lv.height(OCEAN_FLOOR, origin.x + x, origin.z + z);
    BlockPos pos(origin.x + x, y, origin.z + z);
    if (!is_water_block(bd, lv.get(pos))) return false;
    bool tall = random.next_double() < config.at("probability").number();
    uint16_t state = bd.default_state(tall ? "minecraft:tall_seagrass" : "minecraft:seagrass");
    if (!state_can_survive(env, state, pos)) return false;
    if (tall) {
        BlockPos above = pos.above();
        if (is_water_block(bd, lv.get(above))) {
            lv.set(pos, bd.with(state, "half", "lower"));
            lv.set(above, bd.with(state, "half", "upper"));
        }
    } else lv.set(pos, state);
    return true;
}

bool place_sea_pickle(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    int placed = 0;
    int count = sample_int(config.at("count"), random);
    for (int i = 0; i < count; i++) {
        int a = random.next_int(8); int b = random.next_int(8);
        int x = a - b;
        int c = random.next_int(8); int d = random.next_int(8);
        int z = c - d;
        int y = lv.height(OCEAN_FLOOR, origin.x + x, origin.z + z);
        BlockPos pos(origin.x + x, y, origin.z + z);
        uint16_t pickle = bd.with(bd.default_state("minecraft:sea_pickle"), "pickles", std::to_string(random.next_int(4) + 1));
        if (is_water_block(bd, lv.get(pos)) && state_can_survive(env, pickle, pos)) { lv.set(pos, pickle); placed++; }
    }
    return placed > 0;
}

bool place_block_column(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const Json& layers = config.at("layers");
    int n = int(layers.arr.size());
    std::vector<int> heights(size_t(n), 0);
    int total = 0;
    for (int i = 0; i < n; i++) { heights[size_t(i)] = sample_int(layers.arr[size_t(i)].at("height"), random); total += heights[size_t(i)]; }
    if (total == 0) return false;
    static const char* names[6] = {"down", "up", "north", "south", "west", "east"};
    int dir = UP;
    for (int k = 0; k < 6; k++) if (config.at("direction").str == names[k]) dir = k;
    BlockPos place = origin, next = place + Dir(dir);
    for (int y = 0; y < total; y++) {
        if (!test_predicate(env, config.at("allowed_placement"), next)) {
            // truncate
            int to_remove = total - y;
            bool tip = config.at("prioritize_tip").boolean();
            int step = tip ? 1 : -1, start = tip ? 0 : n - 1, end = tip ? n : -1;
            for (int i = start; i != end && to_remove > 0; i += step) {
                int take = std::min(heights[size_t(i)], to_remove);
                to_remove -= take;
                heights[size_t(i)] -= take;
            }
            break;
        }
        next = next + Dir(dir);
    }
    for (int i = 0; i < n; i++) {
        int count = heights[size_t(i)];
        if (!count) continue;
        for (int y = 0; y < count; y++) {
            uint16_t s = provide_state(env, layers.arr[size_t(i)].at("provider"), random, place);
            lv.set(place, s);
            place = place + Dir(dir);
        }
    }
    return true;
}

bool place_block_pile(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (origin.y < lv.min_y() + 5) return false;
    int xr = 2 + random.next_int(2);
    int zr = 2 + random.next_int(2);
    auto try_place = [&](const BlockPos& p) {
        if (!bd.air(lv.get(p))) return;
        BlockPos below = p.below();
        uint16_t bs = lv.get(below);
        bool may = bd.name(bs) == "minecraft:dirt_path" ? random.next_boolean() : bd.sturdy(bs, UP);
        if (may) lv.set(p, provide_state(env, config.at("state_provider"), random, p));
    };
    for (int z = origin.z - zr; z <= origin.z + zr; z++)
        for (int y = origin.y; y <= origin.y + 1; y++)
            for (int x = origin.x - xr; x <= origin.x + xr; x++) {
                BlockPos p(x, y, z);
                int xd = origin.x - x, zd = origin.z - z;
                float a = random.next_float();
                float b = random.next_float();
                if (float(xd * xd + zd * zd) <= a * 10.0f - b * 6.0f) try_place(p);
                else if (double(random.next_float()) < 0.031) try_place(p);
            }
    return true;
}

bool place_bamboo(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    int placed = 0;
    BlockPos bamboo = origin;
    if (bd.air(lv.get(bamboo))) {
        if (state_can_survive(env, bd.default_state("minecraft:bamboo"), bamboo)) {
            int height = random.next_int(12) + 5;
            if (random.next_float() < config.at("probability").float_value()) {
                int r = random.next_int(4) + 1;
                for (int xx = origin.x - r; xx <= origin.x + r; xx++)
                    for (int zz = origin.z - r; zz <= origin.z + r; zz++) {
                        int xd = xx - origin.x, zd = zz - origin.z;
                        if (xd * xd + zd * zd <= r * r) {
                            BlockPos pp(xx, lv.height(WORLD_SURFACE, xx, zz) - 1, zz);
                            if (bd.in_tag(lv.get(pp), "minecraft:beneath_bamboo_podzol_replaceable")) lv.set(pp, bd.default_state("minecraft:podzol"));
                        }
                    }
            }
            uint16_t trunk = bd.with(bd.with(bd.with(bd.default_state("minecraft:bamboo"), "age", "1"), "leaves", "none"), "stage", "0");
            uint16_t final_large = bd.with(bd.with(trunk, "leaves", "large"), "stage", "1");
            uint16_t top_large = bd.with(trunk, "leaves", "large"), top_small = bd.with(trunk, "leaves", "small");
            for (int i = 0; i < height && bd.air(lv.get(bamboo)); i++) { lv.set(bamboo, trunk); bamboo = bamboo.above(); }
            if (bamboo.y - origin.y >= 3) {
                lv.set(bamboo, final_large);
                bamboo = bamboo.below(); lv.set(bamboo, top_large);
                bamboo = bamboo.below(); lv.set(bamboo, top_small);
            }
        }
        placed++;
    }
    return placed > 0;
}

bool can_attach_to_face(FeatureEnv& env, const BlockPos& neighbour, int dir_to_neighbour) {
    // MultifaceBlock.canAttachTo: the neighbour must have a sturdy face towards us
    uint16_t s = env.level.get(neighbour);
    static const int opp[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST};
    return env.bd().sturdy(s, opp[dir_to_neighbour]);
}

bool place_vines(FeatureEnv& env, const Json&, Rng&, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (!bd.air(lv.get(origin))) return false;
    static const char* faces[6] = {"down", "up", "north", "south", "west", "east"};
    for (int d = 0; d < 6; d++) {
        if (d == DOWN) continue;
        if (can_attach_to_face(env, origin + Dir(d), d)) {
            uint16_t v = bd.find("minecraft:vine", "east=false,north=false,south=false,up=false,west=false");
            lv.set(origin, bd.with(v, faces[d], "true"));
            return true;
        }
    }
    return false;
}

bool place_vegetation_patch(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin, bool waterlogged) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    const Json& replaceable = config.at("replaceable");
    int x_radius = sample_int(config.at("xz_radius"), random) + 1;
    int z_radius = sample_int(config.at("xz_radius"), random) + 1;
    const bool floor = config.at("surface").str == "floor";
    const Dir inwards = floor ? DOWN : UP, outwards = floor ? UP : DOWN;
    const int vertical_range = int(config.at("vertical_range").int_value());
    float extra_edge = config.at("extra_edge_column_chance").float_value();
    float extra_bottom = config.at("extra_bottom_block_chance").float_value();
    JHashSet surface;
    for (int dx = -x_radius; dx <= x_radius; dx++) {
        bool x_edge = dx == -x_radius || dx == x_radius;
        for (int dz = -z_radius; dz <= z_radius; dz++) {
            bool z_edge = dz == -z_radius || dz == z_radius;
            bool edge = x_edge || z_edge, corner = x_edge && z_edge, edge_not_corner = edge && !corner;
            if (corner) continue;
            if (edge_not_corner) {
                if (!(extra_edge != 0.0f && !(random.next_float() > extra_edge))) continue;
            }
            BlockPos pos = origin.offset(dx, 0, dz);
            for (int offset = 0; bd.air(lv.get(pos)) && offset < vertical_range; offset++) pos = pos + inwards;
            for (int k = 0; !bd.air(lv.get(pos)) && k < vertical_range; k++) pos = pos + outwards;
            BlockPos below = pos + inwards;
            uint16_t below_state = lv.get(below);
            if (bd.air(lv.get(pos)) && bd.sturdy(below_state, outwards)) {
                int depth = sample_int(config.at("depth"), random);
                bool extra = false;
                if (extra_bottom > 0.0f) extra = random.next_float() < extra_bottom;
                depth += extra ? 1 : 0;
                BlockPos ground = below, belowp = below;
                bool placed = true;
                for (int i = 0; i < depth; i++) {
                    uint16_t to_place = provide_state(env, config.at("ground_state"), random, belowp);
                    uint16_t cur = lv.get(belowp);
                    if (bd.name(to_place) != bd.name(cur)) {
                        if (!state_in_set(env, cur, replaceable)) { placed = i != 0; goto done; }
                        lv.set(belowp, to_place);
                        belowp = belowp + inwards;
                    }
                }
            done:
                if (placed) surface.add(ground);
            }
        }
    }
    JHashSet water;
    JHashSet* result = &surface;
    if (waterlogged) {
        for (auto& p : surface.items()) {
            bool exposed = false;
            for (Dir d : {NORTH, EAST, SOUTH, WEST, DOWN}) {
                static const int opp[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST};
                if (!bd.sturdy(lv.get(p + d), opp[d])) { exposed = true; break; }
            }
            if (!exposed) water.add(p);
        }
        for (auto& p : water.items()) lv.set(p, bd.default_state("minecraft:water"));
        result = &water;
    }
    for (auto& p : result->items()) {
        if (config.at("vegetation_chance").float_value() > 0.0f && random.next_float() < config.at("vegetation_chance").float_value()) {
            BlockPos at = waterlogged ? p.below() : p;
            BlockPos veg = (waterlogged ? at : p) + outwards;
            bool ok = place_placed_feature(env, resolve_ref(env, "placed_feature", config.at("vegetation_feature")), random, veg, nullptr);
            if (waterlogged && ok) {
                uint16_t placed = lv.get(p);
                if (bd.has_prop(placed, "waterlogged") && bd.prop(placed, "waterlogged") == "false") lv.set(p, bd.with(placed, "waterlogged", "true"));
            }
        }
    }
    return !result->empty();
}

bool place_freeze_top_layer(FeatureEnv& env, const Json&, Rng&, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    const auto& biomes = *env.biomes;
    for (int dx = 0; dx < 16; dx++)
        for (int dz = 0; dz < 16; dz++) {
            int x = origin.x + dx, z = origin.z + dz;
            int y = lv.height(MOTION_BLOCKING, x, z);
            BlockPos top(x, y, z), below(x, y - 1, z);
            const BiomeInfo& bi = biomes[size_t(lv.biome(top))];
            // shouldFreeze(level, below, false)
            if (!biome_warm_enough_to_rain(bi, below.x, below.y, below.z, env.sea_level)) {
                if (!lv.outside_build_height(below.y)) {
                    uint16_t s = lv.get(below);
                    if (bd.name(s) == "minecraft:water" ) lv.set(below, bd.default_state("minecraft:ice"));
                }
            }
            // shouldSnow(level, top)
            if (bi.has_precipitation && biome_cold_enough_to_snow(bi, top.x, top.y, top.z, env.sea_level) && !lv.outside_build_height(top.y)) {
                uint16_t s = lv.get(top);
                if (bd.air(s) || bd.name(s) == "minecraft:snow") {
                    // SnowLayerBlock.canSurvive
                    uint16_t bs = lv.get(top.below());
                    bool survive;
                    if (bd.in_tag(bs, "minecraft:cannot_support_snow_layer")) survive = false;
                    else if (bd.in_tag(bs, "minecraft:support_override_snow_layer")) survive = true;
                    else survive = bd.collision_face_full(bs, UP) || (bd.name(bs) == "minecraft:snow" && bd.prop(bs, "layers") == "8");
                    if (survive) {
                        lv.set(top, bd.default_state("minecraft:snow"));
                        uint16_t b2 = lv.get(below);
                        if (bd.has_prop(b2, "snowy")) lv.set(below, bd.with(b2, "snowy", "true"));
                    }
                }
            }
        }
    return true;
}

}  // namespace

bool place_feature_type2(FeatureEnv& env, const std::string& type, const Json& config, Rng& random, const BlockPos& origin, bool& handled) {
    handled = true;
    if (type == "spring_feature") return place_spring(env, config, random, origin);
    if (type == "kelp") return place_kelp(env, config, random, origin);
    if (type == "seagrass") return place_seagrass(env, config, random, origin);
    if (type == "sea_pickle") return place_sea_pickle(env, config, random, origin);
    if (type == "block_column") return place_block_column(env, config, random, origin);
    if (type == "block_pile") return place_block_pile(env, config, random, origin);
    if (type == "bamboo") return place_bamboo(env, config, random, origin);
    if (type == "vines") return place_vines(env, config, random, origin);
    if (type == "vegetation_patch") return place_vegetation_patch(env, config, random, origin, false);
    if (type == "waterlogged_vegetation_patch") return place_vegetation_patch(env, config, random, origin, true);
    if (type == "freeze_top_layer") return place_freeze_top_layer(env, config, random, origin);
    handled = false;
    return false;
}

}  // namespace pn::vg
