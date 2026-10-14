// Strongholds: an underground complex of stone brick halls around the room with the End portal frame.
#include "structures_util.hpp"

namespace pn {

std::unique_ptr<Structure> build_stronghold(const WorldGen& g, int cx, int cz) {
    auto s = std::make_unique<Structure>();
    uint64_t rs = hash2(g.seed() ^ 0x5742, cx, cz);
    int X = cx * 16 + 8, Z = cz * 16 + 8;
    int bm;
    int h = g.sample(X, Z, bm);
    int y0 = std::clamp(h - 26, -44, 34);
    int rot = rndi(rs, 4);
    Builder B(*s, X, y0, Z, rot);
    const uint16_t BR = sb("stone_bricks"), MOSS = sb("mossy_stone_bricks"), CRACK = sb("cracked_stone_bricks"), CHISEL = sb("chiseled_stone_bricks"), AIR = 0;
    const uint16_t LAVA = sb("lava", "level=0"), BARS = sb("iron_bars", "east=false,north=false,south=false,waterlogged=false,west=false"), SHELF = sb("bookshelf");

    // stone bricks, a few of them mossy or cracked
    auto weather = [&](int x0, int y0_, int z0, int x1, int y1, int z1, int n) {
        for (int i = 0; i < n; i++) {
            int x = x0 + rndi(rs, x1 - x0 + 1), y = y0_ + rndi(rs, y1 - y0_ + 1), z = z0 + rndi(rs, z1 - z0 + 1);
            B.set(x, y, z, rnd01(rs) < 0.5 ? MOSS : CRACK);
        }
    };
    // a hall: brick shell with an air inside (coordinates are the inside)
    auto hall = [&](int x0, int y0_, int z0, int x1, int y1, int z1) {
        B.shell(x0 - 1, y0_ - 1, z0 - 1, x1 + 1, y1 + 1, z1 + 1, BR);
        B.air(x0, y0_, z0, x1, y1, z1);
        weather(x0 - 1, y0_ - 1, z0 - 1, x1 + 1, y1 + 1, z1 + 1, (x1 - x0 + z1 - z0) / 2 + 4);
    };

    // portal room: inside x 0..10, z 0..14, y 0..7 around the start, entered from the south (z = -1)
    const int ox = -5, oz = -7;   // the room is centred on the start
    hall(ox, 0, oz, ox + 10, 7, oz + 14);
    // lava pool in front of the platform, platform with the frame, stairs up to it
    B.box(ox + 2, -1, oz + 2, ox + 8, -1, oz + 5, LAVA);
    B.box(ox + 3, 0, oz + 7, ox + 7, 2, oz + 11, BR);
    const std::string stair = std::string("facing=") + B.facing("north") + ",half=bottom,shape=straight,waterlogged=false";
    for (int x = ox + 3; x <= ox + 7; x++) B.set(x, 2, oz + 6, sb("stone_brick_stairs", stair.c_str()));
    // the frame: 12 blocks around a 3x3 hole, each facing the middle; about one in ten holds an eye
    auto frame = [&](bool eye, const char* dir) { return sb("end_portal_frame", ((eye ? "eye=true,facing=" : "eye=false,facing=") + std::string(B.facing(dir))).c_str()); };
    for (int i = 0; i < 3; i++) {
        bool e1 = rnd01(rs) < 0.1, e2 = rnd01(rs) < 0.1, e3 = rnd01(rs) < 0.1, e4 = rnd01(rs) < 0.1;
        B.set(ox + 4 + i, 3, oz + 7, frame(e1, "south"));
        B.set(ox + 4 + i, 3, oz + 11, frame(e2, "north"));
        B.set(ox + 3, 3, oz + 8 + i, frame(e3, "east"));
        B.set(ox + 7, 3, oz + 8 + i, frame(e4, "west"));
    }
    B.box(ox + 4, 3, oz + 8, ox + 6, 3, oz + 10, AIR);
    B.set(ox + 5, 1, oz + 9, sb("spawner"));
    // entrance and the corridor south of it
    B.air(ox + 4, 0, oz - 1, ox + 6, 2, oz - 1);
    auto corridor_z = [&](int x, int zfrom, int zto) {   // 3 wide, 3 high, along z (zfrom > zto walks south)
        int z0 = std::min(zfrom, zto), z1 = std::max(zfrom, zto);
        B.shell(x - 2, -1, z0, x + 2, 4, z1, BR);
        B.air(x - 1, 0, z0, x + 1, 3, z1);
        weather(x - 2, -1, z0, x + 2, 4, z1, (z1 - z0) / 3 + 2);
        const std::string torch = std::string("facing=") + B.facing("east");
        for (int z = z0 + 2; z < z1; z += 5) B.set(x - 1, 2, z, sb("wall_torch", torch.c_str()));
    };
    auto corridor_x = [&](int z, int xfrom, int xto) {
        int x0 = std::min(xfrom, xto), x1 = std::max(xfrom, xto);
        B.shell(x0, -1, z - 2, x1, 4, z + 2, BR);
        B.air(x0, 0, z - 1, x1, 3, z + 1);
        weather(x0, -1, z - 2, x1, 4, z + 2, (x1 - x0) / 3 + 2);
    };
    int cxm = ox + 5;                       // corridor axis
    corridor_z(cxm, oz - 2, oz - 20);
    // crossing hall 7 x 7, pillars in the corners
    int hz1 = oz - 21, hz0 = oz - 27;
    hall(cxm - 3, 0, hz0, cxm + 3, 4, hz1);
    for (int dx : {-2, 2}) for (int dz : {1, 5}) B.box(cxm + dx, 0, hz0 + dz, cxm + dx, 4, hz0 + dz, CHISEL);
    // west and east wings, each ending in a library-like room
    int wz = hz0 + 3;
    corridor_x(wz, cxm - 4, cxm - 18);
    hall(cxm - 26, 0, wz - 3, cxm - 19, 5, wz + 3);
    for (int z = wz - 2; z <= wz + 2; z++) { B.box(cxm - 25, 0, z, cxm - 25, 3, z, SHELF); }
    B.box(cxm - 24, 0, wz - 3, cxm - 20, 0, wz - 3, SHELF);
    corridor_x(wz, cxm + 4, cxm + 18);
    hall(cxm + 19, 0, wz - 3, cxm + 25, 4, wz + 3);
    B.box(cxm + 22, 0, wz - 3, cxm + 22, 1, wz + 3, BARS);
    // further south
    corridor_z(cxm, hz0 - 2, hz0 - 18);
    hall(cxm - 4, 0, hz0 - 27, cxm + 4, 4, hz0 - 19);
    { const std::string ch = std::string("facing=") + B.facing("north") + ",type=single,waterlogged=false"; B.set(cxm, 0, hz0 - 23, sb("chest", ch.c_str())); }
    return s;
}

}  // namespace pn
