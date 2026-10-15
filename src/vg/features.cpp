// The simple feature types: ores, disks, single blocks and the selectors that choose between placed features.
#include "feature.hpp"
#include "mathutil.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>

namespace pn::vg {

namespace {

inline int java_round(float v) { return int(std::floor(v + 0.5f)); }   // Math.round(float)

uint16_t target_state(FeatureEnv& env, const Json& t) { return env.bd().state(t.at("state")); }

// OreFeature.canPlaceOre
bool can_place_ore(FeatureEnv& env, uint16_t at_state, Rng& random, const Json& config, const Json& target, const BlockPos& pos) {
    if (!rule_test(env, target.at("target"), at_state, random)) return false;
    float discard = config.at("discard_chance_on_air_exposure").float_value();
    bool skip_air_check;
    if (discard <= 0.0f) skip_air_check = true;
    else if (discard >= 1.0f) skip_air_check = false;
    else skip_air_check = random.next_float() >= discard;
    if (skip_air_check) return true;
    for (int d = 0; d < 6; d++)
        if (env.bd().air(env.level.get(pos + Dir(d)))) return false;
    return true;
}

bool place_ore(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const int size = int(config.at("size").int_value());
    float dir = random.next_float() * 3.14159274f;
    float spread_xy = float(size) / 8.0f;
    int max_radius = int(std::ceil((float(size) / 16.0f * 2.0f + 1.0f) / 2.0f));
    double x0 = origin.x + std::sin(double(dir)) * double(spread_xy);
    double x1 = origin.x - std::sin(double(dir)) * double(spread_xy);
    double z0 = origin.z + std::cos(double(dir)) * double(spread_xy);
    double z1 = origin.z - std::cos(double(dir)) * double(spread_xy);
    int a = random.next_int(3);
    double y0 = double(origin.y + a - 2);
    int b = random.next_int(3);
    double y1 = double(origin.y + b - 2);
    int ceil_spread = int(std::ceil(double(spread_xy)));
    int x_start = origin.x - ceil_spread - max_radius;
    int y_start = origin.y - 2 - max_radius;
    int z_start = origin.z - ceil_spread - max_radius;
    int size_xz = 2 * (ceil_spread + max_radius);
    int size_y = 2 * (2 + max_radius);
    for (int xp = x_start; xp <= x_start + size_xz; xp++)
        for (int zp = z_start; zp <= z_start + size_xz; zp++)
            if (y_start <= lv.height(OCEAN_FLOOR_WG, xp, zp)) goto do_place;
    if (std::getenv("PAINITE_TRACE")) { int mx = -9999; for (int xp = x_start; xp <= x_start + size_xz; xp++) for (int zp = z_start; zp <= z_start + size_xz; zp++) mx = std::max(mx, lv.height(OCEAN_FLOOR_WG, xp, zp)); fprintf(stderr, "ore-skip at %d %d %d y_start %d maxh %d\n", origin.x, origin.y, origin.z, y_start, mx); }
    return false;
do_place:
    int placed = 0;
    std::vector<uint8_t> tested(size_t(size_xz) * size_t(size_y) * size_t(size_xz), 0);
    std::vector<double> data(size_t(size) * 4);
    for (int i = 0; i < size; i++) {
        float step = float(i) / float(size);
        double xx = lerp(double(step), x0, x1), yy = lerp(double(step), y0, y1), zz = lerp(double(step), z0, z1);
        double ss = random.next_double() * double(size) / 16.0;
        double r = (double(mth_sin(double(kPiF * step)) + 1.0f) * ss + 1.0) / 2.0;
        data[size_t(i) * 4 + 0] = xx; data[size_t(i) * 4 + 1] = yy; data[size_t(i) * 4 + 2] = zz; data[size_t(i) * 4 + 3] = r;
    }
    for (int i1 = 0; i1 < size - 1; i1++) {
        if (data[size_t(i1) * 4 + 3] <= 0.0) continue;
        for (int i2 = i1 + 1; i2 < size; i2++) {
            if (data[size_t(i2) * 4 + 3] <= 0.0) continue;
            double dx = data[size_t(i1) * 4 + 0] - data[size_t(i2) * 4 + 0], dy = data[size_t(i1) * 4 + 1] - data[size_t(i2) * 4 + 1], dz = data[size_t(i1) * 4 + 2] - data[size_t(i2) * 4 + 2];
            double dr = data[size_t(i1) * 4 + 3] - data[size_t(i2) * 4 + 3];
            if (dr * dr > dx * dx + dy * dy + dz * dz) {
                if (dr > 0.0) data[size_t(i2) * 4 + 3] = -1.0;
                else data[size_t(i1) * 4 + 3] = -1.0;
            }
        }
    }
    const Json& targets = config.at("targets");
    for (int i = 0; i < size; i++) {
        double r = data[size_t(i) * 4 + 3];
        if (r < 0.0) continue;
        double xx = data[size_t(i) * 4 + 0], yy = data[size_t(i) * 4 + 1], zz = data[size_t(i) * 4 + 2];
        int x_min = std::max(mth_floor(xx - r), x_start), y_min = std::max(mth_floor(yy - r), y_start), z_min = std::max(mth_floor(zz - r), z_start);
        int x_max = std::max(mth_floor(xx + r), x_min), y_max = std::max(mth_floor(yy + r), y_min), z_max = std::max(mth_floor(zz + r), z_min);
        for (int x = x_min; x <= x_max; x++) {
            double xd = (x + 0.5 - xx) / r;
            if (!(xd * xd < 1.0)) continue;
            for (int y = y_min; y <= y_max; y++) {
                double yd = (y + 0.5 - yy) / r;
                if (!(xd * xd + yd * yd < 1.0)) continue;
                for (int z = z_min; z <= z_max; z++) {
                    double zd = (z + 0.5 - zz) / r;
                    if (!(xd * xd + yd * yd + zd * zd < 1.0) || lv.outside_build_height(y)) continue;
                    size_t bit = size_t(x - x_start) + size_t(y - y_start) * size_t(size_xz) + size_t(z - z_start) * size_t(size_xz) * size_t(size_y);
                    if (tested[bit]) continue;
                    tested[bit] = 1;
                    BlockPos p(x, y, z);
                    if (!lv.can_write(p)) continue;
                    uint16_t cur = lv.get(p);
                    for (auto& t : targets.arr) {
                        if (can_place_ore(env, cur, random, config, t, p)) {
                            lv.set_raw(x, y, z, target_state(env, t));
                            if (std::getenv("PAINITE_CELLS") && env.top_feature && env.top_feature->find(std::getenv("PAINITE_CELLS")) != std::string::npos) fprintf(stderr, "vcell %d %d %d origin %d %d %d\n", x, y, z, origin.x, origin.y, origin.z);
                            placed++;
                            break;
                        }
                    }
                }
            }
        }
    }
    return placed > 0;
}

bool place_scattered_ore(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    int tries = random.next_int(int(config.at("size").int_value()) + 1);
    const Json& targets = config.at("targets");
    auto axis = [&](int max_dist) {
        float a = random.next_float();
        float b = random.next_float();
        return java_round((a - b) * float(max_dist));
    };
    for (int i = 0; i < tries; i++) {
        int md = std::min(i, 7);
        int xd = axis(md);
        int yd = axis(md);
        int zd = axis(md);
        BlockPos p = origin.offset(xd, yd, zd);
        uint16_t cur = env.level.get(p);
        for (auto& t : targets.arr) {
            if (can_place_ore(env, cur, random, config, t, p)) {
                env.level.set(p, target_state(env, t));
                break;
            }
        }
    }
    return true;
}

bool place_replace_single_block(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    for (auto& t : config.at("targets").arr) {
        if (rule_test(env, t.at("target"), env.level.get(origin), random)) {
            env.level.set(origin, target_state(env, t));
            break;
        }
    }
    return true;
}

// Feature.markAboveForPostProcessing is bookkeeping for fluids only: nothing to do for block output

bool place_disk(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    int half_height = int(config.at("half_height").int_value());
    int top = origin.y + half_height, bottom = origin.y - half_height - 1;
    int r = sample_int(config.at("radius"), random);
    bool placed_any = false;
    for (int z = origin.z - r; z <= origin.z + r; z++)
        for (int x = origin.x - r; x <= origin.x + r; x++) {
            int xd = x - origin.x, zd = z - origin.z;
            if (xd * xd + zd * zd > r * r) continue;
            for (int y = top; y > bottom; y--) {
                BlockPos p(x, y, z);
                if (test_predicate(env, config.at("target"), p)) {
                    uint16_t st = provide_state(env, config.at("state_provider"), random, p);
                    if (st != kNoState) { lv.set(p, st); placed_any = true; }
                }
            }
        }
    return placed_any;
}

bool place_simple_block(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    uint16_t st = provide_state(env, config.at("to_place"), random, origin);
    if (st == kNoState) return false;
    if (!state_can_survive(env, st, origin)) return false;
    const auto& cls = bd.classes(st);
    if (std::find(cls.begin(), cls.end(), "DoublePlantBlock") != cls.end()) {
        if (!bd.air(lv.get(origin.above()))) return false;
        // DoublePlantBlock.placeAt: lower half (waterlogged when the spot holds water) and upper half
        uint16_t lower = bd.with(st, "half", "lower"), upper = bd.with(st, "half", "upper");
        lv.set(origin, lower);
        lv.set(origin.above(), upper);
    } else if (!cls.empty() && cls[0] == "MossyCarpetBlock") {
        throw std::runtime_error("mossy carpet placement is not ported yet");
    } else {
        lv.set(origin, st);
    }
    return true;
}

bool place_placed_ref(FeatureEnv& env, const Json& ref, Rng& random, const BlockPos& origin) {
    return place_placed_feature(env, resolve_ref(env, "placed_feature", ref), random, origin, nullptr);
}

}  // namespace

bool place_feature_type(FeatureEnv& env, const std::string& type, const Json& config, Rng& random, const BlockPos& origin) {
    if (type == "ore") return place_ore(env, config, random, origin);
    if (type == "scattered_ore") return place_scattered_ore(env, config, random, origin);
    if (type == "replace_single_block") return place_replace_single_block(env, config, random, origin);
    if (type == "disk") return place_disk(env, config, random, origin);
    if (type == "simple_block") return place_simple_block(env, config, random, origin);
    if (type == "no_op") return true;
    if (type == "fallen_tree") return place_fallen_tree_feature(env, config, random, origin);
    if (type == "tree") return place_tree_feature(env, config, random, origin);
    if (type == "random_selector") {
        for (auto& f : config.at("features").arr) {
            if (random.next_float() < f.at("chance").float_value()) return place_placed_ref(env, f.at("feature"), random, origin);
        }
        return place_placed_ref(env, config.at("default"), random, origin);
    }
    if (type == "simple_random_selector") {
        const Json& fs = config.at("features");
        int index = random.next_int(int(fs.arr.size()));
        return place_placed_ref(env, fs.arr[size_t(index)], random, origin);
    }
    if (type == "random_boolean_selector") {
        bool r = random.next_boolean();
        return place_placed_ref(env, config.at(r ? "feature_true" : "feature_false"), random, origin);
    }
    if (type == "weighted_random_selector") {
        const Json& fs = config.at("features");
        int total = 0;
        for (auto& e : fs.arr) total += int(e.get_int("weight", 1));
        if (total == 0) return false;
        int sel = random.next_int(total);
        for (auto& e : fs.arr) {
            sel -= int(e.get_int("weight", 1));
            if (sel < 0) return place_placed_ref(env, e.at("data"), random, origin);
        }
        return false;
    }
    if (type == "sequence") {
        for (auto& f : config.at("features").arr)
            if (!place_placed_ref(env, f, random, origin)) return false;
        return true;
    }
    {
        bool handled;
        bool r = place_feature_type2(env, type, config, random, origin, handled);
        if (handled) return r;
        r = place_feature_type3(env, type, config, random, origin, handled);
        if (handled) return r;
        r = place_feature_type4(env, type, config, random, origin, handled);
        if (handled) return r;
    }
    if (std::getenv("PAINITE_LENIENT")) {   // development: list the types that are still missing instead of stopping
        static std::mutex mu;
        static std::map<std::string, int> missing;
        static bool registered = false;
        std::lock_guard<std::mutex> l(mu);
        if (!registered) { registered = true; atexit([] { for (auto& kv : missing) fprintf(stderr, "not ported: %s (%d placements)\n", kv.first.c_str(), kv.second); }); }
        missing[type]++;
        return false;
    }
    throw std::runtime_error("feature type not ported yet: " + type);
}

}  // namespace pn::vg
