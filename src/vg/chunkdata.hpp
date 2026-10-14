// The blocks of one chunk while it is generated: a flat array over the generated height range, the WORLD_SURFACE_WG heightmap kept live, and the fluid positions marked for post-processing.
#pragma once
#include <cstdint>
#include <vector>

namespace pn::vg {

struct ChunkBlocks {
    int min_y = 0, height = 0;
    std::vector<uint16_t> b;          // [(y - min_y) * 256 + z * 16 + x]
    std::vector<uint8_t> marks;       // 1 where a fluid needs a tick after generation
    int surface[256];                 // WORLD_SURFACE_WG: first free y per column (top non-air y + 1)
    const uint8_t* flags = nullptr;   // per block id: bit 0 air, bit 1 fluid

    bool air(uint16_t id) const { return flags[id] & 1; }
    bool fluid(uint16_t id) const { return flags[id] & 2; }
    bool inside(int y) const { return y >= min_y && y < min_y + height; }
    size_t index(int x, int y, int z) const { return size_t(y - min_y) * 256 + size_t(z & 15) * 16 + size_t(x & 15); }
    uint16_t get(int x, int y, int z) const { return inside(y) ? b[index(x, y, z)] : 0; }   // outside the range reads as air
    void init(int miny, int h, std::vector<uint16_t>&& blocks, const uint8_t* f) {
        min_y = miny; height = h; b = std::move(blocks); flags = f;
        marks.assign(b.size(), 0);
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) recompute(x, z, min_y + height - 1);
    }
    // sets a block (ignored outside the range) and keeps the heightmap like Heightmap.update does
    void set(int x, int y, int z, uint16_t id, bool mark_fluid = true) {
        if (!inside(y)) return;
        size_t i = index(x, y, z);
        b[i] = id;
        if (mark_fluid && fluid(id)) marks[i] = 1;
        int& h = surface[(z & 15) * 16 + (x & 15)];
        if (y <= h - 2) return;
        if (!air(id)) { if (y >= h) h = y + 1; }
        else if (h - 1 == y) recompute(x & 15, z & 15, y - 1);
    }
private:
    void recompute(int x, int z, int from_y) {
        int h = min_y;
        for (int y = from_y; y >= min_y; y--)
            if (!air(b[index(x, y, z)])) { h = y + 1; break; }
        surface[z * 16 + x] = h;
    }
};

}  // namespace pn::vg
