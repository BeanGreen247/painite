// MultifaceSpreader and MultifaceGrowthFeature (glow lichen, sculk veins).
#include "multiface.hpp"
#include <algorithm>
#include <cstring>

namespace pn::vg {

const Spreader kLichenSpreader{"minecraft:glow_lichen", false, {SAME_POSITION, SAME_PLANE, WRAP_AROUND}};
const Spreader kVeinSpreader{"minecraft:sculk_vein", true, {SAME_POSITION, SAME_PLANE, WRAP_AROUND}};
const Spreader kVeinSameSpaceSpreader{"minecraft:sculk_vein", true, {SAME_POSITION}};

bool mf_can_attach(FeatureEnv& env, int dir_to_neighbour, const BlockPos& neighbour) {
    const BlockData& bd = env.bd();
    uint16_t s = env.level.get(neighbour);
    int face = kOpposite[dir_to_neighbour];
    return bd.sturdy(s, face) || bd.collision_face_full(s, face);
}
bool mf_has_face(const BlockData& bd, uint16_t s, int d) { return bd.has_prop(s, kFaceNames[d]) && bd.prop(s, kFaceNames[d]) == "true"; }

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

namespace {

bool is_source_water(const BlockData& bd, uint16_t s) { return bd.water(s) && (bd.flags(s) & BF_SOURCE); }

// DefaultSpreaderConfig.stateCanBeReplaced
bool default_replaceable(const BlockData& bd, const Spreader& sp, uint16_t existing) {
    return bd.air(existing) || bd.name(existing) == sp.block || (bd.name(existing) == "minecraft:water" && (bd.flags(existing) & BF_SOURCE));
}

// SculkVeinSpreaderConfig.stateCanBeReplaced
bool vein_replaceable(FeatureEnv& env, const Spreader& sp, const BlockPos& src, const BlockPos& placement, int dir, uint16_t existing) {
    const BlockData& bd = env.bd();
    const std::string& against = bd.name(env.level.get(placement + Dir(dir)));
    if (against == "minecraft:sculk" || against == "minecraft:sculk_catalyst" || against == "minecraft:moving_piston") return false;
    int manhattan = std::abs(src.x - placement.x) + std::abs(src.y - placement.y) + std::abs(src.z - placement.z);
    if (manhattan == 2) {
        BlockPos neighbour = src + kOpposite[dir];
        if (bd.sturdy(env.level.get(neighbour), dir)) return false;
    }
    if (bd.fluid(existing) && !is_source_water(bd, existing)) return false;
    if (bd.in_tag(existing, "minecraft:fire")) return false;
    return bd.replaceable(existing) || default_replaceable(bd, sp, existing);
}

bool can_spread_into(FeatureEnv& env, const Spreader& sp, const BlockPos& src, const SpreadPos& s) {
    const BlockData& bd = env.bd();
    uint16_t existing = env.level.get(s.pos);
    bool replaceable = sp.sculk ? vein_replaceable(env, sp, src, s.pos, s.face, existing) : default_replaceable(bd, sp, existing);
    if (!replaceable) return false;
    // MultifaceBlock.isValidStateForPlacement
    if (bd.name(existing) == sp.block && mf_has_face(bd, existing, s.face)) return false;
    return mf_can_attach(env, s.face, s.pos + Dir(s.face));
}

// MultifaceSpreader.getSpreadFromFaceTowardDirection: the first spread type that fits
bool spread_pos(FeatureEnv& env, const Spreader& sp, uint16_t state, const BlockPos& pos, int from_face, int dir, SpreadPos& out) {
    const BlockData& bd = env.bd();
    if ((dir >> 1) == (from_face >> 1)) return false;
    bool other_source = sp.sculk && bd.name(state) != "minecraft:sculk_vein";   // isOtherBlockValidAsSource
    if (!(other_source || (mf_has_face(bd, state, from_face) && !mf_has_face(bd, state, dir)))) return false;
    for (SpreadType type : sp.types) {
        SpreadPos s;
        if (type == SAME_POSITION) s = {pos, dir};
        else if (type == SAME_PLANE) s = {pos + Dir(dir), from_face};
        else s = {(pos + Dir(dir)) + Dir(from_face), int(kOpposite[dir])};
        if (can_spread_into(env, sp, pos, s)) { out = s; return true; }
    }
    return false;
}

// MultifaceSpreader.spreadToFace
bool spread_to_face(FeatureEnv& env, const Spreader& sp, const SpreadPos& s) {
    uint16_t ns = mf_state_for_placement(env, sp.block, env.level.get(s.pos), s.pos, s.face);
    return ns != kNoState && env.level.set(s.pos, ns);
}

}  // namespace

bool mf_spread_toward_random(FeatureEnv& env, const Spreader& sp, uint16_t state, const BlockPos& pos, int starting_face, Rng& random) {
    int dirs[6] = {0, 1, 2, 3, 4, 5};
    for (int i = 6; i > 1; i--) { int swap = random.next_int(i); std::swap(dirs[i - 1], dirs[swap]); }
    for (int d : dirs) {
        SpreadPos s;
        if (spread_pos(env, sp, state, pos, starting_face, d, s) && spread_to_face(env, sp, s)) return true;
    }
    return false;
}

int mf_spread_all(FeatureEnv& env, const Spreader& sp, uint16_t state, const BlockPos& pos) {
    const BlockData& bd = env.bd();
    bool other_source = sp.sculk && bd.name(state) != "minecraft:sculk_vein";
    int total = 0;
    for (int face = 0; face < 6; face++) {
        if (!(other_source || mf_has_face(bd, state, face))) continue;
        for (int d = 0; d < 6; d++) {
            SpreadPos s;
            if (spread_pos(env, sp, state, pos, face, d, s) && spread_to_face(env, sp, s)) total++;
        }
    }
    return total;
}

bool place_multiface(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    std::string block = config.at("block").str;
    const Spreader& spreader = block == "minecraft:sculk_vein" ? kVeinSpreader : kLichenSpreader;
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
                if (random.next_float() < chance) mf_spread_toward_random(env, spreader, ns, pos, d, random);
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

}  // namespace pn::vg
