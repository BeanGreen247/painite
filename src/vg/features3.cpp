// Lakes, rocks, wells and dungeons.
#include "feature.hpp"
#include <algorithm>
#include <cmath>
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
    if (type == "geode") return place_geode(env, config, random, origin);
    if (type == "monster_room") return place_monster_room(env, config, random, origin);
    handled = false;
    return false;
}

}  // namespace pn::vg
