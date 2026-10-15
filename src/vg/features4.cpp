// Nether features: glowstone, fungus-forest vegetation, vines.
#include "feature.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pn::vg {

namespace {

int mth_next_int(Rng& r, int lo, int hi) { return lo >= hi ? lo : r.next_int(hi - lo + 1) + lo; }

bool place_glowstone(FeatureEnv& env, const Json&, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (!bd.air(lv.get(origin))) return false;
    const std::string& above = bd.name(lv.get(origin.above()));
    if (above != "minecraft:netherrack" && above != "minecraft:basalt" && above != "minecraft:blackstone") return false;
    const uint16_t glow = bd.default_state("minecraft:glowstone");
    lv.set(origin, glow);
    for (int i = 0; i < 1500; i++) {
        int a = random.next_int(8); int b = random.next_int(8);
        int c = random.next_int(12);
        int d = random.next_int(8); int e = random.next_int(8);
        BlockPos p = origin.offset(a - b, -c, d - e);
        if (!bd.air(lv.get(p))) continue;
        int n = 0;
        for (int dir = 0; dir < 6; dir++) {
            if (lv.get(p + Dir(dir)) == glow) n++;
            if (n > 1) break;
        }
        if (n == 1) lv.set(p, glow);
    }
    return true;
}

bool place_nether_forest_vegetation(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (!bd.in_tag(lv.get(origin.below()), "minecraft:nylium")) return false;
    int y = origin.y;
    if (!(y >= lv.min_y() + 1 && y + 1 <= lv.max_y())) return false;
    int width = int(config.at("spread_width").int_value()), height = int(config.at("spread_height").int_value());
    int placed = 0;
    for (int i = 0; i < width * width; i++) {
        int a = random.next_int(width); int b = random.next_int(width);
        int c = random.next_int(height); int d = random.next_int(height);
        int e = random.next_int(width); int f = random.next_int(width);
        BlockPos p = origin.offset(a - b, c - d, e - f);
        uint16_t st = provide_state(env, config.at("state_provider"), random, p);
        if (bd.air(lv.get(p)) && p.y > lv.min_y() && state_can_survive(env, st, p)) { lv.set(p, st); placed++; }
    }
    return placed > 0;
}

void vine_column(FeatureEnv& env, Rng& random, BlockPos p, int total, bool up, const char* head, const char* plant) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    for (int h = up ? 1 : 0; h <= total; h++) {
        if (bd.air(lv.get(p))) {
            if (h == total || !bd.air(lv.get(up ? p.above() : p.below()))) {
                lv.set(p, bd.with(bd.default_state(head), "age", std::to_string(mth_next_int(random, 17, 25))));
                break;
            }
            lv.set(p, bd.default_state(plant));
        }
        p = up ? p.above() : p.below();
    }
}

bool place_twisting_vines(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    auto invalid = [&](const BlockPos& p) {
        if (!bd.air(lv.get(p))) return true;
        const std::string& n = bd.name(lv.get(p.below()));
        return n != "minecraft:netherrack" && n != "minecraft:warped_nylium" && n != "minecraft:warped_wart_block";
    };
    if (invalid(origin)) return false;
    int sw = int(config.at("spread_width").int_value()), sh = int(config.at("spread_height").int_value()), mh = int(config.at("max_height").int_value());
    for (int i = 0; i < sw * sw; i++) {
        int dx = mth_next_int(random, -sw, sw);
        int dy = mth_next_int(random, -sh, sh);
        int dz = mth_next_int(random, -sw, sw);
        BlockPos p = origin.offset(dx, dy, dz);
        // findFirstAirBlockAboveGround
        bool ok;
        for (;;) {
            p = p.below();
            if (lv.outside_build_height(p.y)) { ok = false; break; }
            if (!bd.air(lv.get(p))) { p = p.above(); ok = true; break; }
        }
        if (ok && !invalid(p)) {
            int vh = mth_next_int(random, 1, mh);
            if (random.next_int(6) == 0) vh *= 2;
            if (random.next_int(5) == 0) vh = 1;
            vine_column(env, random, p, vh, true, "minecraft:twisting_vines", "minecraft:twisting_vines_plant");
        }
    }
    return true;
}

bool place_weeping_vines(FeatureEnv& env, const Json&, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (!bd.air(lv.get(origin))) return false;
    const std::string& above = bd.name(lv.get(origin.above()));
    if (above != "minecraft:netherrack" && above != "minecraft:nether_wart_block") return false;
    const uint16_t wart = bd.default_state("minecraft:nether_wart_block");
    lv.set(origin, wart);
    for (int i = 0; i < 200; i++) {
        int a = random.next_int(6); int b = random.next_int(6);
        int c = random.next_int(2); int d = random.next_int(5);
        int e = random.next_int(6); int f = random.next_int(6);
        BlockPos p = origin.offset(a - b, c - d, e - f);
        if (!bd.air(lv.get(p))) continue;
        int n = 0;
        for (int dir = 0; dir < 6; dir++) {
            const std::string& nn = bd.name(lv.get(p + Dir(dir)));
            if (nn == "minecraft:netherrack" || nn == "minecraft:nether_wart_block") n++;
            if (n > 1) break;
        }
        if (n == 1) lv.set(p, wart);
    }
    for (int i = 0; i < 100; i++) {
        int a = random.next_int(8); int b = random.next_int(8);
        int c = random.next_int(2); int d = random.next_int(7);
        int e = random.next_int(8); int f = random.next_int(8);
        BlockPos p = origin.offset(a - b, c - d, e - f);
        if (!bd.air(lv.get(p))) continue;
        const std::string& na = bd.name(lv.get(p.above()));
        if (na == "minecraft:netherrack" || na == "minecraft:nether_wart_block") {
            int vh = mth_next_int(random, 1, 8);
            if (random.next_int(6) == 0) vh *= 2;
            if (random.next_int(5) == 0) vh = 1;
            vine_column(env, random, p, vh, false, "minecraft:weeping_vines", "minecraft:weeping_vines_plant");
        }
    }
    return true;
}

bool place_huge_fungus(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (bd.name(lv.get(origin.below())) != config.at("valid_base_block").at("Name").str) return false;
    bool planted = config.get_bool("planted", false);
    int total = mth_next_int(random, 4, 13);
    if (random.next_int(12) == 0) total *= 2;
    if (!planted && origin.y + total + 1 >= lv.range().height) return false;
    bool huge = false;
    if (!planted) huge = random.next_float() < 0.06f;
    lv.set(origin, 0);
    const uint16_t stem = bd.state(config.at("stem_state")), hat = bd.state(config.at("hat_state")), decor = bd.state(config.at("decor_state"));
    auto replaceable = [&](const BlockPos& p, bool check_plants) {
        if (bd.replaceable(lv.get(p))) return true;
        return check_plants ? test_predicate(env, config.at("replaceable_blocks"), p) : false;
    };
    int stem_radius = huge ? 1 : 0;
    for (int dx = -stem_radius; dx <= stem_radius; dx++)
        for (int dz = -stem_radius; dz <= stem_radius; dz++) {
            bool corner = huge && std::abs(dx) == stem_radius && std::abs(dz) == stem_radius;
            for (int dy = 0; dy < total; dy++) {
                BlockPos p = origin.offset(dx, dy, dz);
                if (!replaceable(p, true)) continue;
                if (planted) lv.set(p, stem);
                else if (corner) { if (random.next_float() < 0.1f) lv.set(p, stem); }
                else lv.set(p, stem);
            }
        }
    bool place_vines = bd.name(hat) == "minecraft:nether_wart_block";
    auto try_weeping = [&](const BlockPos& hat_pos) {
        BlockPos p = hat_pos.below();
        if (!bd.air(lv.get(p))) return;
        int h = mth_next_int(random, 1, 5);
        if (random.next_int(7) == 0) h *= 2;
        for (int height = 0; height <= h; height++) {
            if (bd.air(lv.get(p))) {
                if (height == h || !bd.air(lv.get(p.below()))) {
                    lv.set(p, bd.with(bd.default_state("minecraft:weeping_vines"), "age", std::to_string(mth_next_int(random, 23, 25))));
                    break;
                }
                lv.set(p, bd.default_state("minecraft:weeping_vines_plant"));
            }
            p = p.below();
        }
    };
    auto hat_block = [&](const BlockPos& p, float decor_p, float hat_p, float vine_p) {
        if (random.next_float() < decor_p) lv.set(p, decor);
        else if (random.next_float() < hat_p) {
            lv.set(p, hat);
            if (random.next_float() < vine_p) try_weeping(p);
        }
    };
    int hat_height = std::min(random.next_int(1 + total / 3) + 5, total);
    int hat_start = total - hat_height;
    for (int dy = hat_start; dy <= total; dy++) {
        int radius = dy < total - random.next_int(3) ? 2 : 1;
        if (hat_height > 8 && dy < hat_start + 4) radius = 3;
        if (huge) radius++;
        for (int dx = -radius; dx <= radius; dx++)
            for (int dz = -radius; dz <= radius; dz++) {
                bool ex = dx == -radius || dx == radius, ez = dz == -radius || dz == radius;
                bool inside = !ex && !ez && dy != total, corner = ex && ez, bottom = dy < hat_start + 3;
                BlockPos p = origin.offset(dx, dy, dz);
                if (!replaceable(p, false)) continue;
                if (bottom) {
                    if (!inside) {
                        if (lv.get(p.below()) != 0 && bd.name(lv.get(p.below())) == bd.name(hat)) lv.set(p, hat);
                        else if (double(random.next_float()) < 0.15) {
                            lv.set(p, hat);
                            if (place_vines && random.next_int(11) == 0) try_weeping(p);
                        }
                    }
                } else if (inside) hat_block(p, 0.1f, 0.2f, place_vines ? 0.1f : 0.0f);
                else if (corner) hat_block(p, 0.01f, 0.7f, place_vines ? 0.083f : 0.0f);
                else hat_block(p, 5.0E-4f, 0.98f, place_vines ? 0.07f : 0.0f);
            }
    }
    return true;
}

struct Spike { int cx, cz, radius, height; bool guarded; };

std::vector<Spike> end_spikes(int64_t seed) {
    LegacyRandom r(seed);
    int64_t key = r.next_long() & 65535;
    LegacyRandom sr(key);
    int sizes[10];
    for (int i = 0; i < 10; i++) sizes[i] = i;
    for (int i = 10; i > 1; i--) { int swap = sr.next_int(i); std::swap(sizes[i - 1], sizes[swap]); }
    std::vector<Spike> out;
    for (int i = 0; i < 10; i++) {
        double ang = 2.0 * (-3.141592653589793 + (3.141592653589793 / 10.0) * double(i));
        int x = int(std::floor(42.0 * std::cos(ang))), z = int(std::floor(42.0 * std::sin(ang)));
        int size = sizes[i];
        out.push_back({x, z, 2 + size / 3, 76 + size * 3, size == 1 || size == 2});
    }
    return out;
}

bool place_end_spike(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    std::vector<Spike> spikes;
    if (const Json* sj = config.find("spikes")) for (auto& e : sj->arr) spikes.push_back({int(e.get_int("centerX", 0)), int(e.get_int("centerZ", 0)), int(e.get_int("radius", 0)), int(e.get_int("height", 0)), e.get_bool("guarded", false)});
    if (spikes.empty()) spikes = end_spikes(env.seed);
    const uint16_t obsidian = bd.default_state("minecraft:obsidian"), iron_bars = bd.default_state("minecraft:iron_bars");
    for (auto& sp : spikes) {
        if (!((origin.x >> 4) == (sp.cx >> 4) && (origin.z >> 4) == (sp.cz >> 4))) continue;
        int r = sp.radius;
        for (int z = sp.cz - r; z <= sp.cz + r; z++)
            for (int y = lv.min_y(); y <= sp.height + 10; y++)
                for (int x = sp.cx - r; x <= sp.cx + r; x++) {
                    double d = double((x - sp.cx) * (x - sp.cx) + (z - sp.cz) * (z - sp.cz));
                    if (d <= double(r * r + 1) && y < sp.height) lv.set(x, y, z, obsidian);
                    else if (y > 65) lv.set(x, y, z, 0);
                }
        if (sp.guarded) {
            for (int dx = -2; dx <= 2; dx++)
                for (int dz = -2; dz <= 2; dz++)
                    for (int dy = 0; dy <= 3; dy++) {
                        bool xs = std::abs(dx) == 2, zs = std::abs(dz) == 2, top = dy == 3;
                        if (!(xs || zs || top)) continue;
                        bool xe = dx == -2 || dx == 2 || top, ze = dz == -2 || dz == 2 || top;
                        auto tf = [](bool v) { return v ? "true" : "false"; };
                        uint16_t st = bd.with(bd.with(bd.with(bd.with(iron_bars, "north", tf(xe && dz != -2)), "south", tf(xe && dz != 2)), "west", tf(ze && dx != -2)), "east", tf(ze && dx != 2));
                        lv.set(sp.cx + dx, sp.height + dy, sp.cz + dz, st);
                    }
        }
        random.next_float();   // the end crystal's rotation
        BlockPos crystal(sp.cx, sp.height + 1, sp.cz);
        lv.set(crystal.below(), bd.default_state("minecraft:bedrock"));
        lv.set(crystal, bd.default_state("minecraft:fire"));
    }
    return true;
}

bool place_end_platform(FeatureEnv& env, const Json&, Rng&, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    for (int dz = -2; dz <= 2; dz++)
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -1; dy < 3; dy++) {
                BlockPos p = origin.offset(dx, dy, dz);
                uint16_t want = dy == -1 ? bd.default_state("minecraft:obsidian") : 0;
                if (lv.get(p) != want) lv.set(p, want);
            }
    return true;
}

bool place_end_island(FeatureEnv& env, const Json&, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const uint16_t end_stone = env.bd().default_state("minecraft:end_stone");
    float size = float(random.next_int(3)) + 4.0f;
    for (int y = 0; size > 0.5f; y--) {
        int lo = int(std::floor(double(-size))), hi = int(std::ceil(double(size)));
        for (int x = lo; x <= hi; x++)
            for (int z = lo; z <= hi; z++)
                if (float(x * x + z * z) <= (size + 1.0f) * (size + 1.0f)) lv.set(origin.offset(x, y, z), end_stone);
        int step = random.next_int(2);
        size -= float(step) + 0.5f;
    }
    return true;
}

std::vector<BlockPos> within_manhattan(const BlockPos& o, int rx, int ry, int rz) {
    std::vector<BlockPos> out;
    int max_depth = rx + ry + rz;
    for (int depth = 0; depth <= max_depth; depth++) {
        int max_x = std::min(rx, depth);
        for (int x = -max_x; x <= max_x; x++) {
            int max_y = std::min(ry, depth - std::abs(x));
            for (int y = -max_y; y <= max_y; y++) {
                int z = depth - std::abs(x) - std::abs(y);
                if (z > rz) continue;
                out.emplace_back(o.x + x, o.y + y, o.z + z);
                if (z != 0) out.emplace_back(o.x + x, o.y + y, o.z - z);
            }
        }
    }
    return out;
}
int manhattan(const BlockPos& a, const BlockPos& b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z); }

bool place_basalt_columns(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    const int lava_sea = env.sea_level;
    static const char* cannot[] = {"minecraft:lava", "minecraft:bedrock", "minecraft:magma_block", "minecraft:soul_sand", "minecraft:nether_bricks", "minecraft:nether_brick_fence", "minecraft:nether_brick_stairs", "minecraft:nether_wart", "minecraft:chest", "minecraft:spawner"};
    auto cannot_place_on = [&](uint16_t s) { const std::string& n = bd.name(s); for (auto c : cannot) if (n == c) return true; return false; };
    auto air_or_lava_ocean = [&](const BlockPos& p) { uint16_t s = lv.get(p); return bd.air(s) || (bd.name(s) == "minecraft:lava" && p.y <= lava_sea); };
    auto can_place_at = [&](const BlockPos& c) { if (!air_or_lava_ocean(c)) return false; uint16_t b = lv.get(c.below()); return !bd.air(b) && !cannot_place_on(b); };
    if (!can_place_at(origin)) return false;
    int column_height = sample_int(config.at("height"), random);
    bool clustered = random.next_float() < 0.9f;
    int reach = std::min(column_height, clustered ? 5 : 8);
    int count = clustered ? 50 : 15;
    const uint16_t basalt = bd.default_state("minecraft:basalt");
    bool placed = false;
    for (int i = 0; i < count; i++) {
        int px = origin.x - reach + random.next_int(2 * reach + 1);
        int py = origin.y + random.next_int(1);
        int pz = origin.z - reach + random.next_int(2 * reach + 1);
        BlockPos pos(px, py, pz);
        int blocks_to_place = column_height - manhattan(pos, origin);
        if (blocks_to_place < 0) continue;
        int col_reach = sample_int(config.at("reach"), random);
        // placeColumn
        bool placed_col = false;
        for (int z = pos.z - col_reach; z <= pos.z + col_reach; z++)
            for (int x = pos.x - col_reach; x <= pos.x + col_reach; x++) {
                BlockPos p(x, pos.y, z);
                int step_limit = manhattan(p, pos);
                bool found = false;
                BlockPos cursor = p;
                if (air_or_lava_ocean(p)) {   // findSurface
                    int limit = step_limit;
                    while (cursor.y > lv.min_y() + 1 && limit > 0) {
                        limit--;
                        if (can_place_at(cursor)) { found = true; break; }
                        cursor = cursor.below();
                    }
                } else {   // findAir
                    int limit = step_limit;
                    while (cursor.y <= lv.max_y() && limit > 0) {
                        limit--;
                        uint16_t s = lv.get(cursor);
                        if (cannot_place_on(s)) break;
                        if (bd.air(s)) { found = true; break; }
                        cursor = cursor.above();
                    }
                }
                if (!found) continue;
                int blocks_y = blocks_to_place - step_limit / 2;
                for (; blocks_y >= 0; blocks_y--) {
                    if (air_or_lava_ocean(cursor)) { lv.set(cursor, basalt); cursor = cursor.above(); placed_col = true; }
                    else { if (lv.get(cursor) != basalt) break; cursor = cursor.above(); }
                }
            }
        placed |= placed_col;
    }
    return placed;
}

bool place_basalt_pillar(FeatureEnv& env, const Json&, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (!(bd.air(lv.get(origin)) && !bd.air(lv.get(origin.above())))) return false;
    const uint16_t basalt = bd.default_state("minecraft:basalt");
    BlockPos pos = origin;
    bool hang[4] = {true, true, true, true};
    const Dir dirs[4] = {NORTH, SOUTH, WEST, EAST};
    while (bd.air(lv.get(pos))) {
        if (lv.outside_build_height(pos.y)) return true;
        lv.set(pos, basalt);
        for (int i = 0; i < 4; i++) {
            if (hang[i]) {
                if (random.next_int(10) != 0) { lv.set(pos + dirs[i], basalt); } else hang[i] = false;
            }
        }
        pos = pos.below();
    }
    pos = pos.above();
    for (int i = 0; i < 4; i++) if (random.next_boolean()) lv.set(pos + dirs[i], basalt);
    pos = pos.below();
    for (int dx = -3; dx < 4; dx++)
        for (int dz = -3; dz < 4; dz++) {
            int prob = std::abs(dx) * std::abs(dz);
            if (random.next_int(10) < 10 - prob) {
                BlockPos base = pos.offset(dx, 0, dz);
                int max_drop = 3;
                while (bd.air(lv.get(base.below()))) { base = base.below(); if (--max_drop <= 0) break; }
                if (!bd.air(lv.get(base.below()))) lv.set(base, basalt);
            }
        }
    return true;
}

bool place_delta(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    static const char* cannot[] = {"minecraft:bedrock", "minecraft:nether_bricks", "minecraft:nether_brick_fence", "minecraft:nether_brick_stairs", "minecraft:nether_wart", "minecraft:chest", "minecraft:spawner"};
    const uint16_t contents = bd.state(config.at("contents")), rim = bd.state(config.at("rim"));
    auto clear = [&](const BlockPos& p) {
        uint16_t s = lv.get(p);
        if (bd.name(s) == bd.name(contents)) return false;
        for (auto c : cannot) if (bd.name(s) == c) return false;
        for (int d = 0; d < 6; d++) {
            bool air = bd.air(lv.get(p + Dir(d)));
            if ((air && d != UP) || (!air && d == UP)) return false;
        }
        return true;
    };
    bool any = false;
    bool spawn_rim = random.next_double() < 0.9;
    int rim_x = spawn_rim ? sample_int(config.at("rim_size"), random) : 0;
    int rim_z = spawn_rim ? sample_int(config.at("rim_size"), random) : 0;
    bool has_rim = spawn_rim && rim_x != 0 && rim_z != 0;
    int rx = sample_int(config.at("size"), random);
    int rz = sample_int(config.at("size"), random);
    int limit = std::max(rx, rz);
    for (auto& pos : within_manhattan(origin, rx, 0, rz)) {
        if (manhattan(pos, origin) > limit) break;
        if (clear(pos)) {
            if (has_rim) { any = true; lv.set(pos, rim); }
            BlockPos off = pos.offset(rim_x, 0, rim_z);
            if (clear(off)) { any = true; lv.set(off, contents); }
        }
    }
    return any;
}

bool place_replace_blobs(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    const std::string target = config.at("target").at("Name").str;
    const uint16_t replace = bd.state(config.at("state"));
    BlockPos cursor(origin.x, std::clamp(origin.y, lv.min_y() + 1, lv.max_y()), origin.z);
    bool found = false;
    while (cursor.y > lv.min_y() + 1) {
        if (bd.name(lv.get(cursor)) == target) { found = true; break; }
        cursor = cursor.below();
    }
    if (!found) return false;
    int rx = sample_int(config.at("radius"), random);
    int ry = sample_int(config.at("radius"), random);
    int rz = sample_int(config.at("radius"), random);
    int maxr = std::max(rx, std::max(ry, rz));
    bool any = false;
    for (auto& pos : within_manhattan(cursor, rx, ry, rz)) {
        if (manhattan(pos, cursor) > maxr) break;
        if (bd.name(lv.get(pos)) == target) { lv.set(pos, replace); any = true; }
    }
    return any;
}

bool place_spike(FeatureEnv& env, const Json& config, Rng& random, BlockPos origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    while (bd.air(lv.get(origin)) && origin.y > lv.min_y() + 2) origin = origin.below();
    if (!test_predicate(env, config.at("can_place_on"), origin)) return false;
    const uint16_t state = bd.state(config.at("state"));
    origin = origin.above(random.next_int(4));
    int height = random.next_int(4) + 7;
    int width = height / 4 + random.next_int(2);
    if (width > 1 && random.next_int(60) == 0) origin = origin.above(10 + random.next_int(30));
    for (int yo = 0; yo < height; yo++) {
        float scale = (1.0f - float(yo) / float(height)) * float(width);
        int nw = int(std::ceil(double(scale)));
        for (int xo = -nw; xo <= nw; xo++) {
            float dx = float(std::abs(xo)) - 0.25f;
            for (int zo = -nw; zo <= nw; zo++) {
                float dz = float(std::abs(zo)) - 0.25f;
                bool inside = (xo == 0 && zo == 0) || !(dx * dx + dz * dz > scale * scale);
                if (!inside) continue;
                bool edge = xo == -nw || xo == nw || zo == -nw || zo == nw;
                if (edge && random.next_float() > 0.75f) continue;
                BlockPos p = origin.offset(xo, yo, zo);
                if (bd.air(lv.get(p)) || test_predicate(env, config.at("can_replace"), p)) lv.set(p, state);
                if (yo != 0 && nw > 1) {
                    BlockPos n = origin.offset(xo, -yo, zo);
                    if (bd.air(lv.get(n)) || test_predicate(env, config.at("can_replace"), n)) lv.set(n, state);
                }
            }
        }
    }
    int pillar = std::min(std::max(width - 1, 0), 1);
    for (int xo = -pillar; xo <= pillar; xo++)
        for (int zo = -pillar; zo <= pillar; zo++) {
            BlockPos cursor = origin.offset(xo, -1, zo);
            int run = 50;
            if (std::abs(xo) == 1 && std::abs(zo) == 1) run = random.next_int(5);
            while (cursor.y > 50) {
                uint16_t s = lv.get(cursor);
                if (!bd.air(s) && !test_predicate(env, config.at("can_replace"), cursor) && s != state) break;
                lv.set(cursor, state);
                cursor = cursor.below();
                if (--run <= 0) { int d = random.next_int(5) + 1; cursor = cursor.below(d); run = random.next_int(5); }
            }
        }
    return true;
}

bool place_void_start_platform(FeatureEnv& env, const Json&, Rng&, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    int cx = origin.x >> 4, cz = origin.z >> 4;
    if (std::max(std::abs(cx - 0), std::abs(cz - 0)) > 1) return true;
    int py = origin.y + 3;
    for (int z = cz * 16; z <= cz * 16 + 15; z++)
        for (int x = cx * 16; x <= cx * 16 + 15; x++)
            if (std::max(std::abs(8 - x), std::abs(8 - z)) <= 16) lv.set(x, py, z, (x == 8 && z == 8) ? bd.default_state("minecraft:cobblestone") : bd.default_state("minecraft:stone"));
    return true;
}

// ---- chorus plant
uint16_t chorus_state(FeatureEnv& env, const BlockPos& pos) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    auto is_chorus = [&](const BlockPos& p) { const std::string& n = bd.name(lv.get(p)); return n == "minecraft:chorus_plant" || n == "minecraft:chorus_flower"; };
    uint16_t st = bd.default_state("minecraft:chorus_plant");
    auto tf = [](bool v) { return v ? "true" : "false"; };
    uint16_t below = lv.get(pos.below());
    st = bd.with(st, "down", tf(bd.name(below) == "minecraft:chorus_plant" || bd.name(below) == "minecraft:chorus_flower" || bd.in_tag(below, "minecraft:supports_chorus_plant")));
    st = bd.with(st, "up", tf(is_chorus(pos.above())));
    st = bd.with(st, "north", tf(is_chorus(pos + NORTH)));
    st = bd.with(st, "east", tf(is_chorus(pos + EAST)));
    st = bd.with(st, "south", tf(is_chorus(pos + SOUTH)));
    st = bd.with(st, "west", tf(is_chorus(pos + WEST)));
    return st;
}

void chorus_grow(FeatureEnv& env, const BlockPos& current, Rng& random, const BlockPos& start, int max_spread, int depth) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    static const Dir horiz[4] = {NORTH, EAST, SOUTH, WEST};
    static const Dir opp[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST};
    auto all_empty = [&](const BlockPos& pos, int ignore) {
        for (Dir d : horiz) if (int(d) != ignore && !bd.air(lv.get(pos + d))) return false;
        return true;
    };
    int height = random.next_int(4) + 1;
    if (depth == 0) height++;
    for (int i = 0; i < height; i++) {
        BlockPos target = current.above(i + 1);
        if (!all_empty(target, -1)) return;
        lv.set(target, chorus_state(env, target));
        lv.set(target.below(), chorus_state(env, target.below()));
    }
    bool placed_stem = false;
    if (depth < 4) {
        int stems = random.next_int(4);
        if (depth == 0) stems++;
        for (int i = 0; i < stems; i++) {
            Dir dir = horiz[random.next_int(4)];
            BlockPos target = current.above(height) + dir;
            if (std::abs(target.x - start.x) < max_spread && std::abs(target.z - start.z) < max_spread && bd.air(lv.get(target)) && bd.air(lv.get(target.below())) && all_empty(target, opp[dir])) {
                placed_stem = true;
                lv.set(target, chorus_state(env, target));
                BlockPos back = target + opp[dir];
                lv.set(back, chorus_state(env, back));
                chorus_grow(env, target, random, start, max_spread, depth + 1);
            }
        }
    }
    if (!placed_stem) lv.set(current.above(height), env.bd().with(bd.default_state("minecraft:chorus_flower"), "age", "5"));
}

bool place_chorus_plant(FeatureEnv& env, const Json&, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    if (bd.air(lv.get(origin)) && bd.in_tag(lv.get(origin.below()), "minecraft:supports_chorus_plant")) {
        lv.set(origin, chorus_state(env, origin));
        chorus_grow(env, origin, random, origin, 8, 0);
        return true;
    }
    return false;
}

bool place_end_gateway(FeatureEnv& env, const Json&, Rng&, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    const uint16_t bedrock = bd.default_state("minecraft:bedrock"), gateway = bd.default_state("minecraft:end_gateway");
    for (int z = origin.z - 1; z <= origin.z + 1; z++)
        for (int y = origin.y - 2; y <= origin.y + 2; y++)
            for (int x = origin.x - 1; x <= origin.x + 1; x++) {
                bool sx = x == origin.x, sy = y == origin.y, sz = z == origin.z, end = std::abs(y - origin.y) == 2;
                if (sx && sy && sz) lv.set(x, y, z, gateway);
                else if (sy) lv.set(x, y, z, 0);
                else if (end && sx && sz) lv.set(x, y, z, bedrock);
                else if ((sx || sz) && !end) lv.set(x, y, z, bedrock);
                else lv.set(x, y, z, 0);
            }
    return true;
}

// ---- iceberg
bool place_iceberg(FeatureEnv& env, const Json& config, Rng& random, BlockPos origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    origin = BlockPos(origin.x, env.sea_level, origin.z);
    const uint16_t main = bd.state(config.at("state"));
    const uint16_t snow_block = bd.default_state("minecraft:snow_block"), water = bd.default_state("minecraft:water");
    auto is_iceberg = [&](uint16_t st) { const std::string& n = bd.name(st); return n == "minecraft:packed_ice" || n == "minecraft:snow_block" || n == "minecraft:blue_ice"; };
    bool snow_on_top = random.next_double() > 0.7;
    double shape_angle = random.next_double() * 2.0 * 3.141592653589793;
    int ellipse_a = 11 - random.next_int(5);
    int ellipse_c = 3 + random.next_int(3);
    bool is_ellipse = random.next_double() > 0.7;
    int over_water;
    if (is_ellipse) over_water = random.next_int(6) + 6; else over_water = random.next_int(15) + 3;
    if (!is_ellipse && random.next_double() > 0.9) over_water += random.next_int(19) + 7;
    int under_water = std::min(over_water + random.next_int(11), 18);
    int w1 = random.next_int(7); int w2 = random.next_int(5);
    int width = std::min(over_water + w1 - w2, 11);
    int a = is_ellipse ? ellipse_a : 11;
    auto ceil_f = [](float v) { int i = int(v); return v > float(i) ? i + 1 : i; };
    auto round_radius = [&](int yo, int height, int w) {
        float k = 3.5f - random.next_float();
        float scale = (1.0f - float(std::pow(double(yo), 2.0)) / (float(height) * k)) * float(w);
        if (height > 15 + random.next_int(5)) {
            int t = yo < 3 + random.next_int(6) ? yo / 2 : yo;
            scale = (1.0f - float(t) / (float(height) * k * 0.4f)) * float(w);
        }
        return ceil_f(scale / 2.0f);
    };
    auto ellipse_radius = [&](int yo, int height, int w) { float scale = (1.0f - float(std::pow(double(yo), 2.0)) / (float(height) * 1.0f)) * float(w); return ceil_f(scale / 2.0f); };
    auto steep_radius = [&](int yo, int height, int w) { float k = 1.0f + random.next_float() / 2.0f; float scale = (1.0f - float(yo) / (float(height) * k)) * float(w); return ceil_f(scale / 2.0f); };
    auto sd_ellipse = [&](int xo, int zo, int ox, int oz, int ea, int ec, double angle) {
        double t1 = (double(xo - ox) * std::cos(angle) - double(zo - oz) * std::sin(angle)) / double(ea);
        double t2 = (double(xo - ox) * std::sin(angle) + double(zo - oz) * std::cos(angle)) / double(ec);
        return std::pow(t1, 2.0) + std::pow(t2, 2.0) - 1.0;
    };
    auto ellipse_c_at = [&](int yo, int height, int c) { int cc = c; if (yo > 0 && height - yo <= 3) cc = c - (4 - (height - yo)); return cc; };
    auto set_iceberg_block = [&](const BlockPos& pos, int h_diff, int height) {
        uint16_t st = lv.get(pos);
        const std::string& n = bd.name(st);
        if (bd.air(st) || n == "minecraft:snow_block" || n == "minecraft:ice" || n == "minecraft:water") {
            bool randomness = !is_ellipse || random.next_double() > 0.05;
            int divisor = is_ellipse ? 3 : 2;
            if (snow_on_top && n != "minecraft:water") {
                int r = random.next_int(std::max(1, height / divisor));
                if (double(h_diff) <= double(r) + double(height) * 0.6 && randomness) { lv.set(pos, snow_block); return; }
            }
            lv.set(pos, main);
        }
    };
    auto gen_block = [&](int height, int xo, int yo, int zo, int radius, int aa) {
        double sd;
        if (is_ellipse) sd = sd_ellipse(xo, zo, 0, 0, aa, ellipse_c_at(yo, height, ellipse_c), shape_angle);
        else {
            float off = 10.0f * std::min(std::max(random.next_float(), 0.2f), 0.8f) / float(radius);
            sd = double(off) + std::pow(double(xo), 2.0) + std::pow(double(zo), 2.0) - std::pow(double(radius), 2.0);
        }
        if (sd < 0.0) {
            BlockPos pos = origin.offset(xo, yo, zo);
            double compare = is_ellipse ? -0.5 : double(-6 - random.next_int(3));
            if (sd > compare && random.next_double() > 0.9) return;
            set_iceberg_block(pos, height - yo, height);
        }
    };
    for (int xo = -a; xo < a; xo++)
        for (int zo = -a; zo < a; zo++)
            for (int yo = 0; yo < over_water; yo++) {
                int radius = is_ellipse ? ellipse_radius(yo, over_water, width) : round_radius(yo, over_water, width);
                if (is_ellipse || xo < radius) gen_block(over_water, xo, yo, zo, radius, a);
            }
    // smooth
    {
        int sa = is_ellipse ? ellipse_a : width / 2;
        for (int x = -sa; x <= sa; x++)
            for (int z = -sa; z <= sa; z++)
                for (int yo = 0; yo <= over_water; yo++) {
                    BlockPos pos = origin.offset(x, yo, z);
                    uint16_t st = lv.get(pos);
                    if (is_iceberg(st) || bd.name(st) == "minecraft:snow") {
                        if (bd.air(lv.get(pos.below()))) { lv.set(pos, 0); lv.set(pos.above(), 0); }
                        else if (is_iceberg(st)) {
                            int counter = 0;
                            for (Dir d : {WEST, EAST, NORTH, SOUTH}) if (!is_iceberg(lv.get(pos + d))) counter++;
                            if (counter >= 3) lv.set(pos, 0);
                        }
                    }
                }
    }
    for (int xo = -a; xo < a; xo++)
        for (int zo = -a; zo < a; zo++)
            for (int yo = -1; yo > -under_water; yo--) {
                int new_a = is_ellipse ? ceil_f(float(a) * (1.0f - float(std::pow(double(yo), 2.0)) / (float(under_water) * 8.0f))) : a;
                int radius = steep_radius(-yo, under_water, width);
                if (xo < radius) gen_block(under_water, xo, yo, zo, radius, new_a);
            }
    bool cut = is_ellipse ? random.next_double() > 0.1 : random.next_double() > 0.7;
    if (cut) {
        int sign_x = random.next_boolean() ? -1 : 1;
        int sign_z = random.next_boolean() ? -1 : 1;
        int x_off = random.next_int(std::max(width / 2 - 2, 1));
        if (random.next_boolean()) x_off = width / 2 + 1 - random.next_int(std::max(width - width / 2 - 1, 1));
        int z_off = random.next_int(std::max(width / 2 - 2, 1));
        if (random.next_boolean()) z_off = width / 2 + 1 - random.next_int(std::max(width - width / 2 - 1, 1));
        if (is_ellipse) { x_off = z_off = random.next_int(std::max(ellipse_a - 5, 1)); }
        int lox = sign_x * x_off, loz = sign_z * z_off;
        double angle = is_ellipse ? shape_angle + (3.141592653589793 / 2) : random.next_double() * 2.0 * 3.141592653589793;
        auto carve = [&](int radius, int yo, bool under) {
            int ca = radius + 1 + ellipse_a / 3;
            int cc = std::min(radius - 3, 3) + ellipse_c / 2 - 1;
            for (int xo = -ca; xo < ca; xo++)
                for (int zo = -ca; zo < ca; zo++) {
                    double sd = sd_ellipse(xo, zo, lox, loz, ca, cc, angle);
                    if (sd < 0.0) {
                        BlockPos pos = origin.offset(xo, yo, zo);
                        uint16_t st = lv.get(pos);
                        if (is_iceberg(st) || bd.name(st) == "minecraft:snow_block") {
                            if (under) lv.set(pos, water);
                            else { lv.set(pos, 0); if (bd.name(lv.get(pos.above())) == "minecraft:snow") lv.set(pos.above(), 0); }
                        }
                    }
                }
        };
        for (int yo = 0; yo < over_water - 3; yo++) carve(round_radius(yo, over_water, width), yo, false);
        for (int yo = -1; yo > -over_water + random.next_int(5); yo--) carve(steep_radius(-yo, over_water, width), yo, true);
    }
    return true;
}

}  // namespace

bool place_feature_type4(FeatureEnv& env, const std::string& type, const Json& config, Rng& random, const BlockPos& origin, bool& handled) {
    handled = true;
    if (type == "huge_fungus") return place_huge_fungus(env, config, random, origin);
    if (type == "end_spike") return place_end_spike(env, config, random, origin);
    if (type == "end_platform") return place_end_platform(env, config, random, origin);
    if (type == "end_island") return place_end_island(env, config, random, origin);
    if (type == "basalt_columns") return place_basalt_columns(env, config, random, origin);
    if (type == "basalt_pillar") return place_basalt_pillar(env, config, random, origin);
    if (type == "delta_feature") return place_delta(env, config, random, origin);
    if (type == "netherrack_replace_blobs") return place_replace_blobs(env, config, random, origin);
    if (type == "spike") return place_spike(env, config, random, origin);
    if (type == "void_start_platform") return place_void_start_platform(env, config, random, origin);
    if (type == "chorus_plant") return place_chorus_plant(env, config, random, origin);
    if (type == "end_gateway") return place_end_gateway(env, config, random, origin);
    if (type == "iceberg") return place_iceberg(env, config, random, origin);
    if (type == "glowstone_blob") return place_glowstone(env, config, random, origin);
    if (type == "nether_forest_vegetation") return place_nether_forest_vegetation(env, config, random, origin);
    if (type == "twisting_vines") return place_twisting_vines(env, config, random, origin);
    if (type == "weeping_vines") return place_weeping_vines(env, config, random, origin);
    handled = false;
    return false;
}

}  // namespace pn::vg
