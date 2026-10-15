// Lakes, rocks, wells and dungeons.
#include "feature.hpp"
#include <algorithm>
#include <cmath>
#include "mathutil.hpp"
#include <stdexcept>

namespace pn::vg {

namespace {

bool place_lake(FeatureEnv& env, const Json& config, Rng& random, BlockPos origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (origin.y <= lv.min_y() + 4) return false;
    origin = origin.offset(-8, -4, -8);
    std::vector<uint8_t> grid(2048, 0);
    int spots = random.next_int(4) + 4;
    for (int i = 0; i < spots; i++) {
        double xr = random.next_double() * 6.0 + 3.0;
        double yr = random.next_double() * 4.0 + 2.0;
        double zr = random.next_double() * 6.0 + 3.0;
        double xp = random.next_double() * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
        double yp = random.next_double() * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
        double zp = random.next_double() * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;
        for (int xx = 1; xx < 15; xx++)
            for (int zz = 1; zz < 15; zz++)
                for (int yy = 1; yy < 7; yy++) {
                    double xd = (xx - xp) / (xr / 2.0), yd = (yy - yp) / (yr / 2.0), zd = (zz - zp) / (zr / 2.0);
                    if (xd * xd + yd * yd + zd * zd < 1.0) grid[size_t((xx * 16 + zz) * 8 + yy)] = 1;
                }
    }
    const uint16_t fluid = provide_state(env, config.at("fluid"), random, origin);
    const uint16_t cave_air = bd.default_state("minecraft:cave_air");
    auto edge = [&](int xx, int zz, int yy) {
        auto g = [&](int x, int z, int y) { return grid[size_t((x * 16 + z) * 8 + y)] != 0; };
        return !g(xx, zz, yy) && ((xx < 15 && g(xx + 1, zz, yy)) || (xx > 0 && g(xx - 1, zz, yy)) || (zz < 15 && g(xx, zz + 1, yy)) || (zz > 0 && g(xx, zz - 1, yy)) ||
                                  (yy < 7 && g(xx, zz, yy + 1)) || (yy > 0 && g(xx, zz, yy - 1)));
    };
    for (int xx = 0; xx < 16; xx++)
        for (int zz = 0; zz < 16; zz++)
            for (int yy = 0; yy < 8; yy++) {
                if (!edge(xx, zz, yy)) continue;
                BlockPos p = origin.offset(xx, yy, zz);
                uint16_t s = lv.get(p);
                if (yy >= 4 && (bd.flags(s) & BF_LIQUID)) return false;
                if (yy < 4 && !(bd.flags(s) & BF_SOLID) && s != fluid) return false;
                if (!test_predicate(env, config.at("can_place_feature"), p)) return false;
            }
    for (int xx = 0; xx < 16; xx++)
        for (int zz = 0; zz < 16; zz++)
            for (int yy = 0; yy < 8; yy++) {
                if (!grid[size_t((xx * 16 + zz) * 8 + yy)]) continue;
                BlockPos p = origin.offset(xx, yy, zz);
                if (test_predicate(env, config.at("can_replace_with_air_or_fluid"), p)) lv.set(p, yy >= 4 ? cave_air : fluid);
            }
    uint16_t barrier = provide_state(env, config.at("barrier"), random, origin);
    if (!bd.air(barrier)) {
        for (int xx = 0; xx < 16; xx++)
            for (int zz = 0; zz < 16; zz++)
                for (int yy = 0; yy < 8; yy++) {
                    if (!edge(xx, zz, yy)) continue;
                    if (yy < 4 || random.next_int(2) != 0) {
                        BlockPos p = origin.offset(xx, yy, zz);
                        if ((bd.flags(lv.get(p)) & BF_SOLID) && test_predicate(env, config.at("can_replace_with_barrier"), p)) lv.set(p, barrier);
                    }
                }
    }
    if (bd.water(fluid)) {
        for (int xx = 0; xx < 16; xx++)
            for (int zz = 0; zz < 16; zz++) {
                BlockPos p = origin.offset(xx, 4, zz);
                const BiomeInfo& bi = (*env.biomes)[size_t(lv.biome(p))];
                // Biome.shouldFreeze(level, p, false)
                if (!biome_warm_enough_to_rain(bi, p.x, p.y, p.z, env.sea_level) && !lv.outside_build_height(p.y)) {
                    uint16_t s = lv.get(p);
                    if (bd.name(s) == "minecraft:water" && test_predicate(env, config.at("can_replace_with_air_or_fluid"), p)) lv.set(p, bd.default_state("minecraft:ice"));
                }
            }
    }
    return true;
}

bool place_block_blob(FeatureEnv& env, const Json& config, Rng& random, BlockPos origin) {
    Level& lv = env.level;
    while (origin.y > lv.min_y() + 3 && !test_predicate(env, config.at("can_place_on"), origin.below())) origin = origin.below();
    if (origin.y <= lv.min_y() + 3) return false;
    uint16_t state = env.bd().state(config.at("state"));
    for (int c = 0; c < 3; c++) {
        int xr = random.next_int(2);
        int yr = random.next_int(2);
        int zr = random.next_int(2);
        float tr = float(xr + yr + zr) * 0.333f + 0.5f;
        for (int z = origin.z - zr; z <= origin.z + zr; z++)
            for (int y = origin.y - yr; y <= origin.y + yr; y++)
                for (int x = origin.x - xr; x <= origin.x + xr; x++) {
                    int dx = x - origin.x, dy = y - origin.y, dz = z - origin.z;
                    if (double(dx * dx + dy * dy + dz * dz) <= double(tr * tr)) lv.set(x, y, z, state);
                }
        int a = random.next_int(2); int b = random.next_int(2); int d = random.next_int(2);
        origin = origin.offset(-1 + a, -b, -1 + d);
    }
    return true;
}

bool place_desert_well(FeatureEnv& env, const Json&, Rng& random, BlockPos origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    origin = origin.above();
    while (bd.air(lv.get(origin)) && origin.y > lv.min_y() + 2) origin = origin.below();
    if (bd.name(lv.get(origin)) != "minecraft:sand") return false;
    for (int ox = -2; ox <= 2; ox++)
        for (int oz = -2; oz <= 2; oz++)
            if (bd.air(lv.get(origin.offset(ox, -1, oz))) && bd.air(lv.get(origin.offset(ox, -2, oz)))) return false;
    const uint16_t sand = bd.default_state("minecraft:sand"), slab = bd.default_state("minecraft:sandstone_slab"), sandstone = bd.default_state("minecraft:sandstone"), water = bd.default_state("minecraft:water");
    static const Dir horiz[4] = {NORTH, EAST, SOUTH, WEST};
    for (int oy = -2; oy <= 0; oy++)
        for (int ox = -2; ox <= 2; ox++)
            for (int oz = -2; oz <= 2; oz++) lv.set(origin.offset(ox, oy, oz), sandstone);
    lv.set(origin, water);
    for (Dir d : horiz) lv.set(origin + d, water);
    BlockPos sand_center = origin.below();
    lv.set(sand_center, sand);
    for (Dir d : horiz) lv.set(sand_center + d, sand);
    for (int ox = -2; ox <= 2; ox++)
        for (int oz = -2; oz <= 2; oz++)
            if (ox == -2 || ox == 2 || oz == -2 || oz == 2) lv.set(origin.offset(ox, 1, oz), sandstone);
    lv.set(origin.offset(2, 1, 0), slab);
    lv.set(origin.offset(-2, 1, 0), slab);
    lv.set(origin.offset(0, 1, 2), slab);
    lv.set(origin.offset(0, 1, -2), slab);
    for (int ox = -1; ox <= 1; ox++)
        for (int oz = -1; oz <= 1; oz++) lv.set(origin.offset(ox, 4, oz), (ox == 0 && oz == 0) ? sandstone : slab);
    for (int oy = 1; oy <= 3; oy++) {
        lv.set(origin.offset(-1, oy, -1), sandstone);
        lv.set(origin.offset(-1, oy, 1), sandstone);
        lv.set(origin.offset(1, oy, -1), sandstone);
        lv.set(origin.offset(1, oy, 1), sandstone);
    }
    BlockPos water_positions[5] = {origin, origin + EAST, origin + SOUTH, origin + WEST, origin + NORTH};
    uint16_t sus = bd.default_state("minecraft:suspicious_sand");
    lv.set(water_positions[random.next_int(5)].below(1), sus);
    lv.set(water_positions[random.next_int(5)].below(2), sus);
    return true;
}

bool place_monster_room(FeatureEnv& env, const Json&, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    auto replaceable = [&](uint16_t s) { return !bd.in_tag(s, "minecraft:features_cannot_replace"); };
    auto safe_set = [&](const BlockPos& p, uint16_t s) { if (replaceable(lv.get(p))) lv.set(p, s); };
    const uint16_t cave_air = bd.default_state("minecraft:cave_air");
    int xr = random.next_int(2) + 2;
    int min_x = -xr - 1, max_x = xr + 1;
    int zr = random.next_int(2) + 2;
    int min_z = -zr - 1, max_z = zr + 1;
    int holes = 0;
    for (int dx = min_x; dx <= max_x; dx++)
        for (int dy = -1; dy <= 4; dy++)
            for (int dz = min_z; dz <= max_z; dz++) {
                BlockPos hp = origin.offset(dx, dy, dz);
                bool solid = bd.flags(lv.get(hp)) & BF_SOLID;
                if (dy == -1 && !solid) return false;
                if (dy == 4 && !solid) return false;
                if ((dx == min_x || dx == max_x || dz == min_z || dz == max_z) && dy == 0 && bd.air(lv.get(hp)) && bd.air(lv.get(hp.above()))) holes++;
            }
    if (!(holes >= 1 && holes <= 5)) return false;
    const uint16_t cobble = bd.default_state("minecraft:cobblestone"), mossy = bd.default_state("minecraft:mossy_cobblestone");
    for (int dx = min_x; dx <= max_x; dx++)
        for (int dy = 3; dy >= -1; dy--)
            for (int dz = min_z; dz <= max_z; dz++) {
                BlockPos wall = origin.offset(dx, dy, dz);
                uint16_t ws = lv.get(wall);
                if (dx == min_x || dy == -1 || dz == min_z || dx == max_x || dy == 4 || dz == max_z) {
                    if (wall.y >= lv.min_y() && !(bd.flags(lv.get(wall.below())) & BF_SOLID)) lv.set(wall, cave_air);
                    else if ((bd.flags(ws) & BF_SOLID) && bd.name(ws) != "minecraft:chest") {
                        if (dy == -1 && random.next_int(4) != 0) safe_set(wall, mossy);
                        else safe_set(wall, cobble);
                    }
                } else if (bd.name(ws) != "minecraft:chest" && bd.name(ws) != "minecraft:spawner") safe_set(wall, cave_air);
            }
    static const Dir horiz[4] = {NORTH, EAST, SOUTH, WEST};
    for (int cc = 0; cc < 2; cc++) {
        for (int i = 0; i < 3; i++) {
            int a = random.next_int(xr * 2 + 1);
            int b = random.next_int(zr * 2 + 1);
            BlockPos chest(origin.x + a - xr, origin.y, origin.z + b - zr);
            if (!bd.air(lv.get(chest))) continue;
            int walls = 0;
            for (Dir d : horiz) if (bd.flags(lv.get(chest + d)) & BF_SOLID) walls++;
            if (walls != 1) continue;
            // StructurePiece.reorient
            uint16_t state = bd.default_state("minecraft:chest");
            static const char* names[6] = {"down", "up", "north", "south", "west", "east"};
            static const Dir opp[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST};
            auto cw = [](Dir d) { return d == NORTH ? EAST : d == EAST ? SOUTH : d == SOUTH ? WEST : NORTH; };
            int solid_neighbor = -1;
            bool done = false;
            for (Dir d : horiz) {
                uint16_t ns = lv.get(chest + d);
                if (bd.name(ns) == "minecraft:chest") { done = true; break; }
                if (bd.flags(ns) & BF_SOLID_RENDER) { if (solid_neighbor >= 0) { solid_neighbor = -1; break; } solid_neighbor = d; }
            }
            if (!done) {
                if (solid_neighbor >= 0) state = bd.with(state, "facing", names[opp[solid_neighbor]]);
                else {
                    Dir lock = NORTH;
                    auto solid_at = [&](Dir d) { return (bd.flags(lv.get(chest + d)) & BF_SOLID_RENDER) != 0; };
                    if (solid_at(lock)) lock = opp[lock];
                    if (solid_at(lock)) lock = cw(lock);
                    if (solid_at(lock)) lock = opp[lock];
                    state = bd.with(state, "facing", names[lock]);
                }
            }
            safe_set(chest, state);
            random.next_long();   // the chest's loot table seed
            break;
        }
    }
    safe_set(origin, bd.default_state("minecraft:spawner"));
    random.next_int(4);   // the spawner's mob
    return true;
}


Json parse_json(const char* text) { return Json::parse(text); }

bool place_geode(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    static const Json def_outer = parse_json("{\"type\":\"minecraft:uniform\",\"min_inclusive\":4,\"max_inclusive\":5}"), def_points = parse_json("{\"type\":\"minecraft:uniform\",\"min_inclusive\":3,\"max_inclusive\":4}"),
                      def_offset = parse_json("{\"type\":\"minecraft:uniform\",\"min_inclusive\":1,\"max_inclusive\":2}"), empty = parse_json("{}");
    const Json& outer_dist = config.has("outer_wall_distance") ? config.at("outer_wall_distance") : def_outer;
    const Json& dist_points = config.has("distribution_points") ? config.at("distribution_points") : def_points;
    const Json& point_offset = config.has("point_offset") ? config.at("point_offset") : def_offset;
    int min_off = int(config.get_int("min_gen_offset", -16)), max_off = int(config.get_int("max_gen_offset", 16));
    double noise_mult = config.get_number("noise_multiplier", 0.05);
    double use_potential = config.get_number("use_potential_placements_chance", 0.35), use_alt = config.get_number("use_alternate_layer0_chance", 0.0);
    bool require_alt = config.get_bool("placements_require_layer0_alternate", true);
    int invalid_threshold = int(config.at("invalid_blocks_threshold").int_value());
    const Json& blocks = config.at("blocks");
    const Json& layers = config.has("layers") ? config.at("layers") : empty;
    const Json& crack = config.has("crack") ? config.at("crack") : empty;
    int num_points = sample_int(dist_points, random);
    if (!env.shared.geode_noise) {
        LegacyRandom r(env.seed);
        NoiseParameters np;
        np.first_octave = -4;
        np.amplitudes = {1.0};
        env.shared.geode_noise = std::make_unique<NormalNoise>(r, np);
    }
    const NormalNoise& noise = *env.shared.geode_noise;
    double crack_adjust = double(num_points) / double(int_provider_max(outer_dist));
    double filling = layers.get_number("filling", 1.7), inner_layer = layers.get_number("inner_layer", 2.2), middle_layer = layers.get_number("middle_layer", 3.2), outer_layer = layers.get_number("outer_layer", 4.2);
    double inner_air = 1.0 / std::sqrt(filling);
    double innermost = 1.0 / std::sqrt(inner_layer + crack_adjust);
    double inner_crust = 1.0 / std::sqrt(middle_layer + crack_adjust);
    double outer_crust = 1.0 / std::sqrt(outer_layer + crack_adjust);
    double crack_base = crack.get_number("base_crack_size", 2.0);
    int crack_point_offset = int(crack.get_int("crack_point_offset", 2));
    double crack_roll = random.next_double();
    double crack_size = 1.0 / std::sqrt(crack_base + crack_roll / 2.0 + (num_points > 3 ? crack_adjust : 0.0));
    bool should_crack = double(random.next_float()) < crack.get_number("generate_crack_chance", 1.0);
    struct Pt { BlockPos p; int off; };
    std::vector<Pt> points;
    std::vector<BlockPos> crack_points;
    int invalid = 0;
    for (int i = 0; i < num_points; i++) {
        int x = sample_int(outer_dist, random);
        int y = sample_int(outer_dist, random);
        int z = sample_int(outer_dist, random);
        BlockPos pos = origin.offset(x, y, z);
        uint16_t s = lv.get(pos);
        if (bd.air(s) || state_in_set(env, s, blocks.at("invalid_blocks"))) {
            if (++invalid > invalid_threshold) return false;
        }
        int off = sample_int(point_offset, random);
        points.push_back({pos, off});
    }
    if (should_crack) {
        int idx = random.next_int(4);
        int co = num_points * 2 + 1;
        if (idx == 0) { crack_points = {origin.offset(co, 7, 0), origin.offset(co, 5, 0), origin.offset(co, 1, 0)}; }
        else if (idx == 1) { crack_points = {origin.offset(0, 7, co), origin.offset(0, 5, co), origin.offset(0, 1, co)}; }
        else if (idx == 2) { crack_points = {origin.offset(co, 7, co), origin.offset(co, 5, co), origin.offset(co, 1, co)}; }
        else { crack_points = {origin.offset(0, 7, 0), origin.offset(0, 5, 0), origin.offset(0, 1, 0)}; }
    }
    std::vector<BlockPos> potential;
    const Json& cannot_replace = blocks.at("cannot_replace");
    auto can_replace = [&](uint16_t s) { return !state_in_set(env, s, cannot_replace); };
    auto safe_set = [&](const BlockPos& p, uint16_t s) { if (can_replace(lv.get(p))) lv.set(p, s); };
    for (int z = origin.z + min_off; z <= origin.z + max_off; z++)
        for (int y = origin.y + min_off; y <= origin.y + max_off; y++)
            for (int x = origin.x + min_off; x <= origin.x + max_off; x++) {
                double noise_offset = noise.get_value(x, y, z) * noise_mult;
                double sum_shell = 0.0, sum_crack = 0.0;
                for (auto& pt : points) {
                    double d2 = double((x - pt.p.x) * (x - pt.p.x) + (y - pt.p.y) * (y - pt.p.y) + (z - pt.p.z) * (z - pt.p.z));
                    sum_shell += 1.0 / std::sqrt(d2 + double(pt.off)) + noise_offset;
                }
                for (auto& cp : crack_points) {
                    double d2 = double((x - cp.x) * (x - cp.x) + (y - cp.y) * (y - cp.y) + (z - cp.z) * (z - cp.z));
                    sum_crack += 1.0 / std::sqrt(d2 + double(crack_point_offset)) + noise_offset;
                }
                if (sum_shell < outer_crust) continue;
                BlockPos p(x, y, z);
                if (should_crack && sum_crack >= crack_size && sum_shell < inner_air) {
                    safe_set(p, 0);
                } else if (sum_shell >= inner_air) {
                    safe_set(p, provide_state(env, blocks.at("filling_provider"), random, p));
                } else if (sum_shell >= innermost) {
                    bool alt = double(random.next_float()) < use_alt;
                    safe_set(p, provide_state(env, blocks.at(alt ? "alternate_inner_layer_provider" : "inner_layer_provider"), random, p));
                    if ((!require_alt || alt) && double(random.next_float()) < use_potential) potential.push_back(p);
                } else if (sum_shell >= inner_crust) {
                    safe_set(p, provide_state(env, blocks.at("middle_layer_provider"), random, p));
                } else if (sum_shell >= outer_crust) {
                    safe_set(p, provide_state(env, blocks.at("outer_layer_provider"), random, p));
                }
            }
    const Json& inner_placements = blocks.at("inner_placements");
    static const char* names[6] = {"down", "up", "north", "south", "west", "east"};
    for (auto& crystal : potential) {
        uint16_t state = bd.state(inner_placements.arr[size_t(random.next_int(int(inner_placements.arr.size())))]);
        for (int d = 0; d < 6; d++) {
            if (bd.has_prop(state, "facing")) state = bd.with(state, "facing", names[d]);
            BlockPos pp = crystal + Dir(d);
            uint16_t ps = lv.get(pp);
            if (bd.has_prop(state, "waterlogged")) state = bd.with(state, "waterlogged", (bd.water(ps) && (bd.flags(ps) & BF_SOURCE)) ? "true" : "false");
            if (bd.air(ps) || (bd.name(ps) == "minecraft:water" && (bd.flags(ps) & BF_SOURCE))) { safe_set(pp, state); break; }
        }
    }
    return true;
}

bool place_huge_mushroom(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin, bool red) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    int radius = int(config.get_int("foliage_radius", 2));
    int tree_height = random.next_int(3) + 4;
    if (random.next_int(12) == 0) tree_height *= 2;
    int y = origin.y;
    if (!(y >= lv.min_y() + 1 && y + tree_height + 1 <= lv.max_y())) return false;
    if (!test_predicate(env, config.at("can_place_on"), origin.below())) return false;
    auto radius_for = [&](int dy) {
        if (!red) return dy <= 3 ? 0 : radius;
        if (dy < tree_height && dy >= tree_height - 3) return radius;
        if (dy == tree_height) return radius;
        return 0;
    };
    for (int dy = 0; dy <= tree_height; dy++) {
        int r = radius_for(dy);
        for (int dx = -r; dx <= r; dx++)
            for (int dz = -r; dz <= r; dz++) {
                uint16_t s = lv.get(origin.offset(dx, dy, dz));
                if (!bd.air(s) && !bd.in_tag(s, "minecraft:leaves")) return false;
            }
    }
    auto place_mushroom_block = [&](const BlockPos& p, uint16_t st) {
        uint16_t cur = lv.get(p);
        if (bd.air(cur) || bd.in_tag(cur, "minecraft:replaceable_by_mushrooms")) lv.set(p, st);
    };
    auto tf = [](bool v) { return v ? "true" : "false"; };
    // makeCap
    if (!red) {
        for (int dx = -radius; dx <= radius; dx++)
            for (int dz = -radius; dz <= radius; dz++) {
                bool min_x = dx == -radius, max_x = dx == radius, min_z = dz == -radius, max_z = dz == radius, x_edge = min_x || max_x, z_edge = min_z || max_z;
                if (x_edge && z_edge) continue;
                BlockPos bp = origin.offset(dx, tree_height, dz);
                bool west = min_x || (z_edge && dx == 1 - radius), east = max_x || (z_edge && dx == radius - 1), north = min_z || (x_edge && dz == 1 - radius), south = max_z || (x_edge && dz == radius - 1);
                uint16_t st = provide_state(env, config.at("cap_provider"), random, origin);
                if (bd.has_prop(st, "west") && bd.has_prop(st, "east") && bd.has_prop(st, "north") && bd.has_prop(st, "south"))
                    st = bd.with(bd.with(bd.with(bd.with(st, "west", tf(west)), "east", tf(east)), "north", tf(north)), "south", tf(south));
                place_mushroom_block(bp, st);
            }
    } else {
        for (int dy = tree_height - 3; dy <= tree_height; dy++) {
            int r = dy < tree_height ? radius : radius - 1;
            int center = radius - 2;
            for (int dx = -r; dx <= r; dx++)
                for (int dz = -r; dz <= r; dz++) {
                    bool min_x = dx == -r, max_x = dx == r, min_z = dz == -r, max_z = dz == r, x_edge = min_x || max_x, z_edge = min_z || max_z;
                    if (!(dy >= tree_height || x_edge != z_edge)) continue;
                    BlockPos bp = origin.offset(dx, dy, dz);
                    uint16_t st = provide_state(env, config.at("cap_provider"), random, origin);
                    if (bd.has_prop(st, "west") && bd.has_prop(st, "east") && bd.has_prop(st, "north") && bd.has_prop(st, "south") && bd.has_prop(st, "up"))
                        st = bd.with(bd.with(bd.with(bd.with(bd.with(st, "up", tf(dy >= tree_height - 1)), "west", tf(dx < -center)), "east", tf(dx > center)), "north", tf(dz < -center)), "south", tf(dz > center));
                    place_mushroom_block(bp, st);
                }
        }
    }
    // placeTrunk
    for (int dy = 0; dy < tree_height; dy++) place_mushroom_block(origin.above(dy), provide_state(env, config.at("stem_provider"), random, origin));
    return true;
}

bool place_blue_ice(FeatureEnv& env, const Json&, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (origin.y > env.sea_level - 1) return false;
    auto is_water = [&](const BlockPos& p) { return bd.name(lv.get(p)) == "minecraft:water"; };
    if (!is_water(origin) && !is_water(origin.below())) return false;
    bool found = false;
    for (int d = 0; d < 6; d++) if (d != DOWN && bd.name(lv.get(origin + Dir(d))) == "minecraft:packed_ice") { found = true; break; }
    if (!found) return false;
    const uint16_t blue = bd.default_state("minecraft:blue_ice");
    lv.set(origin, blue);
    for (int i = 0; i < 200; i++) {
        int a = random.next_int(5); int b = random.next_int(6);
        int y_off = a - b;
        int xz = 3;
        if (y_off < 2) xz += y_off / 2;
        if (xz >= 1) {
            int c = random.next_int(xz); int d2 = random.next_int(xz);
            int e = random.next_int(xz); int f = random.next_int(xz);
            BlockPos p = origin.offset(c - d2, y_off, e - f);
            uint16_t s = lv.get(p);
            const std::string& n = bd.name(s);
            if (bd.air(s) || n == "minecraft:water" || n == "minecraft:packed_ice" || n == "minecraft:ice") {
                for (int d = 0; d < 6; d++) if (bd.name(lv.get(p + Dir(d))) == "minecraft:blue_ice") { lv.set(p, blue); break; }
            }
        }
    }
    return true;
}

bool place_underwater_magma(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    int search = int(config.at("floor_search_range").int_value());
    int radius = int(config.at("placement_radius_around_floor").int_value());
    float prob = config.at("placement_probability_per_valid_position").float_value();
    auto water = [&](const BlockPos& p) { return bd.name(lv.get(p)) == "minecraft:water"; };
    if (!water(origin)) return false;
    // Column.scan: only the floor is of interest
    BlockPos m = origin;
    for (int i = 1; i < search && water(m); i++) m = m.below();
    if (water(m)) return false;   // validEdge: not water
    int floor_y = m.y;
    BlockPos floor(origin.x, floor_y, origin.z);
    const uint16_t magma = bd.default_state("minecraft:magma_block");
    auto visible = [&](const BlockPos& p) { return !(bd.flags(lv.get(p)) & BF_SOLID_RENDER); };
    auto water_or_air = [&](uint16_t st) { return bd.name(st) == "minecraft:water" || bd.air(st); };
    int placed = 0;
    for (int z = floor.z - radius; z <= floor.z + radius; z++)
        for (int y = floor.y - radius; y <= floor.y + radius; y++)
            for (int x = floor.x - radius; x <= floor.x + radius; x++) {
                if (!(random.next_float() < prob)) continue;
                BlockPos p(x, y, z);
                if (water_or_air(lv.get(p)) || visible(p.below())) continue;
                bool ok = true;
                static const Dir horiz[4] = {NORTH, EAST, SOUTH, WEST};
                static const Dir opp[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST};
                for (Dir d : horiz) if (visible(p + d)) { ok = false; break; }
                (void)opp;
                if (!ok) continue;
                lv.set(p, magma);
                placed++;
            }
    return placed > 0;
}

bool place_root_system(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (!bd.air(lv.get(origin))) return false;
    int required_space = int(config.at("required_vertical_space_for_tree").int_value());
    int level_test = int(config.at("level_test_distance").int_value()), max_dev = int(config.at("max_level_deviation").int_value());
    int allowed_water = int(config.at("allowed_vertical_water_for_tree").int_value());
    int root_max_height = int(config.at("root_column_max_height").int_value());
    BlockPos working = origin;
    bool tree_ok = false;
    for (int y = 0; y < root_max_height; y++) {
        working = working.above();
        if (lv.height(WORLD_SURFACE, working.x, working.z) < working.y) break;
        if (!test_predicate(env, config.at("allowed_tree_position"), working)) continue;
        // spaceForTree
        bool space = true;
        BlockPos up = working;
        for (int i = 1; i <= required_space && space; i++) {
            up = up.above();
            uint16_t s = lv.get(up);
            if (!(bd.air(s) || (i + 1 <= allowed_water && bd.water(s)))) space = false;
        }
        if (space && level_test > 0) {
            static const Dir from2d[4] = {SOUTH, WEST, NORTH, EAST};
            BlockPos corner = working;
            for (int i = 0; i < 4 && space; i++) {
                corner = corner.offset(kDirX[from2d[i]] * level_test, 0, kDirZ[from2d[i]] * level_test);
                uint16_t below = lv.get(corner.below(max_dev)), above = lv.get(corner.above(max_dev));
                if (bd.air(below) || !bd.air(above)) space = false;
                corner = working;
            }
        }
        if (!space) continue;
        BlockPos below = working.below();
        uint16_t bs = lv.get(below);
        if (bd.lava(bs) || !(bd.flags(bs) & BF_SOLID)) break;
        if (place_placed_feature(env, resolve_ref(env, "placed_feature", config.at("feature")), random, working, nullptr)) {
            // placeDirt
            int target = origin.y + y;
            int root_radius = int(config.at("root_radius").int_value()), attempts = int(config.at("root_placement_attempts").int_value());
            for (int yy = origin.y; yy < target; yy++) {
                BlockPos wp(origin.x, yy, origin.z);
                for (int i = 0; i < attempts; i++) {
                    int a = random.next_int(root_radius); int b = random.next_int(root_radius);
                    int c = random.next_int(root_radius); int d = random.next_int(root_radius);
                    BlockPos p = wp.offset(a - b, 0, c - d);
                    if (state_in_set(env, lv.get(p), config.at("root_replaceable"))) lv.set(p, provide_state(env, config.at("root_state_provider"), random, p));
                    wp = BlockPos(origin.x, yy, origin.z);
                }
            }
            tree_ok = true;
            break;
        }
    }
    if (tree_ok) {
        int rr = int(config.at("hanging_root_radius").int_value()), span = int(config.at("hanging_roots_vertical_span").int_value());
        int attempts = int(config.at("hanging_root_placement_attempts").int_value());
        for (int i = 0; i < attempts; i++) {
            int a = random.next_int(rr); int b = random.next_int(rr);
            int c = random.next_int(span); int d = random.next_int(span);
            int e = random.next_int(rr); int f = random.next_int(rr);
            BlockPos wp = origin.offset(a - b, c - d, e - f);
            if (bd.air(lv.get(wp))) {
                uint16_t target = provide_state(env, config.at("hanging_root_state_provider"), random, wp);
                if (state_can_survive(env, target, wp) && bd.sturdy(lv.get(wp.above()), DOWN)) lv.set(wp, target);
            }
        }
    }
    return true;
}

// ---- multiface growth (glow lichen)
const char* const kFaceNames[6] = {"down", "up", "north", "south", "west", "east"};
const Dir kOpposite[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST};

bool mf_can_attach(FeatureEnv& env, int dir_to_neighbour, const BlockPos& neighbour) {
    const BlockData& bd = env.bd();
    uint16_t s = env.level.get(neighbour);
    int face = kOpposite[dir_to_neighbour];
    return bd.sturdy(s, face) || bd.collision_face_full(s, face);
}
bool mf_has_face(const BlockData& bd, uint16_t s, int d) { return bd.has_prop(s, kFaceNames[d]) && bd.prop(s, kFaceNames[d]) == "true"; }

// MultifaceBlock.getStateForPlacement(oldState, level, pos, direction); kNoState = null
uint16_t mf_state_for_placement(FeatureEnv& env, const std::string& block, uint16_t old, const BlockPos& pos, int dir) {
    const BlockData& bd = env.bd();
    bool is_this = bd.name(old) == block;
    if (!(!is_this || !mf_has_face(bd, old, dir))) return kNoState;
    if (!mf_can_attach(env, dir, pos + Dir(dir))) return kNoState;
    uint16_t ns;
    if (is_this) ns = old;
    else if (bd.water(old) && (bd.flags(old) & BF_SOURCE)) ns = bd.with(bd.default_state(block), "waterlogged", "true");
    else ns = bd.default_state(block);
    return bd.with(ns, kFaceNames[dir], "true");
}

bool mf_can_spread_into(FeatureEnv& env, const std::string& block, const BlockPos& pos, int face) {
    const BlockData& bd = env.bd();
    uint16_t ex = env.level.get(pos);
    bool replaceable = bd.air(ex) || bd.name(ex) == block || (bd.name(ex) == "minecraft:water" && (bd.flags(ex) & BF_SOURCE));
    if (!replaceable) return false;
    // isValidStateForPlacement
    if (bd.name(ex) == block && mf_has_face(bd, ex, face)) return false;
    return mf_can_attach(env, face, pos + Dir(face));
}

// MultifaceSpreader.spreadFromFaceTowardRandomDirection; returns whether something was placed
bool mf_spread_toward_random(FeatureEnv& env, const std::string& block, uint16_t state, const BlockPos& pos, int starting_face, Rng& random) {
    const BlockData& bd = env.bd();
    int dirs[6] = {0, 1, 2, 3, 4, 5};
    for (int i = 6; i > 1; i--) { int swap = random.next_int(i); std::swap(dirs[i - 1], dirs[swap]); }
    for (int spread : dirs) {
        if (kOpposite[spread] == starting_face || spread == starting_face) continue;
        // same axis: down/up (0,1), north/south (2,3), west/east (4,5)
        if ((spread >> 1) == (starting_face >> 1)) continue;
        if (!(mf_has_face(bd, state, starting_face) && !mf_has_face(bd, state, spread))) continue;
        struct Sp { BlockPos pos; int face; };
        Sp types[3] = {{pos, spread}, {pos + Dir(spread), starting_face}, {(pos + Dir(spread)) + Dir(starting_face), int(kOpposite[spread])}};
        for (auto& t : types) {
            if (mf_can_spread_into(env, block, t.pos, t.face)) {
                uint16_t old = env.level.get(t.pos);
                uint16_t ns = mf_state_for_placement(env, block, old, t.pos, t.face);
                if (ns != kNoState) { env.level.set(t.pos, ns); return true; }
                break;
            }
        }
    }
    return false;
}

bool place_multiface(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    std::string block = config.at("block").str;
    if (block != "minecraft:glow_lichen") throw std::runtime_error("multiface growth for " + block + " is not ported yet");
    int search = int(config.get_int("search_range", 10));
    float chance = config.has("chance_of_spreading") ? config.at("chance_of_spreading").float_value() : 0.5f;
    auto air_or_water = [&](uint16_t s) { return bd.air(s) || bd.name(s) == "minecraft:water"; };
    if (!air_or_water(lv.get(origin))) return false;
    std::vector<int> valid;
    if (config.get_bool("can_place_on_ceiling", false)) valid.push_back(UP);
    if (config.get_bool("can_place_on_floor", false)) valid.push_back(DOWN);
    if (config.get_bool("can_place_on_wall", false)) for (int d : {NORTH, EAST, SOUTH, WEST}) valid.push_back(d);
    auto shuffled = [&](std::vector<int> v) { for (int i = int(v.size()); i > 1; i--) { int swap = random.next_int(i); std::swap(v[size_t(i - 1)], v[size_t(swap)]); } return v; };
    auto place_if_possible = [&](const BlockPos& pos, uint16_t old, const std::vector<int>& dirs) {
        for (int d : dirs) {
            if (state_in_set(env, lv.get(pos + Dir(d)), config.at("can_be_placed_on"))) {
                uint16_t ns = mf_state_for_placement(env, block, old, pos, d);
                if (ns == kNoState) return false;
                lv.set(pos, ns);
                if (random.next_float() < chance) mf_spread_toward_random(env, block, ns, pos, d, random);
                return true;
            }
        }
        return false;
    };
    std::vector<int> dirs = shuffled(valid);
    if (place_if_possible(origin, lv.get(origin), dirs)) return true;
    for (int sd : dirs) {
        std::vector<int> placement;
        for (int d : valid) if (d != kOpposite[sd]) placement.push_back(d);
        placement = shuffled(placement);
        for (int i = 0; i < search; i++) {
            BlockPos pos = origin + Dir(sd);
            uint16_t st = lv.get(pos);
            if (!air_or_water(st) && bd.name(st) != block) break;
            if (place_if_possible(pos, st, placement)) return true;
        }
    }
    return false;
}

// ---- dripstone (speleothem)
struct SpCfg {
    std::string base, pointed;   // block names
    const Json* replaceable;
};
bool sp_is_base(FeatureEnv& env, const SpCfg& c, uint16_t s) { return env.bd().name(s) == c.base || state_in_set(env, s, *c.replaceable); }
bool sp_empty_or_water(const BlockData& bd, uint16_t s) { return bd.air(s) || bd.name(s) == "minecraft:water"; }

bool sp_place_base_if_possible(FeatureEnv& env, const SpCfg& c, const BlockPos& p) {
    if (state_in_set(env, env.level.get(p), *c.replaceable)) { env.level.set(p, env.bd().default_state(c.base)); return true; }
    return false;
}

void sp_grow(FeatureEnv& env, const SpCfg& c, const BlockPos& start, int tip_dir, int height, bool merged) {
    const BlockData& bd = env.bd();
    Level& lv = env.level;
    if (!sp_is_base(env, c, lv.get(start + kOpposite[tip_dir]))) return;
    static const char* names[6] = {"down", "up", "north", "south", "west", "east"};
    BlockPos pos = start;
    auto put = [&](const char* thickness) {
        uint16_t st = bd.with(bd.with(bd.default_state(c.pointed), "vertical_direction", names[tip_dir]), "thickness", thickness);
        if (bd.has_prop(st, "waterlogged")) st = bd.with(st, "waterlogged", bd.water(lv.get(pos)) ? "true" : "false");
        lv.set(pos, st);
        pos = pos + Dir(tip_dir);
    };
    if (height >= 3) { put("base"); for (int i = 0; i < height - 3; i++) put("middle"); }
    if (height >= 2) put("frustum");
    if (height >= 1) put(merged ? "tip_merge" : "tip");
}

bool place_speleothem(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& pos) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    SpCfg c{config.at("base_block").at("Name").str, config.at("pointed_block").at("Name").str, &config.at("replaceable_blocks")};
    float taller = config.has("chance_of_taller_generation") ? config.at("chance_of_taller_generation").float_value() : 0.2f;
    float dir_spread = config.has("chance_of_directional_spread") ? config.at("chance_of_directional_spread").float_value() : 0.7f;
    float r2 = config.has("chance_of_spread_radius2") ? config.at("chance_of_spread_radius2").float_value() : 0.5f;
    float r3 = config.has("chance_of_spread_radius3") ? config.at("chance_of_spread_radius3").float_value() : 0.5f;
    bool above = sp_is_base(env, c, lv.get(pos.above())), below = sp_is_base(env, c, lv.get(pos.below()));
    int tip;
    if (above && below) tip = random.next_boolean() ? DOWN : UP;
    else if (above) tip = DOWN;
    else if (below) tip = UP;
    else return false;
    BlockPos root = pos + kOpposite[tip];
    // createPatchOfBaseBlocks
    static const Dir horiz[4] = {NORTH, EAST, SOUTH, WEST};
    sp_place_base_if_possible(env, c, root);
    for (Dir d : horiz) {
        if (!(random.next_float() > dir_spread)) {
            BlockPos p1 = root + d;
            sp_place_base_if_possible(env, c, p1);
            if (!(random.next_float() > r2)) {
                BlockPos p2 = p1 + Dir(random.next_int(6));
                sp_place_base_if_possible(env, c, p2);
                if (!(random.next_float() > r3)) {
                    BlockPos p3 = p2 + Dir(random.next_int(6));
                    sp_place_base_if_possible(env, c, p3);
                }
            }
        }
    }
    int height = (random.next_float() < taller && sp_empty_or_water(bd, lv.get(pos + Dir(tip)))) ? 2 : 1;
    sp_grow(env, c, pos, tip, height, false);
    return true;
}

struct Col { bool has_floor = false, has_ceiling = false; int floor = 0, ceiling = 0; };

bool place_speleothem_cluster(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    SpCfg c{config.at("base_block").at("Name").str, config.at("pointed_block").at("Name").str, &config.at("replaceable_blocks")};
    if (!sp_empty_or_water(bd, lv.get(origin))) return false;
    int height = sample_int(config.at("height"), random);
    float wetness = sample_float(config.at("wetness"), random);
    float density = sample_float(config.at("density"), random);
    int xr = sample_int(config.at("radius"), random);
    int zr = sample_int(config.at("radius"), random);
    int search = int(config.at("floor_to_ceiling_search_range").int_value());
    int max_diff = int(config.at("max_stalagmite_stalactite_height_diff").int_value());
    int deviation = int(config.at("height_deviation").int_value());
    int max_edge = int(config.at("max_distance_from_edge_affecting_chance_of_speleothem").int_value());
    int max_center = int(config.at("max_distance_from_center_affecting_height_bias").int_value());
    float chance_at_max = config.at("chance_of_speleothem_at_max_distance_from_center").float_value();
    auto is_lava = [&](const BlockPos& p) { return bd.name(lv.get(p)) == "minecraft:lava"; };
    auto clamped_map_f = [](float v, float a, float b, float x, float y) { float f = (v - a) / (b - a); return f < 0.0f ? x : f > 1.0f ? y : x + f * (y - x); };
    auto spel_height = [&](int dx, int dz, int max_height) -> int {
        if (random.next_float() > density) return 0;
        int dist = std::abs(dx) + std::abs(dz);
        float mean = float(clamped_map(double(dist), 0.0, double(max_center), double(max_height) / 2.0, 0.0));
        float n = mean + float(random.next_gaussian()) * float(deviation);
        float lo = 0.0f, hi = float(max_height);
        n = n < lo ? lo : std::min(n, hi);
        return int(n);
    };
    auto replace_with_base = [&](BlockPos p, int max_count, Dir d) {
        for (int i = 0; i < max_count; i++) { if (!sp_place_base_if_possible(env, c, p)) return; p = p + d; }
    };
    for (int dx = -xr; dx <= xr; dx++)
        for (int dz = -zr; dz <= zr; dz++) {
            int xe = xr - std::abs(dx), ze = zr - std::abs(dz);
            float chance = clamped_map_f(float(std::min(xe, ze)), 0.0f, float(max_edge), chance_at_max, 1.0f);
            BlockPos pos = origin.offset(dx, 0, dz);
            // Column.scan
            Col base;
            if (!sp_empty_or_water(bd, lv.get(pos))) continue;
            {
                BlockPos m = pos;
                for (int i = 1; i < search && sp_empty_or_water(bd, lv.get(m)); i++) m = m.above();
                if (!sp_empty_or_water(bd, lv.get(m))) { base.has_ceiling = true; base.ceiling = m.y; }
                m = pos;
                for (int i = 1; i < search && sp_empty_or_water(bd, lv.get(m)); i++) m = m.below();
                if (!sp_empty_or_water(bd, lv.get(m))) { base.has_floor = true; base.floor = m.y; }
            }
            if (!base.has_ceiling && !base.has_floor) continue;
            bool want_pool = random.next_float() < wetness;
            Col col = base;
            bool pool = false;
            if (want_pool && base.has_floor) {
                BlockPos fp(pos.x, base.floor, pos.z);
                uint16_t st = lv.get(fp);
                bool ok = !(bd.name(st) == "minecraft:water" || bd.name(st) == c.base || bd.name(st) == c.pointed);
                if (ok) {
                    if (bd.water(lv.get(fp.above()))) ok = false;
                    else {
                        static const Dir horiz[4] = {NORTH, EAST, SOUTH, WEST};
                        auto adj = [&](const BlockPos& q) { uint16_t s2 = lv.get(q); return bd.in_tag(s2, "minecraft:base_stone_overworld") || bd.water(s2); };
                        for (Dir d : horiz) if (!adj(fp + d)) { ok = false; break; }
                        if (ok) ok = adj(fp.below());
                    }
                }
                if (ok) { pool = true; col.floor = base.floor - 1; lv.set(fp, bd.default_state("minecraft:water")); }
            }
            (void)pool;
            bool want_stalactite = random.next_double() < double(chance);
            int stalactite = 0;
            if (base.has_ceiling && want_stalactite && !is_lava(BlockPos(pos.x, base.ceiling, pos.z))) {
                int thickness = sample_int(config.at("speleothem_block_layer_thickness"), random);
                replace_with_base(BlockPos(pos.x, base.ceiling, pos.z), thickness, UP);
                int max_h = col.has_floor ? std::min(height, base.ceiling - col.floor) : height;
                stalactite = spel_height(dx, dz, max_h);
            }
            bool want_stalagmite = random.next_double() < double(chance);
            int stalagmite = 0;
            if (col.has_floor && want_stalagmite && !is_lava(BlockPos(pos.x, col.floor, pos.z))) {
                int thickness = sample_int(config.at("speleothem_block_layer_thickness"), random);
                replace_with_base(BlockPos(pos.x, col.floor, pos.z), thickness, DOWN);
                if (base.has_ceiling) {
                    int r = random.next_int(2 * max_diff + 1) - max_diff;
                    stalagmite = std::max(0, stalactite + r);
                } else stalagmite = spel_height(dx, dz, height);
            }
            int actual_stalagmite, actual_stalactite;
            if (base.has_ceiling && col.has_floor && base.ceiling - stalactite <= col.floor + stalagmite) {
                int floor_y = col.floor, ceiling_y = base.ceiling;
                int lowest = std::max(ceiling_y - stalactite, floor_y + 1);
                int highest = std::min(floor_y + stalagmite, ceiling_y - 1);
                int actual_bottom = random.next_int(highest + 1 - lowest + 1) + lowest;
                int actual_top = actual_bottom - 1;
                actual_stalactite = ceiling_y - actual_bottom;
                actual_stalagmite = actual_top - floor_y;
            } else { actual_stalactite = stalactite; actual_stalagmite = stalagmite; }
            bool merge = random.next_boolean();
            merge = merge && actual_stalactite > 0 && actual_stalagmite > 0 && col.has_floor && base.has_ceiling && actual_stalactite + actual_stalagmite == base.ceiling - col.floor - 1;
            if (base.has_ceiling) sp_grow(env, c, BlockPos(pos.x, base.ceiling - 1, pos.z), DOWN, actual_stalactite, merge);
            if (col.has_floor) sp_grow(env, c, BlockPos(pos.x, col.floor + 1, pos.z), UP, actual_stalagmite, merge);
        }
    return true;
}

// ---- large dripstone
double sp_height(double xz_dist, double radius, double scale, double bluntness) {
    if (xz_dist < bluntness) xz_dist = bluntness;
    double r = xz_dist / radius * 0.384;
    double part1 = 0.75 * std::pow(r, 1.3333333333333333);
    double part2 = std::pow(r, 0.6666666666666666);
    double part3 = 0.3333333333333333 * std::log(r);
    double h = scale * (part1 - part2 - part3);
    h = std::max(h, 0.0);
    return h / 0.384 * radius;
}

struct Wind {
    bool on = false;
    int origin_y = 0, max_offset = 0;
    double wx = 0.0, wz = 0.0;
    BlockPos offset(const BlockPos& p) const {
        if (!on) return p;
        int dy = origin_y - p.y;
        int dx = std::clamp(mth_floor(wx * double(dy)), -max_offset, max_offset);
        int dz = std::clamp(mth_floor(wz * double(dy)), -max_offset, max_offset);
        return p.offset(dx, 0, dz);
    }
};

struct LargeStone {
    BlockPos root;
    bool up;
    int radius;
    double bluntness, scale;
    int height_at(float r) const { return int(sp_height(double(r), double(radius), scale, bluntness)); }
    int height() const { return height_at(0.0f); }
};

bool place_large_dripstone(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (!sp_empty_or_water(bd, lv.get(origin))) return false;
    int search = int(config.get_int("floor_to_ceiling_search_range", 30));
    const Json& replaceable = config.at("replaceable_blocks");
    auto edge_ok = [&](uint16_t s) { return bd.name(s) == "minecraft:dripstone_block" || state_in_set(env, s, replaceable) || bd.name(s) == "minecraft:lava"; };
    int ceiling = 0, floor = 0;
    bool has_c = false, has_f = false;
    BlockPos m = origin;
    for (int i = 1; i < search && sp_empty_or_water(bd, lv.get(m)); i++) m = m.above();
    if (edge_ok(lv.get(m))) { has_c = true; ceiling = m.y; }
    m = origin;
    for (int i = 1; i < search && sp_empty_or_water(bd, lv.get(m)); i++) m = m.below();
    if (edge_ok(lv.get(m))) { has_f = true; floor = m.y; }
    if (!(has_c && has_f)) return false;
    int col_height = ceiling - floor - 1;
    if (col_height < 4) return false;
    const Json& col_radius = config.at("column_radius");
    float ratio = config.at("max_column_radius_to_cave_height_ratio").float_value();
    int max_by_height = int(double(col_height) * double(ratio));
    int rmin = int_provider_min(col_radius), rmax = int_provider_max(col_radius);
    int max_radius = std::min(std::max(max_by_height, rmin), rmax);
    int radius = random.next_int(max_radius - rmin + 1) + rmin;
    auto make = [&](BlockPos root, bool up, const char* blunt_key) {
        double bl = double(sample_float(config.at(blunt_key), random));
        double sc = double(sample_float(config.at("height_scale"), random));
        return LargeStone{root, up, radius, bl, sc};
    };
    LargeStone stalactite = make(BlockPos(origin.x, ceiling - 1, origin.z), false, "stalactite_bluntness");
    LargeStone stalagmite = make(BlockPos(origin.x, floor + 1, origin.z), true, "stalagmite_bluntness");
    int min_wind_radius = int(config.at("min_radius_for_wind").int_value());
    float min_wind_blunt = config.at("min_bluntness_for_wind").float_value();
    auto suitable = [&](const LargeStone& s) { return s.radius >= min_wind_radius && s.bluntness >= double(min_wind_blunt); };
    Wind wind;
    if (suitable(stalactite) && suitable(stalagmite)) {
        wind.on = true;
        wind.origin_y = origin.y;
        wind.max_offset = 16 - radius;
        float speed = sample_float(config.at("wind_speed"), random);
        float lo = 0.0f, hi = kPiF;
        float dir = random.next_float() * (hi - lo) + lo;
        wind.wx = double(mth_cos(double(dir)) * speed);
        wind.wz = double(mth_sin(double(dir)) * speed);
    }
    auto empty_water_lava = [&](const BlockPos& p) { uint16_t s = lv.get(p); return bd.air(s) || bd.name(s) == "minecraft:water" || bd.name(s) == "minecraft:lava"; };
    auto circle_embedded = [&](const BlockPos& center, int xz_radius) {
        if (empty_water_lava(center)) return false;
        float inc = 6.0f / float(xz_radius);
        for (float angle = 0.0f; angle < kPiF * 2.0f; angle += inc) {
            int dx = int(mth_cos(double(angle)) * float(xz_radius));
            int dz = int(mth_sin(double(angle)) * float(xz_radius));
            if (empty_water_lava(center.offset(dx, 0, dz))) return false;
        }
        return true;
    };
    auto move_back = [&](LargeStone& st) {
        while (st.radius > 1) {
            BlockPos new_root = st.root;
            int tries = std::min(10, st.height());
            for (int i = 0; i < tries; i++) {
                if (bd.name(lv.get(new_root)) == "minecraft:lava") return false;
                if (circle_embedded(wind.offset(new_root), st.radius)) { st.root = new_root; return true; }
                new_root = st.up ? new_root.below() : new_root.above();
            }
            st.radius /= 2;
        }
        return false;
    };
    auto place_blocks = [&](LargeStone& st) {
        const uint16_t drip = bd.default_state("minecraft:dripstone_block");
        for (int dx = -st.radius; dx <= st.radius; dx++)
            for (int dz = -st.radius; dz <= st.radius; dz++) {
                float cur = float(std::sqrt(double(dx * dx + dz * dz)));
                if (cur > float(st.radius)) continue;
                int h = st.height_at(cur);
                if (h <= 0) continue;
                if (double(random.next_float()) < 0.2) {
                    float f = random.next_float() * (1.0f - 0.8f) + 0.8f;
                    h = int(float(h) * f);
                }
                BlockPos pos = st.root.offset(dx, 0, dz);
                bool out_of_stone = false;
                int max_y = st.up ? lv.height(WORLD_SURFACE_WG, pos.x, pos.z) : INT32_MAX;
                for (int i = 0; i < h && pos.y < max_y; i++) {
                    BlockPos wp = wind.offset(pos);
                    if (empty_water_lava(wp)) { out_of_stone = true; lv.set(wp, drip); }
                    else if (out_of_stone && bd.in_tag(lv.get(wp), "minecraft:base_stone_overworld")) break;
                    pos = st.up ? pos.above() : pos.below();
                }
            }
    };
    bool a = move_back(stalactite);
    bool b = move_back(stalagmite);
    if (a) place_blocks(stalactite);
    if (b) place_blocks(stalagmite);
    return true;
}

}  // namespace

bool place_feature_type3(FeatureEnv& env, const std::string& type, const Json& config, Rng& random, const BlockPos& origin, bool& handled) {
    handled = true;
    if (type == "lake") return place_lake(env, config, random, origin);
    if (type == "block_blob") return place_block_blob(env, config, random, origin);
    if (type == "desert_well") return place_desert_well(env, config, random, origin);
    if (type == "huge_red_mushroom") return place_huge_mushroom(env, config, random, origin, true);
    if (type == "huge_brown_mushroom") return place_huge_mushroom(env, config, random, origin, false);
    if (type == "blue_ice") return place_blue_ice(env, config, random, origin);
    if (type == "underwater_magma") return place_underwater_magma(env, config, random, origin);
    if (type == "root_system") return place_root_system(env, config, random, origin);
    if (type == "multiface_growth") return place_multiface(env, config, random, origin);
    if (type == "speleothem") return place_speleothem(env, config, random, origin);
    if (type == "speleothem_cluster") return place_speleothem_cluster(env, config, random, origin);
    if (type == "large_dripstone") return place_large_dripstone(env, config, random, origin);
    if (type == "geode") return place_geode(env, config, random, origin);
    if (type == "monster_room") return place_monster_room(env, config, random, origin);
    handled = false;
    return false;
}

}  // namespace pn::vg
