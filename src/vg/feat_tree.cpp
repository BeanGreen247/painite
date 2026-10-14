// Mojang's TreeFeature: trunk placers, foliage placers, root placers and tree decorators.
#include "feature.hpp"
#include "jhash.hpp"
#include "mathutil.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace pn::vg {

namespace {

constexpr Dir kHorizontal[4] = {NORTH, EAST, SOUTH, WEST};   // Direction.Plane.HORIZONTAL
inline Dir opposite(Dir d) { static const Dir o[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST}; return o[d]; }
inline Dir clockwise(Dir d) { switch (d) { case NORTH: return EAST; case EAST: return SOUTH; case SOUTH: return WEST; default: return NORTH; } }
inline BlockPos rel(const BlockPos& p, Dir d, int n = 1) { return {p.x + kDirX[d] * n, p.y + kDirY[d] * n, p.z + kDirZ[d] * n}; }
inline int manhattan(const BlockPos& a, const BlockPos& b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z); }
inline int mth_floor_f(float v) { int i = int(v); return v < float(i) ? i - 1 : i; }
inline int mth_floor_d(double v) { int i = int(v); return v < double(i) ? i - 1 : i; }

std::string short_type(const Json& j) {
    const std::string& t = j.at("type").str;
    return t.rfind("minecraft:", 0) == 0 ? t.substr(10) : t;
}

struct Attach {
    BlockPos pos;
    int radius_offset;
    bool double_trunk;
};

struct TreeCtx {
    FeatureEnv& env;
    Level& lv;
    Rng& random;
    const Json& cfg;
    JHashSet roots, trunks, foliage, decorations;
    const BlockData& bd;
    TreeCtx(FeatureEnv& e, Rng& r, const Json& c) : env(e), lv(e.level), random(r), cfg(c), bd(e.bd()) {}

    uint16_t get(const BlockPos& p) const { return lv.get(p); }
    bool is_air(const BlockPos& p) const { return bd.air(get(p)); }
    bool valid_tree_pos(const BlockPos& p) const { uint16_t s = get(p); return bd.air(s) || bd.in_tag(s, "minecraft:replaceable_by_trees"); }
    bool air_or_leaves(const BlockPos& p) const { uint16_t s = get(p); return bd.air(s) || bd.in_tag(s, "minecraft:leaves"); }
    bool is_vine(const BlockPos& p) const { return bd.name(get(p)) == "minecraft:vine"; }
    void set_trunk(const BlockPos& p, uint16_t s) { trunks.add(p); lv.set(p, s); }
    void set_root(const BlockPos& p, uint16_t s) { roots.add(p); lv.set(p, s); }
    void set_foliage(const BlockPos& p, uint16_t s) { foliage.add(p); lv.set(p, s); }
    void set_decoration(const BlockPos& p, uint16_t s) { decorations.add(p); lv.set(p, s); }
};

// ---------------------------------------------------------------- trunk placers

struct Trunk {
    std::string type;
    const Json& j;
    int base, ra, rb;
    explicit Trunk(const Json& tj) : type(short_type(tj)), j(tj), base(int(tj.at("base_height").int_value())), ra(int(tj.at("height_rand_a").int_value())), rb(int(tj.at("height_rand_b").int_value())) {}

    int tree_height(Rng& r) const {
        int a = r.next_int(ra + 1);
        int b = r.next_int(rb + 1);
        return base + a + b;
    }
    bool valid_tree_pos(TreeCtx& t, const BlockPos& p) const {
        if (t.valid_tree_pos(p)) return true;
        if (type == "upwards_branching_trunk_placer") return state_in_set(t.env, t.get(p), j.at("can_grow_through"));
        return false;
    }
    bool is_free(TreeCtx& t, const BlockPos& p) const { return valid_tree_pos(t, p) || t.bd.in_tag(t.get(p), "minecraft:logs"); }

    void place_below(TreeCtx& t, const BlockPos& p) const {
        const Json* prov = t.cfg.find("below_trunk_provider");
        if (!prov) return;
        uint16_t s = provide_state(t.env, *prov, t.random, p);
        if (s != kNoState) t.set_trunk(p, s);
    }
    // modifier: axis override (0 none, 'x','y','z') or a direction axis letter
    bool place_log(TreeCtx& t, const BlockPos& p, char axis = 0) const {
        if (!valid_tree_pos(t, p)) return false;
        uint16_t s = provide_state(t.env, t.cfg.at("trunk_provider"), t.random, p);
        if (axis && t.bd.has_prop(s, "axis")) s = t.bd.with(s, "axis", std::string(1, axis));
        t.set_trunk(p, s);
        return true;
    }
    void place_log_if_free(TreeCtx& t, const BlockPos& p) const { if (is_free(t, p)) place_log(t, p); }

    std::vector<Attach> place(TreeCtx& t, int tree_height, const BlockPos& origin) const;
    // pieces shared by several placers
    std::vector<Attach> straight(TreeCtx& t, int h, const BlockPos& o) const {
        place_below(t, o.below());
        for (int y = 0; y < h; y++) place_log(t, o.above(y));
        return {Attach{o.above(h), 0, false}};
    }
    std::vector<Attach> giant(TreeCtx& t, int h, const BlockPos& o) const {
        BlockPos below = o.below();
        place_below(t, below);
        place_below(t, rel(below, EAST));
        place_below(t, rel(below, SOUTH));
        place_below(t, rel(rel(below, SOUTH), EAST));
        for (int hh = 0; hh < h; hh++) {
            place_log_if_free(t, o.offset(0, hh, 0));
            if (hh < h - 1) {
                place_log_if_free(t, o.offset(1, hh, 0));
                place_log_if_free(t, o.offset(1, hh, 1));
                place_log_if_free(t, o.offset(0, hh, 1));
            }
        }
        return {Attach{o.above(h), 0, true}};
    }
    std::vector<Attach> mega_jungle(TreeCtx& t, int h, const BlockPos& o) const {
        std::vector<Attach> out = giant(t, h, o);
        int first = t.random.next_int(4);
        for (int branch_height = h - 2 - first; branch_height > h / 2;) {
            float angle = t.random.next_float() * (kPiF * 2.0f);
            int bx = 0, bz = 0;
            for (int b = 0; b < 5; b++) {
                bx = int(1.5f + mth_cos(double(angle)) * float(b));
                bz = int(1.5f + mth_sin(double(angle)) * float(b));
                place_log(t, o.offset(bx, branch_height - 3 + b / 2, bz));
            }
            out.push_back(Attach{o.offset(bx, branch_height, bz), -2, false});
            int step = t.random.next_int(4);
            branch_height -= 2 + step;
        }
        return out;
    }
    static float tree_shape(int height, int y) {
        if (float(y) < float(height) * 0.3f) return -1.0f;
        float radius = float(height) / 2.0f;
        float adjacent = radius - float(y);
        float distance = float(std::sqrt(double(radius * radius - adjacent * adjacent)));
        if (adjacent == 0.0f) distance = radius;
        else if (std::fabs(adjacent) >= radius) return 0.0f;
        return distance * 0.5f;
    }
    bool make_limb(TreeCtx& t, const BlockPos& start, const BlockPos& end, bool do_place) const {
        if (!do_place && start == end) return true;
        BlockPos delta(end.x - start.x, end.y - start.y, end.z - start.z);
        int steps = std::max(std::abs(delta.x), std::max(std::abs(delta.y), std::abs(delta.z)));
        float dx = float(delta.x) / float(steps), dy = float(delta.y) / float(steps), dz = float(delta.z) / float(steps);
        for (int i = 0; i <= steps; i++) {
            BlockPos p = start.offset(mth_floor_f(0.5f + float(i) * dx), mth_floor_f(0.5f + float(i) * dy), mth_floor_f(0.5f + float(i) * dz));
            if (do_place) {
                char axis = 'y';
                int xd = std::abs(p.x - start.x), zd = std::abs(p.z - start.z), m = std::max(xd, zd);
                if (m > 0) axis = xd == m ? 'x' : 'z';
                place_log(t, p, axis);
            } else if (!is_free(t, p)) return false;
        }
        return true;
    }
    std::vector<Attach> fancy(TreeCtx& t, int tree_height, const BlockPos& o) const {
        int height = tree_height + 2;
        int trunk_height = mth_floor_d(double(height) * 0.618);
        place_below(t, o.below());
        int clusters_per_y = std::min(1, mth_floor_d(1.382 + std::pow(1.0 * double(height) / 13.0, 2.0)));
        int trunk_top = o.y + trunk_height;
        int relative_y = height - 5;
        struct Coords { BlockPos pos; int branch_base; };
        std::vector<Coords> coords;
        coords.push_back({o.above(relative_y), trunk_top});
        for (; relative_y >= 0; relative_y--) {
            float shape = tree_shape(height, relative_y);
            if (shape < 0.0f) continue;
            for (int i = 0; i < clusters_per_y; i++) {
                double radius = 1.0 * double(shape) * (double(t.random.next_float()) + 0.328);
                double angle = double(t.random.next_float() * 2.0f) * 3.141592653589793;
                double x = radius * std::sin(angle) + 0.5;
                double z = radius * std::cos(angle) + 0.5;
                BlockPos check_start = o.offset(mth_floor_d(x), relative_y - 1, mth_floor_d(z));
                BlockPos check_end = check_start.above(5);
                if (make_limb(t, check_start, check_end, false)) {
                    int dx = o.x - check_start.x, dz = o.z - check_start.z;
                    double branch_height = double(check_start.y) - std::sqrt(double(dx * dx + dz * dz)) * 0.381;
                    int branch_top = branch_height > double(trunk_top) ? trunk_top : int(branch_height);
                    BlockPos base(o.x, branch_top, o.z);
                    if (make_limb(t, base, check_start, false)) coords.push_back({check_start, base.y});
                }
            }
        }
        make_limb(t, o, o.above(trunk_height), true);
        // makeBranches
        for (auto& c : coords) {
            BlockPos base(o.x, c.branch_base, o.z);
            if (base != c.pos && double(c.branch_base - o.y) >= double(height) * 0.2) make_limb(t, base, c.pos, true);
        }
        std::vector<Attach> out;
        for (auto& c : coords)
            if (double(c.branch_base - o.y) >= double(height) * 0.2) out.push_back(Attach{c.pos, 0, false});
        return out;
    }
    std::vector<Attach> forking(TreeCtx& t, int h, const BlockPos& o) const {
        place_below(t, o.below());
        std::vector<Attach> out;
        Dir lean = kHorizontal[t.random.next_int(4)];
        int lean_height = h - t.random.next_int(4) - 1;
        int lean_steps = 3 - t.random.next_int(3);
        int tx = o.x, tz = o.z;
        int ey = INT32_MIN;
        bool have = false;
        for (int yo = 0; yo < h; yo++) {
            int yy = o.y + yo;
            if (yo >= lean_height && lean_steps > 0) { tx += kDirX[lean]; tz += kDirZ[lean]; lean_steps--; }
            if (place_log(t, BlockPos(tx, yy, tz))) { ey = yy + 1; have = true; }
        }
        if (have) out.push_back(Attach{BlockPos(tx, ey, tz), 1, false});
        tx = o.x; tz = o.z;
        Dir branch = kHorizontal[t.random.next_int(4)];
        if (branch != lean) {
            int branch_pos = lean_height - t.random.next_int(2) - 1;
            int branch_steps = 1 + t.random.next_int(3);
            have = false;
            for (int yo = branch_pos; yo < h && branch_steps > 0; branch_steps--) {
                if (yo >= 1) {
                    int yy = o.y + yo;
                    tx += kDirX[branch]; tz += kDirZ[branch];
                    if (place_log(t, BlockPos(tx, yy, tz))) { ey = yy + 1; have = true; }
                }
                yo++;
            }
            if (have) out.push_back(Attach{BlockPos(tx, ey, tz), 0, false});
        }
        return out;
    }
    std::vector<Attach> dark_oak(TreeCtx& t, int h, const BlockPos& o) const {
        std::vector<Attach> out;
        BlockPos below = o.below();
        place_below(t, below);
        place_below(t, rel(below, EAST));
        place_below(t, rel(below, SOUTH));
        place_below(t, rel(rel(below, SOUTH), EAST));
        Dir lean = kHorizontal[t.random.next_int(4)];
        int lean_height = h - t.random.next_int(4);
        int lean_steps = 2 - t.random.next_int(3);
        int x = o.x, y = o.y, z = o.z, tx = x, tz = z, ey = y + h - 1;
        for (int dy = 0; dy < h; dy++) {
            if (dy >= lean_height && lean_steps > 0) { tx += kDirX[lean]; tz += kDirZ[lean]; lean_steps--; }
            BlockPos bp(tx, y + dy, tz);
            if (t.air_or_leaves(bp)) {
                place_log(t, bp);
                place_log(t, rel(bp, EAST));
                place_log(t, rel(bp, SOUTH));
                place_log(t, rel(rel(bp, EAST), SOUTH));
            }
        }
        out.push_back(Attach{BlockPos(tx, ey, tz), 0, true});
        for (int ox = -1; ox <= 2; ox++)
            for (int oz = -1; oz <= 2; oz++) {
                if ((ox < 0 || ox > 1 || oz < 0 || oz > 1) && t.random.next_int(3) <= 0) {
                    int length = t.random.next_int(3) + 2;
                    for (int by = 0; by < length; by++) place_log(t, BlockPos(x + ox, ey - by - 1, z + oz));
                    out.push_back(Attach{BlockPos(x + ox, ey, z + oz), 0, false});
                }
            }
        return out;
    }
    std::vector<Attach> bending(TreeCtx& t, int h, const BlockPos& o) const {
        Dir dir = kHorizontal[t.random.next_int(4)];
        int log_height = h - 1;
        BlockPos pos = o;
        place_below(t, pos.below());
        std::vector<Attach> out;
        int min_leaf = int(j.get_int("min_height_for_leaves", 1));
        for (int i = 0; i <= log_height; i++) {
            if (i + 1 >= log_height + t.random.next_int(2)) pos = rel(pos, dir);
            if (t.valid_tree_pos(pos)) place_log(t, pos);
            if (i >= min_leaf) out.push_back(Attach{pos, 0, false});
            pos = pos.above();
        }
        int dir_length = sample_int(j.at("bend_length"), t.random);
        for (int i = 0; i <= dir_length; i++) {
            if (t.valid_tree_pos(pos)) place_log(t, pos);
            out.push_back(Attach{pos, 0, false});
            pos = rel(pos, dir);
        }
        return out;
    }
    void place_branch_up(TreeCtx& t, int tree_height, std::vector<Attach>& out, BlockPos& log_pos, int current_height, Dir dir, int branch_pos, int branch_steps) const {
        int height_along = current_height + branch_pos;
        int lx = log_pos.x, lz = log_pos.z;
        int idx = branch_pos;
        while (idx < tree_height && branch_steps > 0) {
            if (idx >= 1) {
                int ph = current_height + idx;
                lx += kDirX[dir]; lz += kDirZ[dir];
                height_along = ph;
                log_pos = BlockPos(lx, ph, lz);
                if (place_log(t, log_pos)) height_along = ph + 1;
                out.push_back(Attach{log_pos, 0, false});
            }
            idx++;
            branch_steps--;
        }
        if (height_along - current_height > 1) {
            BlockPos fp(lx, height_along, lz);
            out.push_back(Attach{fp, 0, false});
            out.push_back(Attach{fp.below(2), 0, false});
        }
    }
    std::vector<Attach> upwards_branching(TreeCtx& t, int h, const BlockPos& o) const {
        std::vector<Attach> out;
        BlockPos log_pos;
        float prob = j.at("place_branch_per_log_probability").float_value();
        for (int hp = 0; hp < h; hp++) {
            int current = o.y + hp;
            log_pos = BlockPos(o.x, current, o.z);
            bool placed = place_log(t, log_pos);
            if (placed && hp < h - 1 && t.random.next_float() < prob) {
                Dir dir = kHorizontal[t.random.next_int(4)];
                int len = sample_int(j.at("extra_branch_length"), t.random);
                int len2 = sample_int(j.at("extra_branch_length"), t.random);
                int branch_pos = std::max(0, len - len2 - 1);
                int steps = sample_int(j.at("extra_branch_steps"), t.random);
                place_branch_up(t, h, out, log_pos, current, dir, branch_pos, steps);
            }
            if (hp == h - 1) { log_pos = BlockPos(o.x, current + 1, o.z); out.push_back(Attach{log_pos, 0, false}); }
        }
        return out;
    }
    Attach cherry_branch(TreeCtx& t, int tree_height, const BlockPos& o, Dir branch_dir, int offset_from_origin, bool middle_continues, char sideways_axis) const {
        BlockPos log_pos = o.above(offset_from_origin);
        int end_offset = tree_height - 1 + sample_int(j.at("branch_end_offset_from_top"), t.random);
        bool extend = middle_continues || end_offset < offset_from_origin;
        int distance_to_trunk = sample_int(j.at("branch_horizontal_length"), t.random) + (extend ? 1 : 0);
        BlockPos end_pos = rel(o, branch_dir, distance_to_trunk).above(end_offset);
        int steps_h = extend ? 2 : 1;
        for (int i = 0; i < steps_h; i++) { log_pos = rel(log_pos, branch_dir); place_log(t, log_pos, sideways_axis); }
        Dir vertical = end_pos.y > log_pos.y ? UP : DOWN;
        while (true) {
            int distance = manhattan(log_pos, end_pos);
            if (distance == 0) return Attach{end_pos.above(), 0, false};
            float chance = float(std::abs(end_pos.y - log_pos.y)) / float(distance);
            bool grow_vertical = t.random.next_float() < chance;
            log_pos = rel(log_pos, grow_vertical ? vertical : branch_dir);
            place_log(t, log_pos, grow_vertical ? 0 : sideways_axis);
        }
    }
    std::vector<Attach> cherry(TreeCtx& t, int h, const BlockPos& o) const {
        place_below(t, o.below());
        const Json& start = j.at("branch_start_offset_from_top");
        int s_lo = int(start.at("min_inclusive").int_value()), s_hi = int(start.at("max_inclusive").int_value());
        int first = std::max(0, h - 1 + (t.random.next_int(s_hi - s_lo + 1) + s_lo));
        int second = std::max(0, h - 1 + (t.random.next_int((s_hi - 1) - s_lo + 1) + s_lo));
        if (second >= first) second++;
        int branch_count = sample_int(j.at("branch_count"), t.random);
        bool middle = branch_count == 3, both = branch_count >= 2;
        int trunk_height = middle ? h : both ? std::max(first, second) + 1 : first + 1;
        for (int y = 0; y < trunk_height; y++) place_log(t, o.above(y));
        std::vector<Attach> out;
        if (middle) out.push_back(Attach{o.above(trunk_height), 0, false});
        Dir tree_dir = kHorizontal[t.random.next_int(4)];
        char axis = (tree_dir == NORTH || tree_dir == SOUTH) ? 'z' : 'x';
        out.push_back(cherry_branch(t, h, o, tree_dir, first, first < trunk_height - 1, axis));
        if (both) out.push_back(cherry_branch(t, h, o, opposite(tree_dir), second, second < trunk_height - 1, axis));
        return out;
    }
};

std::vector<Attach> Trunk::place(TreeCtx& t, int h, const BlockPos& o) const {
    if (type == "straight_trunk_placer") return straight(t, h, o);
    if (type == "giant_trunk_placer") return giant(t, h, o);
    if (type == "mega_jungle_trunk_placer") return mega_jungle(t, h, o);
    if (type == "fancy_trunk_placer") return fancy(t, h, o);
    if (type == "forking_trunk_placer") return forking(t, h, o);
    if (type == "dark_oak_trunk_placer") return dark_oak(t, h, o);
    if (type == "bending_trunk_placer") return bending(t, h, o);
    if (type == "upwards_branching_trunk_placer") return upwards_branching(t, h, o);
    if (type == "cherry_trunk_placer") return cherry(t, h, o);
    throw std::runtime_error("unknown trunk placer " + type);
}

// ---------------------------------------------------------------- foliage placers

struct Foliage {
    std::string type;
    const Json& j;
    explicit Foliage(const Json& fj) : type(short_type(fj)), j(fj) {}

    int foliage_height(TreeCtx& t, int tree_height) const {
        if (type == "blob_foliage_placer" || type == "fancy_foliage_placer" || type == "bush_foliage_placer") return int(j.at("height").int_value());
        if (type == "jungle_foliage_placer" || type == "mega_jungle_foliage_placer") return int(j.at("height").int_value());
        if (type == "spruce_foliage_placer") return std::max(4, tree_height - sample_int(j.at("trunk_height"), t.random));
        if (type == "pine_foliage_placer") return sample_int(j.at("height"), t.random);
        if (type == "mega_pine_foliage_placer") return sample_int(j.at("crown_height"), t.random);
        if (type == "dark_oak_foliage_placer") return 4;
        if (type == "acacia_foliage_placer") return 0;
        if (type == "random_spread_foliage_placer") return sample_int(j.at("foliage_height"), t.random);
        if (type == "cherry_foliage_placer") return sample_int(j.at("height"), t.random);
        throw std::runtime_error("unknown foliage placer " + type);
    }
    int foliage_radius(TreeCtx& t, int trunk_height) const {
        int r = sample_int(j.at("radius"), t.random);
        if (type == "pine_foliage_placer") { int extra = t.random.next_int(std::max(trunk_height + 1, 1)); return r + extra; }
        return r;
    }
    bool skip(TreeCtx& t, int dx, int y, int dz, int cr, bool dt) const {
        Rng& r = t.random;
        if (type == "blob_foliage_placer") return dx == cr && dz == cr && (r.next_int(2) == 0 || y == 0);
        if (type == "fancy_foliage_placer") { float a = float(dx) + 0.5f, b = float(dz) + 0.5f; return a * a + b * b > float(cr * cr); }
        if (type == "bush_foliage_placer") return dx == cr && dz == cr && r.next_int(2) == 0;
        if (type == "spruce_foliage_placer" || type == "pine_foliage_placer") return dx == cr && dz == cr && cr > 0;
        if (type == "mega_pine_foliage_placer" || type == "jungle_foliage_placer" || type == "mega_jungle_foliage_placer") return dx + dz >= 7 ? true : dx * dx + dz * dz > cr * cr;
        if (type == "dark_oak_foliage_placer") {
            if (y == -1 && !dt) return dx == cr && dz == cr;
            return y == 1 ? dx + dz > cr * 2 - 2 : false;
        }
        if (type == "acacia_foliage_placer") return y == 0 ? (dx > 1 || dz > 1) && dx != 0 && dz != 0 : dx == cr && dz == cr && cr > 0;
        if (type == "random_spread_foliage_placer") return false;
        if (type == "cherry_foliage_placer") {
            float wide_hole = j.at("wide_bottom_layer_hole_chance").float_value(), corner_hole = j.at("corner_hole_chance").float_value();
            if (y == -1 && (dx == cr || dz == cr) && r.next_float() < wide_hole) return true;
            bool corner = dx == cr && dz == cr, wide = cr > 2;
            if (wide) return corner || (dx + dz > cr * 2 - 2 && r.next_float() < corner_hole);
            return corner && r.next_float() < corner_hole;
        }
        throw std::runtime_error("unknown foliage placer " + type);
    }
    bool skip_signed(TreeCtx& t, int dx, int y, int dz, int cr, bool dt) const {
        if (type == "dark_oak_foliage_placer" && y == 0 && dt && (dx == -cr || dx >= cr) && (dz == -cr || dz >= cr)) return true;
        int mdx, mdz;
        if (dt) { mdx = std::min(std::abs(dx), std::abs(dx - 1)); mdz = std::min(std::abs(dz), std::abs(dz - 1)); }
        else { mdx = std::abs(dx); mdz = std::abs(dz); }
        return skip(t, mdx, y, mdz, cr, dt);
    }
    bool try_place_leaf(TreeCtx& t, const BlockPos& p) const {
        const BlockData& bd = t.bd;
        uint16_t cur = t.get(p);
        bool persistent = bd.has_prop(cur, "persistent") && bd.prop(cur, "persistent") == "true";
        if (!persistent && t.valid_tree_pos(p)) {
            uint16_t s = provide_state(t.env, t.cfg.at("foliage_provider"), t.random, p);
            if (bd.has_prop(s, "waterlogged")) s = bd.with(s, "waterlogged", (bd.water(cur) && (bd.flags(cur) & BF_SOURCE)) ? "true" : "false");
            t.set_foliage(p, s);
            return true;
        }
        return false;
    }
    void leaves_row(TreeCtx& t, const BlockPos& origin, int cr, int y, bool dt) const {
        int offset = dt ? 1 : 0;
        for (int dx = -cr; dx <= cr + offset; dx++)
            for (int dz = -cr; dz <= cr + offset; dz++)
                if (!skip_signed(t, dx, y, dz, cr, dt)) try_place_leaf(t, origin.offset(dx, y, dz));
    }
    bool try_place_extension(TreeCtx& t, float chance, const BlockPos& log_pos, const BlockPos& pos) const {
        if (manhattan(pos, log_pos) >= 7) return false;
        return t.random.next_float() > chance ? false : try_place_leaf(t, pos);
    }
    void leaves_row_hanging(TreeCtx& t, const BlockPos& origin, int cr, int y, bool dt, float chance, float ext_chance) const {
        leaves_row(t, origin, cr, y, dt);
        int offset = dt ? 1 : 0;
        BlockPos log_pos = origin.below();
        for (Dir along : kHorizontal) {
            Dir to_edge = clockwise(along);
            bool positive = to_edge == EAST || to_edge == SOUTH;
            int offset_to_edge = positive ? cr + offset : cr;
            BlockPos pos = rel(rel(origin.offset(0, y - 1, 0), to_edge, offset_to_edge), along, -cr);
            int off_along = -cr;
            while (off_along < cr + offset) {
                bool above = t.foliage.contains(pos.above());
                if (above && try_place_extension(t, chance, log_pos, pos)) {
                    pos = pos.below();
                    try_place_extension(t, ext_chance, log_pos, pos);
                    pos = pos.above();
                }
                off_along++;
                pos = rel(pos, along);
            }
        }
    }
    void create(TreeCtx& t, int tree_height, const Attach& a, int foliage_h, int leaf_radius) const {
        int offset = sample_int(j.at("offset"), t.random);
        const BlockPos& p = a.pos;
        if (type == "blob_foliage_placer") {
            for (int yo = offset; yo >= offset - foliage_h; yo--) {
                int cr = std::max(leaf_radius + a.radius_offset - 1 - yo / 2, 0);
                leaves_row(t, p, cr, yo, a.double_trunk);
            }
        } else if (type == "fancy_foliage_placer") {
            for (int yo = offset; yo >= offset - foliage_h; yo--) {
                int cr = leaf_radius + (yo != offset && yo != offset - foliage_h ? 1 : 0);
                leaves_row(t, p, cr, yo, a.double_trunk);
            }
        } else if (type == "bush_foliage_placer") {
            for (int yo = offset; yo >= offset - foliage_h; yo--) {
                int cr = leaf_radius + a.radius_offset - 1 - yo;
                leaves_row(t, p, cr, yo, a.double_trunk);
            }
        } else if (type == "spruce_foliage_placer") {
            int cr = t.random.next_int(2), max_r = 1, min_r = 0;
            for (int yo = offset; yo >= -foliage_h; yo--) {
                leaves_row(t, p, cr, yo, a.double_trunk);
                if (cr >= max_r) { cr = min_r; min_r = 1; max_r = std::min(max_r + 1, leaf_radius + a.radius_offset); }
                else cr++;
            }
        } else if (type == "pine_foliage_placer") {
            int cr = 0;
            for (int yo = offset; yo >= offset - foliage_h; yo--) {
                leaves_row(t, p, cr, yo, a.double_trunk);
                if (cr >= 1 && yo == offset - foliage_h + 1) cr--;
                else if (cr < leaf_radius + a.radius_offset) cr++;
            }
        } else if (type == "mega_pine_foliage_placer") {
            int prev = 0;
            for (int yy = p.y - foliage_h + offset; yy <= p.y + offset; yy++) {
                int yo = p.y - yy;
                int smooth = leaf_radius + a.radius_offset + mth_floor_f(float(yo) / float(foliage_h) * 3.5f);
                int jagged = (yo > 0 && smooth == prev && (yy & 1) == 0) ? smooth + 1 : smooth;
                leaves_row(t, BlockPos(p.x, yy, p.z), jagged, 0, a.double_trunk);
                prev = smooth;
            }
        } else if (type == "dark_oak_foliage_placer") {
            BlockPos pos = p.above(offset);
            bool dt = a.double_trunk;
            if (dt) {
                leaves_row(t, pos, leaf_radius + 2, -1, dt);
                leaves_row(t, pos, leaf_radius + 3, 0, dt);
                leaves_row(t, pos, leaf_radius + 2, 1, dt);
                if (t.random.next_boolean()) leaves_row(t, pos, leaf_radius, 2, dt);
            } else {
                leaves_row(t, pos, leaf_radius + 2, -1, dt);
                leaves_row(t, pos, leaf_radius + 1, 0, dt);
            }
        } else if (type == "acacia_foliage_placer") {
            bool dt = a.double_trunk;
            BlockPos pos = p.above(offset);
            leaves_row(t, pos, leaf_radius + a.radius_offset, -1 - foliage_h, dt);
            leaves_row(t, pos, leaf_radius - 1, -foliage_h, dt);
            leaves_row(t, pos, leaf_radius + a.radius_offset - 1, 0, dt);
        } else if (type == "jungle_foliage_placer" || type == "mega_jungle_foliage_placer") {
            int leaf_height = a.double_trunk ? foliage_h : 1 + t.random.next_int(2);
            for (int yo = offset; yo >= offset - leaf_height; yo--) {
                int cr = leaf_radius + a.radius_offset + 1 - yo;
                leaves_row(t, p, cr, yo, a.double_trunk);
            }
        } else if (type == "random_spread_foliage_placer") {
            int attempts = int(j.at("leaf_placement_attempts").int_value());
            for (int i = 0; i < attempts; i++) {
                int a1 = t.random.next_int(leaf_radius); int a2 = t.random.next_int(leaf_radius);
                int b1 = t.random.next_int(foliage_h); int b2 = t.random.next_int(foliage_h);
                int c1 = t.random.next_int(leaf_radius); int c2 = t.random.next_int(leaf_radius);
                try_place_leaf(t, p.offset(a1 - a2, b1 - b2, c1 - c2));
            }
        } else if (type == "cherry_foliage_placer") {
            bool dt = a.double_trunk;
            BlockPos pos = p.above(offset);
            int cr = leaf_radius + a.radius_offset - 1;
            float hang = j.at("hanging_leaves_chance").float_value(), hang_ext = j.at("hanging_leaves_extension_chance").float_value();
            leaves_row(t, pos, cr - 2, foliage_h - 3, dt);
            leaves_row(t, pos, cr - 1, foliage_h - 4, dt);
            for (int y = foliage_h - 5; y >= 0; y--) leaves_row(t, pos, cr, y, dt);
            leaves_row_hanging(t, pos, cr, -1, dt, hang, hang_ext);
            leaves_row_hanging(t, pos, cr - 1, -2, dt, hang, hang_ext);
        } else {
            throw std::runtime_error("unknown foliage placer " + type);
        }
    }
};

// ---------------------------------------------------------------- root placer (mangrove)

struct RootPlacer {
    const Json& j;
    explicit RootPlacer(const Json& rj) : j(rj) {}
    const Json& mangrove() const { return j.at("mangrove_root_placement"); }

    BlockPos trunk_origin(TreeCtx& t, const BlockPos& origin) const { return origin.above(sample_int(j.at("trunk_offset_y"), t.random)); }
    bool can_place_root(TreeCtx& t, const BlockPos& p) const { return t.valid_tree_pos(p) || state_in_set(t.env, t.get(p), mangrove().at("can_grow_through")); }
    uint16_t waterlogged_state(TreeCtx& t, const BlockPos& p, uint16_t s) const {
        if (!t.bd.has_prop(s, "waterlogged")) return s;
        return t.bd.with(s, "waterlogged", t.bd.water(t.get(p)) ? "true" : "false");
    }
    void place_root(TreeCtx& t, const BlockPos& pos) const {
        if (state_in_set(t.env, t.get(pos), mangrove().at("muddy_roots_in"))) {
            uint16_t s = provide_state(t.env, mangrove().at("muddy_roots_provider"), t.random, pos);
            t.set_root(pos, waterlogged_state(t, pos, s));
            return;
        }
        if (t.valid_tree_pos(pos) || state_in_set(t.env, t.get(pos), mangrove().at("can_grow_through"))) {
            uint16_t s = provide_state(t.env, j.at("root_provider"), t.random, pos);
            t.set_root(pos, waterlogged_state(t, pos, s));
            if (const Json* above = j.find("above_root_placement")) {
                BlockPos ap = pos.above();
                if (t.random.next_float() < above->at("above_root_placement_chance").float_value() && t.is_air(ap)) {
                    uint16_t as = provide_state(t.env, above->at("above_root_provider"), t.random, ap);
                    t.set_root(ap, waterlogged_state(t, ap, as));
                }
            }
        }
    }
    bool simulate(TreeCtx& t, const BlockPos& root_pos, Dir dir, const BlockPos& root_origin, std::vector<BlockPos>& positions, int layer) const {
        int max_len = int(mangrove().at("max_root_length").int_value());
        if (layer == max_len || int(positions.size()) > max_len) return false;
        for (const BlockPos& p : potential(t, root_pos, dir, root_origin)) {
            if (can_place_root(t, p)) {
                positions.push_back(p);
                if (!simulate(t, p, dir, root_origin, positions, layer + 1)) return false;
            }
        }
        return true;
    }
    std::vector<BlockPos> potential(TreeCtx& t, const BlockPos& pos, Dir prev, const BlockPos& root_origin) const {
        BlockPos below = pos.below(), next_to = rel(pos, prev);
        int width = manhattan(pos, root_origin);
        int max_w = int(mangrove().at("max_root_width").int_value());
        float skew = mangrove().at("random_skew_chance").float_value();
        if (width > max_w - 3 && width <= max_w) return t.random.next_float() < skew ? std::vector<BlockPos>{below, next_to.below()} : std::vector<BlockPos>{below};
        if (width > max_w) return {below};
        if (t.random.next_float() < skew) return {below};
        return t.random.next_boolean() ? std::vector<BlockPos>{next_to} : std::vector<BlockPos>{below};
    }
    bool place_roots(TreeCtx& t, const BlockPos& origin, const BlockPos& trunk_origin_pos) const {
        std::vector<BlockPos> roots;
        BlockPos col = origin;
        while (col.y < trunk_origin_pos.y) {
            if (!can_place_root(t, col)) return false;
            col = col.above();
        }
        roots.push_back(trunk_origin_pos.below());
        for (Dir dir : kHorizontal) {
            BlockPos pos = rel(trunk_origin_pos, dir);
            std::vector<BlockPos> in_dir;
            if (!simulate(t, pos, dir, trunk_origin_pos, in_dir, 0)) return false;
            roots.insert(roots.end(), in_dir.begin(), in_dir.end());
            roots.push_back(rel(trunk_origin_pos, dir));
        }
        for (auto& p : roots) place_root(t, p);
        return true;
    }
};

// ---------------------------------------------------------------- decorators

struct DecoCtx {
    TreeCtx& t;
    std::vector<BlockPos> logs, leaves, roots;
    DecoCtx(TreeCtx& tc) : t(tc) {
        logs = tc.trunks.items(); leaves = tc.foliage.items(); roots = tc.roots.items();
        auto by_y = [](const BlockPos& a, const BlockPos& b) { return a.y < b.y; };
        std::stable_sort(logs.begin(), logs.end(), by_y);
        std::stable_sort(leaves.begin(), leaves.end(), by_y);
        std::stable_sort(roots.begin(), roots.end(), by_y);
    }
    bool is_air(const BlockPos& p) const { return t.is_air(p); }
    void place_vine(const BlockPos& p, const char* face) {
        uint16_t v = t.bd.with(t.bd.find("minecraft:vine", "east=false,north=false,south=false,up=false,west=false"), face, "true");
        t.set_decoration(p, v);
    }
};

void shuffle(std::vector<BlockPos>& list, Rng& r) {
    for (int i = int(list.size()); i > 1; i--) {
        int swap_to = r.next_int(i);
        std::swap(list[size_t(i - 1)], list[size_t(swap_to)]);
    }
}

std::vector<BlockPos> lowest_trunk_or_root(DecoCtx& c) {
    std::vector<BlockPos> out;
    if (c.roots.empty()) out = c.logs;
    else if (!c.logs.empty() && c.roots[0].y == c.logs[0].y) { out = c.logs; out.insert(out.end(), c.roots.begin(), c.roots.end()); }
    else out = c.roots;
    return out;
}

void run_decorator(DecoCtx& c, const Json& d) {
    TreeCtx& t = c.t;
    Rng& r = t.random;
    const std::string type = short_type(d);
    const BlockData& bd = t.bd;
    if (type == "beehive") {
        float prob = d.at("probability").float_value();
        if (c.logs.empty()) return;
        if (r.next_float() >= prob) return;
        int hive_y;
        if (!c.leaves.empty()) hive_y = std::max(c.leaves.front().y - 1, c.logs.front().y + 1);
        else { int rr = r.next_int(3); hive_y = std::min(c.logs.front().y + 1 + rr, c.logs.back().y); }
        // SPAWN_DIRECTIONS: horizontal directions except the opposite of SOUTH (north): east, south, west in plane order
        static const Dir spawn[3] = {EAST, SOUTH, WEST};
        std::vector<BlockPos> placements;
        for (auto& p : c.logs) if (p.y == hive_y) for (Dir dd : spawn) placements.push_back(rel(p, dd));
        if (placements.empty()) return;
        shuffle(placements, r);
        for (auto& p : placements) {
            if (c.is_air(p) && c.is_air(rel(p, SOUTH))) {
                uint16_t nest = bd.with(bd.default_state("minecraft:bee_nest"), "facing", "south");
                t.set_decoration(p, nest);
                int bees = 2 + r.next_int(2);
                for (int k = 0; k < bees; k++) r.next_int(599);
                break;
            }
        }
    } else if (type == "leave_vine") {
        float prob = d.at("probability").float_value();
        for (auto& pos : c.leaves) {
            struct Side { Dir d; const char* face; };
            static const Side sides[4] = {{WEST, "east"}, {EAST, "west"}, {NORTH, "south"}, {SOUTH, "north"}};
            for (auto& s : sides) {
                if (r.next_float() < prob) {
                    BlockPos p = rel(pos, s.d);
                    if (c.is_air(p)) {
                        c.place_vine(p, s.face);
                        int max_dir = 4;
                        for (BlockPos q = p.below(); c.is_air(q) && max_dir > 0; max_dir--) { c.place_vine(q, s.face); q = q.below(); }
                    }
                }
            }
        }
    } else if (type == "trunk_vine") {
        for (auto& pos : c.logs) {
            struct Side { Dir d; const char* face; };
            static const Side sides[4] = {{WEST, "east"}, {EAST, "west"}, {NORTH, "south"}, {SOUTH, "north"}};
            for (auto& s : sides) {
                if (r.next_int(3) > 0) {
                    BlockPos p = rel(pos, s.d);
                    if (c.is_air(p)) c.place_vine(p, s.face);
                }
            }
        }
    } else if (type == "cocoa") {
        float prob = d.at("probability").float_value();
        if (r.next_float() >= prob) return;
        if (c.logs.empty()) return;
        int tree_y = c.logs.front().y;
        for (auto& pos : c.logs) {
            if (pos.y - tree_y > 2) continue;
            for (Dir dir : kHorizontal) {
                if (r.next_float() <= 0.25f) {
                    Dir opp = opposite(dir);
                    BlockPos cp = pos.offset(kDirX[opp], 0, kDirZ[opp]);
                    if (c.is_air(cp)) {
                        static const char* names[6] = {"down", "up", "north", "south", "west", "east"};
                        uint16_t s = bd.with(bd.with(bd.default_state("minecraft:cocoa"), "age", std::to_string(r.next_int(3))), "facing", names[dir]);
                        t.set_decoration(cp, s);
                    }
                }
            }
        }
    } else if (type == "place_on_ground") {
        auto positions = lowest_trunk_or_root(c);
        if (positions.empty()) return;
        int tries = int(d.get_int("tries", 128)), radius = int(d.get_int("radius", 2)), height = int(d.get_int("height", 1));
        int min_y = positions[0].y, min_x = positions[0].x, max_x = positions[0].x, min_z = positions[0].z, max_z = positions[0].z;
        for (auto& p : positions) if (p.y == min_y) { min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x); min_z = std::min(min_z, p.z); max_z = std::max(max_z, p.z); }
        int bx0 = min_x - radius, bx1 = max_x + radius, by0 = min_y - height, by1 = min_y + height, bz0 = min_z - radius, bz1 = max_z + radius;
        for (int i = 0; i < tries; i++) {
            int x = r.next_int_between_inclusive(bx0, bx1);
            int y = r.next_int_between_inclusive(by0, by1);
            int z = r.next_int_between_inclusive(bz0, bz1);
            BlockPos pos(x, y, z), above = pos.above();
            uint16_t as = t.get(above);
            if ((bd.air(as) || bd.name(as) == "minecraft:vine") && (bd.flags(t.get(pos)) & BF_SOLID_RENDER) && t.lv.height(MOTION_BLOCKING_NO_LEAVES, x, z) <= above.y) {
                uint16_t s = provide_state(t.env, d.at("block_state_provider"), r, above);
                if (s != kNoState) t.set_decoration(above, s);
            }
        }
    } else if (type == "alter_ground") {
        auto positions = lowest_trunk_or_root(c);
        if (positions.empty()) return;
        int min_y = positions[0].y;
        auto place_block_at = [&](const BlockPos& pos) {
            for (int dy = 2; dy >= -3; dy--) {
                BlockPos cursor = pos.above(dy);
                uint16_t s = provide_state(t.env, d.at("provider"), r, cursor);
                if (s != kNoState) { t.set_decoration(cursor, s); break; }
                if (!c.is_air(cursor) && dy < 0) break;
            }
        };
        auto place_circle = [&](const BlockPos& pos) {
            for (int xx = -2; xx <= 2; xx++)
                for (int zz = -2; zz <= 2; zz++)
                    if (std::abs(xx) != 2 || std::abs(zz) != 2) place_block_at(pos.offset(xx, 0, zz));
        };
        for (auto& pos : positions) {
            if (pos.y != min_y) continue;
            place_circle(pos.offset(-1, 0, -1));
            place_circle(pos.offset(2, 0, -1));
            place_circle(pos.offset(-1, 0, 2));
            place_circle(pos.offset(2, 0, 2));
            for (int i = 0; i < 5; i++) {
                int placement = r.next_int(64);
                int xx = placement % 8, zz = placement / 8;
                if (xx == 0 || xx == 7 || zz == 0 || zz == 7) place_circle(pos.offset(-3 + xx, 0, -3 + zz));
            }
        }
    } else if (type == "attached_to_leaves") {
        float prob = d.at("probability").float_value();
        int xz = int(d.at("exclusion_radius_xz").int_value()), yy = int(d.at("exclusion_radius_y").int_value()), req = int(d.at("required_empty_blocks").int_value());
        std::vector<Dir> dirs;
        static const char* names[6] = {"down", "up", "north", "south", "west", "east"};
        for (auto& dn : d.at("directions").arr) for (int k = 0; k < 6; k++) if (dn.str == names[k]) dirs.push_back(Dir(k));
        JHashSet blacklist;
        std::vector<BlockPos> shuffled = c.leaves;
        shuffle(shuffled, r);
        for (auto& leaf : shuffled) {
            Dir dir = dirs[size_t(r.next_int(int(dirs.size())))];
            BlockPos placement = rel(leaf, dir);
            if (blacklist.contains(placement)) continue;
            if (!(r.next_float() < prob)) continue;
            bool ok = true;
            for (int i = 1; i <= req; i++) if (!c.is_air(rel(leaf, dir, i))) { ok = false; break; }
            if (!ok) continue;
            for (int z = placement.z - xz; z <= placement.z + xz; z++)
                for (int y = placement.y - yy; y <= placement.y + yy; y++)
                    for (int x = placement.x - xz; x <= placement.x + xz; x++) blacklist.add(BlockPos(x, y, z));
            uint16_t s = provide_state(t.env, d.at("block_provider"), r, placement);
            if (s != kNoState) t.set_decoration(placement, s);
        }
    } else {
        throw std::runtime_error("tree decorator not ported yet: " + type);
    }
}

// ---------------------------------------------------------------- the feature

int leaf_distance_at(TreeCtx& t, uint16_t s, bool& present) {
    present = true;
    if (t.bd.in_tag(s, "minecraft:prevents_nearby_leaf_decay")) return 0;
    if (t.bd.has_prop(s, "distance")) return std::stoi(t.bd.prop(s, "distance"));
    present = false;
    return 0;
}

void update_leaves(TreeCtx& t, int minx, int miny, int minz, int maxx, int maxy, int maxz) {
    auto inside = [&](const BlockPos& p) { return p.x >= minx && p.x <= maxx && p.y >= miny && p.y <= maxy && p.z >= minz && p.z <= maxz; };
    const int sx = maxx - minx + 1, sy = maxy - miny + 1, sz = maxz - minz + 1;
    std::vector<uint8_t> shape(size_t(sx) * size_t(sy) * size_t(sz), 0);
    auto idx = [&](const BlockPos& p) { return (size_t(p.x - minx) * size_t(sy) + size_t(p.y - miny)) * size_t(sz) + size_t(p.z - minz); };
    for (auto& p : t.decorations.items()) if (inside(p)) shape[idx(p)] = 1;
    for (auto& p : t.roots.items()) if (inside(p)) shape[idx(p)] = 1;
    std::vector<std::set<BlockPos>> to_check(7);
    for (auto& p : t.trunks.items()) to_check[0].insert(p);
    int smallest = 0;
    while (true) {
        while (smallest >= 7 || !to_check[size_t(smallest)].empty()) {
            if (smallest >= 7) return;
            BlockPos pos = *to_check[size_t(smallest)].begin();
            to_check[size_t(smallest)].erase(to_check[size_t(smallest)].begin());
            if (!inside(pos)) continue;
            if (smallest != 0) {
                uint16_t s = t.get(pos);
                if (t.bd.has_prop(s, "distance")) t.lv.set(pos, t.bd.with(s, "distance", std::to_string(smallest)));
            }
            shape[idx(pos)] = 1;
            for (int d = 0; d < 6; d++) {
                BlockPos n = pos + Dir(d);
                if (!inside(n) || shape[idx(n)]) continue;
                bool present;
                int dist = leaf_distance_at(t, t.get(n), present);
                if (!present) continue;
                int nd = std::min(dist, smallest + 1);
                if (nd < 7) { to_check[size_t(nd)].insert(n); smallest = std::min(smallest, nd); }
            }
        }
        smallest++;
    }
}

bool place_tree(FeatureEnv& env, const Json& cfg, Rng& random, const BlockPos& origin) {
    TreeCtx t(env, random, cfg);
    Trunk trunk(cfg.at("trunk_placer"));
    Foliage foliage(cfg.at("foliage_placer"));
    const Json* root_j = cfg.find("root_placer");
    int tree_height = trunk.tree_height(random);
    int foliage_h = foliage.foliage_height(t, tree_height);
    int trunk_h = tree_height - foliage_h;
    int leaf_radius = foliage.foliage_radius(t, trunk_h);
    std::unique_ptr<RootPlacer> root;
    BlockPos trunk_origin = origin;
    if (root_j) { root = std::make_unique<RootPlacer>(*root_j); trunk_origin = root->trunk_origin(t, origin); }
    int min_y = std::min(origin.y, trunk_origin.y), max_y = std::max(origin.y, trunk_origin.y) + tree_height + 1;
    Level& lv = env.level;
    if (!(min_y >= lv.min_y() + 1 && max_y <= lv.max_y() + 1)) return false;
    // FeatureSize
    const Json& size = cfg.at("minimum_size");
    bool has_min_clip = size.has("min_clipped_height");
    int min_clip = has_min_clip ? int(size.at("min_clipped_height").int_value()) : 0;
    std::string size_type = short_type(size);
    auto size_at = [&](int h, int yo) {
        if (size_type == "two_layers_feature_size") return yo < int(size.get_int("limit", 1)) ? int(size.get_int("lower_size", 0)) : int(size.get_int("upper_size", 1));
        int limit = int(size.get_int("limit", 1)), upper_limit = int(size.get_int("upper_limit", 1));
        if (yo < limit) return int(size.get_int("lower_size", 0));
        return yo >= h - upper_limit ? int(size.get_int("upper_size", 1)) : int(size.get_int("middle_size", 1));
    };
    bool ignore_vines = cfg.get_bool("ignore_vines", false);
    // getMaxFreeTreeHeight
    int clipped = tree_height;
    for (int y = 0; y <= tree_height + 1; y++) {
        int r = size_at(tree_height, y);
        bool blocked = false;
        for (int x = -r; x <= r && !blocked; x++)
            for (int z = -r; z <= r; z++) {
                BlockPos p = trunk_origin.offset(x, y, z);
                if (!trunk.is_free(t, p) || (!ignore_vines && t.is_vine(p))) { blocked = true; break; }
            }
        if (blocked) { clipped = y - 2; break; }
    }
    bool ok;
    if (clipped >= tree_height || (has_min_clip && clipped >= min_clip)) {
        if (root && !root->place_roots(t, origin, trunk_origin)) { ok = false; }
        else {
            std::vector<Attach> attachments = trunk.place(t, clipped, trunk_origin);
            for (auto& a : attachments) foliage.create(t, clipped, a, foliage_h, leaf_radius);
            ok = true;
        }
    } else ok = false;
    if (!(ok && (!t.trunks.empty() || !t.foliage.empty()))) return false;
    if (const Json* decs = cfg.find("decorators"); decs && !decs->arr.empty()) {
        DecoCtx dc(t);
        for (auto& d : decs->arr) run_decorator(dc, d);
    }
    // bounding box of everything placed
    bool any = false;
    int minx = 0, miny = 0, minz = 0, maxx = 0, maxy = 0, maxz = 0;
    auto grow = [&](const JHashSet& s) {
        for (auto& p : s.items()) {
            if (!any) { minx = maxx = p.x; miny = maxy = p.y; minz = maxz = p.z; any = true; continue; }
            minx = std::min(minx, p.x); maxx = std::max(maxx, p.x); miny = std::min(miny, p.y); maxy = std::max(maxy, p.y); minz = std::min(minz, p.z); maxz = std::max(maxz, p.z);
        }
    };
    grow(t.roots); grow(t.trunks); grow(t.foliage); grow(t.decorations);
    if (!any) return false;
    update_leaves(t, minx, miny, minz, maxx, maxy, maxz);
    return true;
}

}  // namespace

bool place_tree_feature(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin) { return place_tree(env, config, random, origin); }

}  // namespace pn::vg
