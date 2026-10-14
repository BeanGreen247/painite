// Villages: a well, four streets with lamp posts, houses and farms along them, in the materials of the surrounding biome.
#include "structures_util.hpp"
#include "worldgen_biomes.hpp"
#include <array>

namespace pn {

namespace {

struct Mat {
    const char *planks, *log, *stairs, *cobble, *fence, *path;
};

const Mat* material_for(int bm) {
    static const Mat oak = {"oak_planks", "oak_log", "oak_stairs", "cobblestone", "oak_fence", "dirt_path"};
    static const Mat sand = {"cut_sandstone", "chiseled_sandstone", "sandstone_stairs", "sandstone", "birch_fence", "smooth_sandstone"};
    static const Mat acacia = {"acacia_planks", "acacia_log", "acacia_stairs", "cobblestone", "acacia_fence", "dirt_path"};
    static const Mat spruce = {"spruce_planks", "spruce_log", "spruce_stairs", "cobblestone", "spruce_fence", "dirt_path"};
    switch (bm) {
        case PLAINS: case MEADOW: return &oak;
        case DESERT: return &sand;
        case SAVANNA: return &acacia;
        case TAIGA: case SNOWY_TAIGA: case SNOWY_PLAINS: return &spruce;
        default: return nullptr;
    }
}

}  // namespace

std::unique_ptr<Structure> build_village(const WorldGen& g, int cx, int cz) {
    int X = cx * 16 + 8, Z = cz * 16 + 8, bm;
    int h0 = g.sample(X, Z, bm);
    const Mat* M = material_for(bm);
    if (!M || h0 < 63 || h0 > 110) return nullptr;
    auto s = std::make_unique<Structure>();
    uint64_t rs = hash2(g.seed() ^ 0x76696c6c, cx, cz);
    const uint16_t PLANKS = sb(M->planks), COBBLE = sb(M->cobble), PATH = sb(M->path), FENCE = sb(M->fence, "east=false,north=false,south=false,waterlogged=false,west=false");
    const uint16_t WATER = sb("water", "level=0"), TORCH = sb("torch"), GLASS = sb("glass"), FARM = sb("farmland", "moisture=7");
    const bool desert = bm == DESERT;

    auto ground = [&](int x, int z, int& h) { int b2; h = g.sample(x, z, b2); return h >= 63 && b2 != RIVER && b2 != FROZEN_RIVER && !(b2 >= OCEAN && b2 <= WARM); };

    // ---- the well at the centre
    {
        Builder b(*s, X, h0, Z, 0);
        b.box(-2, -6, -2, 2, -1, 2, COBBLE, P_FOUND);
        b.air(-2, 1, -2, 2, 7, 2);
        b.box(-2, 0, -2, 2, 1, 2, COBBLE);
        b.box(-1, -3, -1, 1, 0, 1, WATER);
        b.air(-1, 1, -1, 1, 1, 1);
        for (int dx : {-2, 2}) for (int dz : {-2, 2}) { b.box(dx, 2, dz, dx, 3, dz, FENCE); }
        b.box(-2, 4, -2, 2, 4, 2, PLANKS);
    }
    // ---- streets: four arms, every 4 blocks one patch of path that follows the ground
    struct Arm { int dx, dz, len; };
    Arm arms[4] = {{1, 0, 24 + rndi(rs, 17)}, {-1, 0, 24 + rndi(rs, 17)}, {0, 1, 24 + rndi(rs, 17)}, {0, -1, 24 + rndi(rs, 17)}};
    for (Arm& a : arms) {
        for (int t = 3; t < a.len; t += 4) {
            int px = X + a.dx * t, pz = Z + a.dz * t, h;
            if (!ground(px, pz, h)) { a.len = t; break; }
            int hx = a.dx ? 3 : 1, hz = a.dz ? 3 : 1;   // patch is 4 long along the arm, 3 wide across
            int x0 = px - (a.dx ? 0 : 1), z0 = pz - (a.dz ? 0 : 1);
            s->add(x0, h - 5, z0, x0 + (a.dx ? hx : 2), h + 7, z0 + (a.dz ? hz : 2), PATH, P_SURFACE);
        }
    }
    std::vector<std::array<int, 3>> taken;   // building centres (x, z, radius)
    taken.push_back({X, Z, 5});
    auto free_spot = [&](int x, int z, int r) {
        for (auto& t : taken) if (std::abs(t[0] - x) < t[2] + r + 1 && std::abs(t[1] - z) < t[2] + r + 1) return false;
        return true;
    };
    int built = 0;
    for (Arm& a : arms) {
        for (int t = 9; t + 4 < a.len && built < 26; t += 7 + rndi(rs, 4)) {
            for (int side = -1; side <= 1; side += 2) {
                if (rnd01(rs) < 0.3) continue;
                // a building centre beside the street; its door faces the street
                double kind = rnd01(rs);
                int hw = kind < 0.35 ? 2 : kind < 0.6 ? 3 : kind < 0.8 ? 4 : 4, hd = kind < 0.35 ? 2 : kind < 0.6 ? 3 : kind < 0.8 ? 3 : 4;
                bool farm = kind >= 0.8;
                int off = hd + 3;            // door side is 2 blocks away from the street's edge
                int sx = a.dx ? X + a.dx * t : X + side * off, sz = a.dz ? Z + a.dz * t : Z + side * off;
                // the street direction decides how the building turns: local +z (the door side) must face the street
                int rot;
                if (a.dx) rot = side < 0 ? 0 : 2;     // street along x, building north (side -1) faces south
                else rot = side < 0 ? 3 : 1;          // street along z, building west faces east
                if (a.dx) sz = Z + side * off; else sx = X + side * off;
                int h;
                if (!ground(sx, sz, h)) continue;
                int hmin = h, hmax = h, hh;
                bool ok = true;
                for (int dx : {-hw, hw}) for (int dz : {-hd, hd}) { if (!ground(sx + dx, sz + dz, hh)) ok = false; hmin = std::min(hmin, hh); hmax = std::max(hmax, hh); }
                if (!ok || hmax - hmin > 3 || !free_spot(sx, sz, std::max(hw, hd))) continue;
                int base = hmax;   // build on the highest corner so nothing sinks into the hill
                taken.push_back({sx, sz, std::max(hw, hd)});
                built++;
                Builder b(*s, sx, base, sz, rot);
                // path from the door to the street
                b.box(0, -4, hd + 1, 1, 6, hd + 2 + 1, PATH, P_SURFACE);
                if (farm) {
                    b.box(-hw, -4, -hd, hw, -1, hd, sb("dirt"), P_FOUND);
                    b.air(-hw, 1, -hd, hw, 5, hd);
                    b.box(-hw, 0, -hd, hw, 0, hd, FARM);
                    b.walls(-hw, 0, -hd, hw, 0, hd, sb(M->log, "axis=y"));
                    b.box(0, -3, -hd + 1, 0, 0, hd - 1, WATER);
                    b.box(0, 0, -hd + 1, 0, 0, hd - 1, WATER);
                    for (int x = -hw + 1; x <= hw - 1; x++) {
                        if (x == 0) continue;
                        std::string age = std::string("age=") + std::to_string(5 + rndi(rs, 3));
                        b.box(x, 1, -hd + 1, x, 1, hd - 1, sb("wheat", age.c_str()));
                    }
                    b.set(-hw, 1, -hd, FENCE); b.set(hw, 1, hd, FENCE); b.set(-hw, 2, -hd, TORCH); b.set(hw, 2, hd, TORCH);
                    continue;
                }
                int wallh = 3 + (hw >= 4 ? 1 : 0);
                b.box(-hw, -8, -hd, hw, -1, hd, COBBLE, P_FOUND);
                b.air(-hw, 1, -hd, hw, wallh + hw + 3, hd);
                b.box(-hw, 0, -hd, hw, 0, hd, PLANKS);
                b.walls(-hw, 1, -hd, hw, wallh, hd, PLANKS);
                for (int dx : {-hw, hw}) for (int dz : {-hd, hd}) b.box(dx, 1, dz, dx, wallh, dz, sb(M->log, "axis=y"));
                // windows on the long walls and the back, the door in the front wall
                b.set(-hw, 2, 0, GLASS); b.set(hw, 2, 0, GLASS); b.set(0, 2, -hd, GLASS);
                if (hw >= 3) { b.set(-hw, 2, hd - 2 > 0 ? hd - 2 : -1, GLASS); b.set(hw, 2, hd - 2 > 0 ? hd - 2 : -1, GLASS); }
                std::string door_lo = std::string("facing=") + b.facing("south") + ",half=lower,hinge=left,open=false,powered=false";
                std::string door_hi = std::string("facing=") + b.facing("south") + ",half=upper,hinge=left,open=false,powered=false";
                const char* door = desert ? "birch_door" : (M->planks[0] == 'a' ? "acacia_door" : M->planks[0] == 's' && M->planks[1] == 'p' ? "spruce_door" : "oak_door");
                b.air(0, 1, hd, 0, 2, hd);
                b.set(0, 1, hd, sb(door, door_lo.c_str()));
                b.set(0, 2, hd, sb(door, door_hi.c_str()));
                // gabled roof: stairs run along z, the ridge sits over the middle
                for (int i = 0; i <= hw; i++) {
                    int y = wallh + 1 + i;
                    if (-hw + i < hw - i) {
                        std::string west = std::string("facing=") + b.facing("west") + ",half=bottom,shape=straight,waterlogged=false";
                        std::string east = std::string("facing=") + b.facing("east") + ",half=bottom,shape=straight,waterlogged=false";
                        b.box(-hw + i, y, -hd - 1, -hw + i, y, hd + 1, sb(M->stairs, west.c_str()));
                        b.box(hw - i, y, -hd - 1, hw - i, y, hd + 1, sb(M->stairs, east.c_str()));
                        if (-hw + i + 1 <= hw - i - 1) { b.box(-hw + i + 1, y, -hd, hw - i - 1, y, -hd, PLANKS); b.box(-hw + i + 1, y, hd, hw - i - 1, y, hd, PLANKS); }
                    } else {
                        b.box(-hw + i, y, -hd - 1, hw - i, y, hd + 1, PLANKS);
                    }
                }
                // a little furniture and light
                b.set(-hw + 1, 1, -hd + 1, sb("crafting_table"));
                b.set(hw - 1, 1, -hd + 1, sb("furnace", (std::string("facing=") + b.facing("south") + ",lit=false").c_str()));
                b.set(0, 3, -hd + 1, TORCH, P_SOFT);
                b.set(hw - 1, 3, hd - 1, sb("wall_torch", (std::string("facing=") + b.facing("north")).c_str()), P_SOFT);
            }
        }
    }
    // lamp posts along the streets
    for (Arm& a : arms)
        for (int t = 12; t < a.len; t += 12) {
            int px = X + a.dx * t + (a.dz ? 2 : 0), pz = Z + a.dz * t + (a.dx ? 2 : 0), h;
            if (!ground(px, pz, h)) continue;
            s->add(px, h + 1, pz, px, h + 3, pz, FENCE);
            s->add(px, h + 4, pz, px, h + 4, pz, TORCH);
        }
    if (built < 3) return nullptr;
    return s;
}

}  // namespace pn
