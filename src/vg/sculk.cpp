// Sculk patches: Mojang's SculkSpreader with its charge cursors, SculkBlock and SculkVeinBlock behaviours and SculkPatchFeature.
#include "multiface.hpp"
#include <algorithm>
#include <cmath>

namespace pn::vg {

namespace {

enum class Behaviour { Default, Sculk, Vein };

struct Cursor {
    BlockPos pos;
    int charge = 0;
    int update_delay = 0;
    int decay_delay = 1;
    bool has_facings = false;
    unsigned facings = 0;   // bit per Direction (EnumSet order)
};

struct Spread {
    FeatureEnv& env;
    Rng& random;
    const BlockPos origin;
    // SculkSpreader.createWorldGenSpreader()
    static constexpr int kGrowthSpawnCost = 50, kNoGrowthRadius = 1, kChargeDecayRate = 5, kAdditionalDecayRate = 10;
    std::vector<Cursor> cursors;

    Level& lv() { return env.level; }
    const BlockData& bd() { return env.bd(); }

    Behaviour behaviour_of(uint16_t s) {
        const std::string& n = bd().name(s);
        return n == "minecraft:sculk" ? Behaviour::Sculk : n == "minecraft:sculk_vein" ? Behaviour::Vein : Behaviour::Default;
    }
    bool has_any_face(uint16_t s) {
        for (int d = 0; d < 6; d++) if (mf_has_face(bd(), s, d)) return true;
        return false;
    }
    std::vector<int> all_shuffled() {
        std::vector<int> v{0, 1, 2, 3, 4, 5};
        for (int i = 6; i > 1; i--) { int swap = random.next_int(i); std::swap(v[size_t(i - 1)], v[size_t(swap)]); }
        return v;
    }
    bool fluid_here(const BlockPos& p) { return bd().fluid(lv().get(p)); }

    // SculkVeinBlock.regrow
    bool regrow(const BlockPos& pos, uint16_t existing, unsigned faces) {
        uint16_t ns = bd().default_state("minecraft:sculk_vein");
        bool any = false;
        for (int d = 0; d < 6; d++) {
            if (!(faces >> d & 1)) continue;
            if (mf_can_attach(env, d, pos + Dir(d))) { ns = bd().with(ns, kFaceNames[d], "true"); any = true; }
        }
        if (!any) return false;
        if (bd().fluid(existing)) ns = bd().with(ns, "waterlogged", "true");
        lv().set(pos, ns);
        return true;
    }

    // SculkVeinBlock.onDischarged
    void vein_discharged(const BlockPos& pos, uint16_t state) {
        if (bd().name(state) != "minecraft:sculk_vein") return;
        for (int d = 0; d < 6; d++)
            if (mf_has_face(bd(), state, d) && bd().name(lv().get(pos + Dir(d))) == "minecraft:sculk") state = bd().with(state, kFaceNames[d], "false");
        if (!has_any_face(state)) state = fluid_here(pos) ? bd().default_state("minecraft:water") : 0;
        lv().set(pos, state);
    }
    void on_discharged(Behaviour b, const BlockPos& pos, uint16_t state) { if (b == Behaviour::Vein) vein_discharged(pos, state); }

    bool attempt_spread_vein(Behaviour b, const BlockPos& pos, uint16_t state, const Cursor& c) {
        if (b == Behaviour::Default) {   // SculkBehaviour.DEFAULT
            if (!c.has_facings) return mf_spread_all(env, kVeinSameSpaceSpreader, lv().get(pos), pos) > 0;
            if (c.facings != 0) {
                bool src_water = bd().water(state) && (bd().flags(state) & BF_SOURCE);
                if (!bd().air(state) && !src_water) return false;
                return regrow(pos, state, c.facings);
            }
        }
        return mf_spread_all(env, kVeinSpreader, state, pos) > 0;
    }

    // SculkBlock
    bool can_place_growth(const BlockPos& pos) {
        uint16_t above = lv().get(pos.above());
        if (!(bd().air(above) || (bd().name(above) == "minecraft:water" && bd().water(above) && (bd().flags(above) & BF_SOURCE)))) return false;
        int count = 0;
        for (int z = pos.z - 4; z <= pos.z + 4; z++)
            for (int y = pos.y; y <= pos.y + 2; y++)
                for (int x = pos.x - 4; x <= pos.x + 4; x++) {
                    const std::string& n = bd().name(lv().get(x, y, z));
                    if (n == "minecraft:sculk_sensor" || n == "minecraft:sculk_shrieker") count++;
                    if (count > 2) return false;
                }
        return true;
    }
    int decay_penalty(const BlockPos& pos, int charge) {
        double dx = double(pos.x - origin.x), dy = double(pos.y - origin.y), dz = double(pos.z - origin.z);
        double dist_sqr = dx * dx + dy * dy + dz * dz;
        float outer = float(std::sqrt(dist_sqr)) - float(kNoGrowthRadius);
        outer = outer * outer;
        int reach = 24 - kNoGrowthRadius;
        float factor = std::min(1.0f, outer / float(reach * reach));
        return std::max(1, int(float(charge) * factor * 0.5f));
    }
    int sculk_use_charge(const Cursor& c) {
        int charge = c.charge;
        if (charge != 0 && random.next_int(kChargeDecayRate) == 0) {
            const BlockPos& cp = c.pos;
            double dx = double(cp.x - origin.x), dy = double(cp.y - origin.y), dz = double(cp.z - origin.z);
            bool close = dx * dx + dy * dy + dz * dz < double(kNoGrowthRadius) * double(kNoGrowthRadius);
            if (!close && can_place_growth(cp)) {
                if (random.next_int(kGrowthSpawnCost) < charge) {
                    BlockPos gp = cp.above();
                    uint16_t st;
                    if (random.next_int(11) == 0) st = bd().with(bd().default_state("minecraft:sculk_shrieker"), "can_summon", "true");
                    else st = bd().default_state("minecraft:sculk_sensor");
                    if (bd().has_prop(st, "waterlogged") && fluid_here(gp)) st = bd().with(st, "waterlogged", "true");
                    lv().set(gp, st);
                }
                return std::max(0, charge - kGrowthSpawnCost);
            }
            if (random.next_int(kAdditionalDecayRate) != 0) return charge;
            return charge - (close ? 1 : decay_penalty(cp, charge));
        }
        return charge;
    }

    // SculkVeinBlock.attemptUseCharge / attemptPlaceSculk
    bool vein_place_sculk(const BlockPos& pos) {
        uint16_t state = lv().get(pos);
        for (int support : all_shuffled()) {
            if (!mf_has_face(bd(), state, support)) continue;
            BlockPos sp = pos + Dir(support);
            if (!bd().in_tag(lv().get(sp), "minecraft:sculk_replaceable_world_gen")) continue;
            uint16_t sculk = bd().default_state("minecraft:sculk");
            lv().set(sp, sculk);
            mf_spread_all(env, kVeinSpreader, sculk, sp);
            int skip = kOpposite[support];
            for (int d = 0; d < 6; d++) {
                if (d == skip) continue;
                BlockPos vp = sp + Dir(d);
                uint16_t vs = lv().get(vp);
                if (bd().name(vs) == "minecraft:sculk_vein") vein_discharged(vp, vs);
            }
            return true;
        }
        return false;
    }
    int vein_use_charge(const Cursor& c, bool spread_veins) {
        if (spread_veins && vein_place_sculk(c.pos)) return c.charge - 1;
        return random.next_int(kChargeDecayRate) == 0 ? int(std::floor(float(c.charge) * 0.5f)) : c.charge;
    }

    int use_charge(Behaviour b, const Cursor& c, bool spread_veins) {
        if (b == Behaviour::Sculk) return sculk_use_charge(c);
        if (b == Behaviour::Vein) return vein_use_charge(c, spread_veins);
        return c.decay_delay > 0 ? c.charge : 0;
    }
    int update_decay_delay(Behaviour b, int age) { return b == Behaviour::Default ? std::max(age - 1, 0) : 1; }

    // ChargeCursor.getValidMovementPos
    bool is_unobstructed(const BlockPos& from, int dir) {
        BlockPos t = from + Dir(dir);
        return !bd().sturdy(lv().get(t), kOpposite[dir]);
    }
    bool movement_unobstructed(const BlockPos& from, const BlockPos& to) {
        int dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        if (std::abs(dx) + std::abs(dy) + std::abs(dz) == 1) return true;
        int dir_x = dx < 0 ? WEST : EAST, dir_y = dy < 0 ? DOWN : UP, dir_z = dz < 0 ? NORTH : SOUTH;
        if (dx == 0) return is_unobstructed(from, dir_y) || is_unobstructed(from, dir_z);
        if (dy == 0) return is_unobstructed(from, dir_x) || is_unobstructed(from, dir_z);
        return is_unobstructed(from, dir_x) || is_unobstructed(from, dir_y);
    }
    bool has_substrate_access(uint16_t s, const BlockPos& pos) {
        if (bd().name(s) != "minecraft:sculk_vein") return false;
        for (int d = 0; d < 6; d++)
            if (mf_has_face(bd(), s, d) && bd().in_tag(lv().get(pos + Dir(d)), "minecraft:sculk_replaceable")) return true;
        return false;
    }
    bool valid_movement_pos(const BlockPos& pos, BlockPos& out) {
        // NON_CORNER_NEIGHBOURS: betweenClosed(-1..1) with at least one zero coordinate, not the origin
        static const std::vector<BlockPos> offsets = [] {
            std::vector<BlockPos> v;
            for (int z = -1; z <= 1; z++)
                for (int y = -1; y <= 1; y++)
                    for (int x = -1; x <= 1; x++)
                        if ((x == 0 || y == 0 || z == 0) && !(x == 0 && y == 0 && z == 0)) v.emplace_back(x, y, z);
            return v;
        }();
        std::vector<BlockPos> shuffled = offsets;
        for (int i = int(shuffled.size()); i > 1; i--) { int swap = random.next_int(i); std::swap(shuffled[size_t(i - 1)], shuffled[size_t(swap)]); }
        BlockPos result = pos;
        for (const BlockPos& o : shuffled) {
            BlockPos n = pos.offset(o.x, o.y, o.z);
            uint16_t t = lv().get(n);
            if (behaviour_of(t) != Behaviour::Default && movement_unobstructed(pos, n)) {
                result = n;
                if (has_substrate_access(t, n)) break;
            }
        }
        if (result == pos) return false;
        out = result;
        return true;
    }

    unsigned available_faces(uint16_t s) {
        unsigned m = 0;
        if (bd().name(s) == "minecraft:sculk_vein")
            for (int d = 0; d < 6; d++) if (mf_has_face(bd(), s, d)) m |= 1u << d;
        return m;
    }

    // ChargeCursor.update
    void update_cursor(Cursor& c, bool spread_veins) {
        if (c.charge <= 0) return;   // world generation always updates
        if (c.update_delay > 0) { c.update_delay--; return; }
        uint16_t state = lv().get(c.pos);
        Behaviour b = behaviour_of(state);
        if (spread_veins && attempt_spread_vein(b, c.pos, state, c)) {
            if (b != Behaviour::Sculk) {   // canChangeBlockStateOnSpread
                state = lv().get(c.pos);
                b = behaviour_of(state);
            }
        }
        c.charge = use_charge(b, c, spread_veins);
        if (c.charge <= 0) { on_discharged(b, c.pos, state); return; }
        BlockPos transfer;
        if (valid_movement_pos(c.pos, transfer)) {
            on_discharged(b, c.pos, state);
            c.pos = transfer;
            int dx = c.pos.x - origin.x, dz = c.pos.z - origin.z;
            if (!(double(dx * dx + dz * dz) < 15.0 * 15.0)) { c.charge = 0; return; }
            state = lv().get(transfer);
        }
        if (behaviour_of(state) != Behaviour::Default) { c.has_facings = true; c.facings = available_faces(state); }
        c.decay_delay = update_decay_delay(b, c.decay_delay);
        c.update_delay = 1;   // getSculkSpreadDelay
    }

    void add_cursors(int charge) {
        while (charge > 0) {
            int cur = std::min(charge, 1000);
            if (cursors.size() < 32) { Cursor c; c.pos = origin; c.charge = cur; cursors.push_back(c); }
            charge -= cur;
        }
    }
    // SculkSpreader.updateCursors (world generation never merges cursors)
    void update_cursors(bool spread_veins) {
        if (cursors.empty()) return;
        std::vector<Cursor> processed;
        for (Cursor& c : cursors) {
            if (std::max({std::abs(c.pos.x - origin.x), std::abs(c.pos.y - origin.y), std::abs(c.pos.z - origin.z)}) > 1024) continue;
            update_cursor(c, spread_veins);
            if (c.charge <= 0) continue;
            processed.push_back(c);
        }
        cursors = std::move(processed);
    }
};

}  // namespace

bool place_sculk_patch(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) {
    Level& lv = env.level;
    const BlockData& bd = env.bd();
    // canSpreadFrom
    uint16_t start = lv.get(origin);
    const std::string& sn = bd.name(start);
    if (sn != "minecraft:sculk" && sn != "minecraft:sculk_vein") {
        if (!(bd.air(start) || (bd.water(start) && (bd.flags(start) & BF_SOURCE)))) return false;
        bool full = false;
        for (int d = 0; d < 6 && !full; d++) full = bd.flags(lv.get(origin + Dir(d))) & BF_FULL_COLLISION;
        if (!full) return false;
    }
    int spread_rounds = int(config.at("spread_rounds").int_value()), growth_rounds = int(config.at("growth_rounds").int_value());
    int charge_count = int(config.at("charge_count").int_value()), amount = int(config.at("amount_per_charge").int_value());
    int attempts = int(config.at("spread_attempts").int_value());
    Spread sp{env, random, origin, {}};
    for (int round = 0; round < spread_rounds + growth_rounds; round++) {
        for (int i = 0; i < charge_count; i++) sp.add_cursors(amount);
        bool veins = round < spread_rounds;
        for (int i = 0; i < attempts; i++) sp.update_cursors(veins);
        sp.cursors.clear();
    }
    BlockPos below = origin.below();
    // 26.3 moved the catalyst and the shriekers out of the feature into the features that follow it in a sequence
    if (config.has("catalyst_chance") && random.next_float() <= config.at("catalyst_chance").float_value() && (bd.flags(lv.get(below)) & BF_FULL_COLLISION))
        lv.set(origin, bd.default_state("minecraft:sculk_catalyst"));
    int extra = config.has("extra_rare_growths") ? sample_int(config.at("extra_rare_growths"), random) : 0;
    for (int i = 0; i < extra; i++) {
        int dx = random.next_int(5) - 2;
        int dz = random.next_int(5) - 2;
        BlockPos cand = origin.offset(dx, 0, dz);
        if (bd.air(lv.get(cand)) && bd.sturdy(lv.get(cand.below()), UP))
            lv.set(cand, bd.with(bd.default_state("minecraft:sculk_shrieker"), "can_summon", "true"));
    }
    return true;
}

}  // namespace pn::vg
