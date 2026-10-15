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

}  // namespace

bool place_feature_type4(FeatureEnv& env, const std::string& type, const Json& config, Rng& random, const BlockPos& origin, bool& handled) {
    handled = true;
    if (type == "huge_fungus") return place_huge_fungus(env, config, random, origin);
    if (type == "end_spike") return place_end_spike(env, config, random, origin);
    if (type == "end_platform") return place_end_platform(env, config, random, origin);
    if (type == "end_island") return place_end_island(env, config, random, origin);
    if (type == "glowstone_blob") return place_glowstone(env, config, random, origin);
    if (type == "nether_forest_vegetation") return place_nether_forest_vegetation(env, config, random, origin);
    if (type == "twisting_vines") return place_twisting_vines(env, config, random, origin);
    if (type == "weeping_vines") return place_weeping_vines(env, config, random, origin);
    handled = false;
    return false;
}

}  // namespace pn::vg
