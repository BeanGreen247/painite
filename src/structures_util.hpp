// Helpers for the structure builders: block lookup by name, and a box builder that works in a rotated local frame.
#pragma once
#include "blocks.hpp"
#include "log.hpp"
#include "structures.hpp"
#include "worldgen.hpp"
#include "worldgen_util.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace pn {

// Block state by name and properties ("facing=north,half=bottom"); properties must be in alphabetical order like in the block data.
inline uint16_t sb(const char* name, const char* props = "") {
    const Blocks& B = Blocks::get();
    std::string n = std::string("minecraft:") + name;
    if (!B.known(n)) {
        static std::mutex m;
        static std::set<std::string> seen;
        std::lock_guard<std::mutex> l(m);
        if (seen.insert(n).second) warn("structure block " + n + " is not in the block data");
        return 0;
    }
    return B.find(n, props);
}

// Builder in a local frame: (lx, ly, lz) -> world through an origin and a rotation of 0..3 quarter turns (clockwise seen from above).
struct Builder {
    Structure& s;
    int ox, oy, oz, rot;
    Builder(Structure& st, int x, int y, int z, int r) : s(st), ox(x), oy(y), oz(z), rot(r & 3) {}
    void tr(int lx, int lz, int& wx, int& wz) const {
        switch (rot) {
            case 0: wx = ox + lx; wz = oz + lz; break;
            case 1: wx = ox - lz; wz = oz + lx; break;
            case 2: wx = ox - lx; wz = oz - lz; break;
            default: wx = ox + lz; wz = oz - lx; break;
        }
    }
    // direction names rotated with the frame
    const char* facing(const char* f) const {
        static const char* order[4] = {"north", "east", "south", "west"};
        for (int i = 0; i < 4; i++) if (!strcmp(order[i], f)) return order[(i + rot) & 3];
        return f;
    }
    void box(int x0, int y0, int z0, int x1, int y1, int z1, uint16_t b, PrimMode m = P_FILL) {
        int ax, az, bx, bz;
        tr(x0, z0, ax, az);
        tr(x1, z1, bx, bz);
        s.add(std::min(ax, bx), oy + std::min(y0, y1), std::min(az, bz), std::max(ax, bx), oy + std::max(y0, y1), std::max(az, bz), b, m);
    }
    void set(int x, int y, int z, uint16_t b, PrimMode m = P_FILL) { box(x, y, z, x, y, z, b, m); }
    void air(int x0, int y0, int z0, int x1, int y1, int z1) { box(x0, y0, z0, x1, y1, z1, 0); }
    // walls and floor/ceiling of a box; the inside is left alone
    void shell(int x0, int y0, int z0, int x1, int y1, int z1, uint16_t b) {
        box(x0, y0, z0, x1, y0, z1, b); box(x0, y1, z0, x1, y1, z1, b);
        box(x0, y0, z0, x0, y1, z1, b); box(x1, y0, z0, x1, y1, z1, b);
        box(x0, y0, z0, x1, y1, z0, b); box(x0, y0, z1, x1, y1, z1, b);
    }
    void walls(int x0, int y0, int z0, int x1, int y1, int z1, uint16_t b) {
        box(x0, y0, z0, x0, y1, z1, b); box(x1, y0, z0, x1, y1, z1, b);
        box(x0, y0, z0, x1, y1, z0, b); box(x0, y0, z1, x1, y1, z1, b);
    }
};

inline void Structure::add(int x0, int y0, int z0, int x1, int y1, int z1, uint16_t b, PrimMode m) {
    if (prims.empty()) { minx = x0; minz = z0; maxx = x1; maxz = z1; }
    minx = std::min(minx, x0); minz = std::min(minz, z0); maxx = std::max(maxx, x1); maxz = std::max(maxz, z1);
    prims.push_back({x0, y0, z0, x1, y1, z1, b, m});
}

// builders (structures_*.cpp); null when the spot does not suit the structure
std::unique_ptr<Structure> build_village(const WorldGen& g, int cx, int cz);
std::unique_ptr<Structure> build_stronghold(const WorldGen& g, int cx, int cz);
std::unique_ptr<Structure> build_desert_temple(const WorldGen& g, int cx, int cz);
std::unique_ptr<Structure> build_ruined_portal(const WorldGen& g, int dim, int cx, int cz);
std::unique_ptr<Structure> build_fortress(const WorldGen& g, int cx, int cz);
std::unique_ptr<Structure> build_end_city(const WorldGen& g, int cx, int cz);

}  // namespace pn
