// Desert temples, ruined portals, nether fortresses and end cities.
#include "structures_util.hpp"
#include "worldgen_biomes.hpp"

namespace pn {

// ---- desert temple ---------------------------------------------------------------------------------------------

std::unique_ptr<Structure> build_desert_temple(const WorldGen& g, int cx, int cz) {
    int X = cx * 16 + 8, Z = cz * 16 + 8, bm;
    int hmin = 1000, hmax = -1000;
    for (int dx : {-10, 0, 10}) for (int dz : {-10, 0, 10}) {
        int b2, h = g.sample(X + dx, Z + dz, b2);
        if (b2 != DESERT) return nullptr;
        hmin = std::min(hmin, h); hmax = std::max(hmax, h);
    }
    int h0 = g.sample(X, Z, bm);
    if (bm != DESERT || hmax - hmin > 6 || hmin < 63) return nullptr;
    auto s = std::make_unique<Structure>();
    uint64_t rs = hash2(g.seed() ^ 0x74656d70, cx, cz);
    int rot = rndi(rs, 4);
    Builder b(*s, X, hmin, Z, rot);
    const uint16_t SAND = sb("sandstone"), CUT = sb("cut_sandstone"), CHISEL = sb("chiseled_sandstone"), SMOOTH = sb("smooth_sandstone");
    const uint16_t ORANGE = sb("orange_terracotta"), BLUE = sb("blue_terracotta");
    (void)h0;
    // the stepped pyramid
    b.box(-10, -12, -10, 10, -1, 10, SAND, P_FOUND);
    for (int i = 0; i <= 9; i++) b.box(-10 + i, i, -10 + i, 10 - i, i, 10 - i, i == 0 ? CUT : SAND);
    b.box(-10, 0, -10, 10, 0, 10, CUT);
    // hollow: a chamber under the top, entered by a stair-shaped hall from the front
    b.air(-3, 1, -3, 3, 6, 3);
    b.box(-3, 0, -3, 3, 0, 3, SMOOTH);
    b.box(-1, 0, -1, 1, 0, 1, BLUE);
    b.box(0, 0, 0, 0, 0, 0, ORANGE);
    // entrance
    b.air(-1, 1, 3, 1, 3, 10);
    for (int z = 4; z <= 10; z++) b.box(-1, 0, z, 1, 0, z, SMOOTH);
    b.air(-1, 1, 6, 1, 3, 10);
    // front towers and decoration stripes
    for (int dx : {-9, 9}) {
        b.box(dx - 1, 1, 7, dx + 1, 11, 9, SAND);
        b.box(dx - 1, 6, 7, dx + 1, 6, 9, ORANGE);
        b.box(dx - 1, 12, 7, dx + 1, 12, 9, CUT);
        b.set(dx, 13, 8, CHISEL);
    }
    // four chests in the chamber's corners (empty: loot tables are not generated)
    for (int dx : {-2, 2}) for (int dz : {-2, 2}) {
        std::string f = std::string("facing=") + b.facing(dz < 0 ? "south" : "north") + ",type=single,waterlogged=false";
        b.set(dx, 1, dz, sb("chest", f.c_str()));
    }
    return s;
}

// ---- ruined portal ----------------------------------------------------------------------------------------------

std::unique_ptr<Structure> build_ruined_portal(const WorldGen& g, int dim, int cx, int cz) {
    if (dim != 0) return nullptr;
    int X = cx * 16 + 8, Z = cz * 16 + 8, bm;
    int h = g.sample(X, Z, bm);
    if (h < 64 || bm == OCEAN || (bm >= OCEAN && bm <= WARM) || bm == RIVER || bm == FROZEN_RIVER) return nullptr;
    auto s = std::make_unique<Structure>();
    uint64_t rs = hash2(g.seed() ^ 0x72706f72, cx, cz);
    int rot = rndi(rs, 4);
    Builder b(*s, X, h, Z, rot);
    const uint16_t OBS = sb("obsidian"), CRY = sb("crying_obsidian"), NR = sb("netherrack"), MAGMA = sb("magma_block"), GOLD = sb("gold_block");
    int w = 2 + rndi(rs, 3), ht = 3 + rndi(rs, 3);   // inside size
    b.box(-w - 2, -6, -4, w + 2, -1, 4, sb("netherrack"), P_FOUND);
    b.box(-w - 2, 0, -3, w + 2, 0, 3, NR, P_FILL);
    for (int i = 0; i < 14; i++) b.set(rndi(rs, 2 * w + 5) - w - 2, 0, rndi(rs, 7) - 3, rnd01(rs) < 0.4 ? MAGMA : NR);
    // frame with gaps
    for (int y = 0; y <= ht + 1; y++)
        for (int x = -1; x <= w; x++) {
            bool edge = x == -1 || x == w || y == 0 || y == ht + 1;
            if (!edge) continue;
            double r = rnd01(rs);
            if (r < 0.25) continue;
            b.set(x - w / 2, 1 + y, 0, r < 0.4 ? CRY : OBS);
        }
    if (rnd01(rs) < 0.3) b.set(0, 1, 2, GOLD);
    b.air(-w, 1, -1, w, ht + 4, 1);
    for (int y = 0; y <= ht + 1; y++) for (int x = -1; x <= w; x++) {   // paint the frame again over the cleared air
        bool edge = x == -1 || x == w || y == 0 || y == ht + 1;
        if (!edge) continue;
        uint64_t k = hash2(rs, x + 100, y + 100);
        double r = rnd01(k);
        if (r < 0.25) continue;
        b.set(x - w / 2, 1 + y, 0, r < 0.4 ? CRY : OBS);
    }
    return s;
}

// ---- nether fortress --------------------------------------------------------------------------------------------

std::unique_ptr<Structure> build_fortress(const WorldGen& g, int cx, int cz) {
    auto s = std::make_unique<Structure>();
    uint64_t rs = hash2(g.seed() ^ 0x666f7274, cx, cz);
    int X = cx * 16 + 8, Z = cz * 16 + 8;
    int Y = 48 + rndi(rs, 18);
    const uint16_t NB = sb("nether_bricks"), NBF_ = sb("nether_brick_fence", "east=false,north=false,south=false,waterlogged=false,west=false");
    const uint16_t FZ = sb("nether_brick_fence", "east=false,north=true,south=true,waterlogged=false,west=false");   // rail along z
    const uint16_t FX = sb("nether_brick_fence", "east=true,north=false,south=false,waterlogged=false,west=true");   // rail along x
    const uint16_t RED = sb("red_nether_bricks"), CHISEL = sb("chiseled_nether_bricks"), SOUL = sb("soul_sand"), WART = sb("nether_wart", "age=3"), LAVA = sb("lava", "level=0");
    (void)NBF_;
    struct Rect { int x0, z0, x1, z1; };
    std::vector<Rect> used;
    auto overlaps = [&](int x0, int z0, int x1, int z1, int skip_from) {
        for (size_t i = static_cast<size_t>(skip_from); i < used.size(); i++) if (x0 <= used[i].x1 && x1 >= used[i].x0 && z0 <= used[i].z1 && z1 >= used[i].z0) return true;
        return false;
    };
    const int LIMIT = 70;
    // pieces are placed along the x or z axis from a crossing; dir 0..3 = +x, +z, -x, -z
    static const int DX[4] = {1, 0, -1, 0}, DZ[4] = {0, 1, 0, -1};
    struct Node { int x, z, dir, depth; };
    std::vector<Node> open;
    auto crossing = [&](int x, int z) {
        s->add(x - 3, Y - 1, z - 3, x + 3, Y - 1, z + 3, NB);
        s->add(x - 3, Y, z - 3, x + 3, Y + 6, z + 3, 0);
        s->add(x - 3, Y - 12, z - 3, x - 3, Y - 2, z - 3, NB, P_FOUND); s->add(x + 3, Y - 12, z - 3, x + 3, Y - 2, z - 3, NB, P_FOUND);
        s->add(x - 3, Y - 12, z + 3, x - 3, Y - 2, z + 3, NB, P_FOUND); s->add(x + 3, Y - 12, z + 3, x + 3, Y - 2, z + 3, NB, P_FOUND);
        for (int dx : {-3, 3}) for (int dz : {-3, 3}) s->add(x + dx, Y, z + dz, x + dx, Y + 3, z + dz, NB);
        s->add(x - 2, Y, z - 3, x + 2, Y, z - 3, FX); s->add(x - 2, Y, z + 3, x + 2, Y, z + 3, FX);
        s->add(x - 3, Y, z - 2, x - 3, Y, z + 2, FZ); s->add(x + 3, Y, z - 2, x + 3, Y, z + 2, FZ);
        used.push_back({x - 4, z - 4, x + 4, z + 4});
    };
    auto bridge = [&](int x, int z, int dir, int len, bool roofed) {
        // from the crossing edge outwards
        int x0 = x + DX[dir] * 4, z0 = z + DZ[dir] * 4;
        int x1 = x + DX[dir] * (3 + len), z1 = z + DZ[dir] * (3 + len);
        int ax0 = std::min(x0, x1), ax1 = std::max(x0, x1), az0 = std::min(z0, z1), az1 = std::max(z0, z1);
        bool alongx = DX[dir] != 0;
        int hw = 2;   // half width
        int bx0 = ax0 - (alongx ? 0 : hw), bx1 = ax1 + (alongx ? 0 : hw), bz0 = az0 - (alongx ? hw : 0), bz1 = az1 + (alongx ? hw : 0);
        s->add(bx0, Y - 1, bz0, bx1, Y - 1, bz1, NB);
        if (roofed) {
            s->add(bx0, Y, bz0, bx1, Y + 4, bz1, NB);
            s->add(alongx ? bx0 : bx0 + 1, Y, alongx ? bz0 + 1 : bz0, alongx ? bx1 : bx1 - 1, Y + 3, alongx ? bz1 - 1 : bz1, 0);
            // windows
            for (int t = 3; t < len - 3; t += 4) {
                int wx = alongx ? ax0 + t : bx0, wz = alongx ? bz0 : az0 + t;
                s->add(wx, Y + 2, wz, alongx ? wx : bx0, Y + 2, alongx ? wz : wz, NBF_);
                s->add(alongx ? wx : bx1, Y + 2, alongx ? bz1 : wz, alongx ? wx : bx1, Y + 2, alongx ? bz1 : wz, NBF_);
            }
        } else {
            s->add(bx0, Y, bz0, bx1, Y + 5, bz1, 0);
            if (alongx) { s->add(bx0, Y, bz0, bx1, Y, bz0, FX); s->add(bx0, Y, bz1, bx1, Y, bz1, FX); }
            else { s->add(bx0, Y, bz0, bx0, Y, bz1, FZ); s->add(bx1, Y, bz0, bx1, Y, bz1, FZ); }
        }
        for (int t = 4; t < len; t += 8) {   // supports down to whatever is below
            int px = alongx ? ax0 + t : x, pz = alongx ? z : az0 + t;
            for (int off : {-hw, hw}) {
                int sx = alongx ? px : px + off, sz = alongx ? pz + off : pz;
                s->add(sx, Y - 14, sz, sx, Y - 2, sz, NB, P_FOUND);
            }
        }
        used.push_back({bx0, bz0, bx1, bz1});
    };
    crossing(X, Z);
    open.push_back({X, Z, 0, 0});
    open.push_back({X, Z, 1, 0});
    open.push_back({X, Z, 2, 0});
    open.push_back({X, Z, 3, 0});
    int pieces = 0;
    while (!open.empty() && pieces < 40) {
        size_t pick = static_cast<size_t>(rndi(rs, static_cast<int>(open.size())));
        Node n = open[pick];
        open.erase(open.begin() + static_cast<long>(pick));
        if (rnd01(rs) < 0.25 && n.depth > 0) continue;
        int len = 10 + rndi(rs, 14);
        int ex = n.x + DX[n.dir] * (4 + len + 3), ez = n.z + DZ[n.dir] * (4 + len + 3);
        if (std::abs(ex - X) > LIMIT || std::abs(ez - Z) > LIMIT) continue;
        int from = static_cast<int>(used.size());
        (void)from;
        int sx0 = std::min(n.x + DX[n.dir] * 4, ex) - 2, sx1 = std::max(n.x + DX[n.dir] * 4, ex) + 2, sz0 = std::min(n.z + DZ[n.dir] * 4, ez) - 2, sz1 = std::max(n.z + DZ[n.dir] * 4, ez) + 2;
        if (overlaps(sx0, sz0, sx1, sz1, 1)) continue;
        bool roofed = rnd01(rs) < 0.45;
        bridge(n.x, n.z, n.dir, len, roofed);
        int endx = n.x + DX[n.dir] * (4 + len + 3), endz = n.z + DZ[n.dir] * (4 + len + 3);
        crossing(endx, endz);
        pieces++;
        if (n.depth < 5) {
            int d1 = (n.dir + 1) & 3, d2 = (n.dir + 3) & 3;
            if (rnd01(rs) < 0.7) open.push_back({endx, endz, n.dir, n.depth + 1});
            if (rnd01(rs) < 0.6) open.push_back({endx, endz, d1, n.depth + 1});
            if (rnd01(rs) < 0.6) open.push_back({endx, endz, d2, n.depth + 1});
        }
        // a nether wart room or a tower on some crossings
        double r = rnd01(rs);
        if (r < 0.15) {
            int rx = endx, rz = endz;
            s->add(rx - 3, Y, rz - 3, rx + 3, Y, rz + 3, SOUL);
            s->add(rx - 2, Y + 1, rz - 2, rx + 2, Y + 1, rz + 2, WART);
        } else if (r < 0.3) {
            s->add(endx - 2, Y, endz - 2, endx + 2, Y + 12, endz + 2, RED);
            s->add(endx - 1, Y, endz - 1, endx + 1, Y + 11, endz + 1, 0);
            s->add(endx - 2, Y + 13, endz - 2, endx + 2, Y + 13, endz + 2, CHISEL);
        }
    }
    // a lava-free floor under the whole thing is not built: the terrain below stays as it is
    (void)LAVA;
    return s;
}

// ---- end city ---------------------------------------------------------------------------------------------------

std::unique_ptr<Structure> build_end_city(const WorldGen& g, int cx, int cz) {
    int X = cx * 16 + 8, Z = cz * 16 + 8, bm;
    int h = g.sample(X, Z, bm);
    if (h < 0 || (bm != END_MIDLANDS && bm != END_HIGHLANDS)) return nullptr;
    int hmin = h, hmax = h;
    for (int dx : {-6, 6}) for (int dz : {-6, 6}) {
        int b2, hh = g.sample(X + dx, Z + dz, b2);
        if (hh < 0) return nullptr;
        hmin = std::min(hmin, hh); hmax = std::max(hmax, hh);
    }
    if (hmax - hmin > 4) return nullptr;
    auto s = std::make_unique<Structure>();
    uint64_t rs = hash2(g.seed() ^ 0x656e6463, cx, cz);
    Builder b(*s, X, hmax, Z, rndi(rs, 4));
    const uint16_t PUR = sb("purpur_block"), PIL = sb("purpur_pillar", "axis=y"), BRICK = sb("end_stone_bricks"), GLASS = sb("magenta_stained_glass"), ROD = sb("end_rod", "facing=up");
    const uint16_t SLAB = sb("purpur_slab", "type=bottom,waterlogged=false");
    b.box(-6, -6, -6, 6, 0, 6, BRICK, P_FOUND);
    // base house 9x9 with two floors, a tower on top and a rod-lit roof
    b.box(-4, 1, -4, 4, 1, 4, PUR);
    b.walls(-4, 2, -4, 4, 6, 4, BRICK);
    for (int dx : {-4, 4}) for (int dz : {-4, 4}) b.box(dx, 1, dz, dx, 8, dz, PIL);
    b.air(-3, 2, -3, 3, 6, 3);
    b.box(-3, 4, -3, 3, 4, 3, PUR);
    b.box(-1, 4, -1, 1, 4, 1, 0);   // stairwell
    for (int x = -3; x <= 3; x += 2) { b.set(x, 3, -4, GLASS); b.set(x, 6, -4, GLASS); b.set(x, 3, 4, GLASS); b.set(x, 6, 4, GLASS); }
    b.air(0, 2, 4, 0, 3, 4);   // door
    b.box(-4, 7, -4, 4, 7, 4, PUR);
    b.box(-4, 8, -4, 4, 8, 4, SLAB);
    b.box(-2, 7, -2, 2, 14, 2, BRICK);
    b.air(-1, 8, -1, 1, 13, 1);
    b.box(-2, 15, -2, 2, 15, 2, PUR);
    b.set(0, 16, 0, ROD);
    for (int dx : {-2, 2}) for (int dz : {-2, 2}) b.set(dx, 16, dz, ROD);
    b.set(0, 10, 2, GLASS);
    return s;
}

}  // namespace pn
