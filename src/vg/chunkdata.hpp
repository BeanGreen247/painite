// The blocks of one chunk while it is generated: a flat array over the generated height range, the heightmaps kept live like ProtoChunk does, and the fluid positions marked for post-processing.
#pragma once
#include <cstdint>
#include <vector>

namespace pn::vg {

// Heightmap.Types
enum HeightmapType { WORLD_SURFACE_WG = 0, WORLD_SURFACE = 1, OCEAN_FLOOR_WG = 2, OCEAN_FLOOR = 3, MOTION_BLOCKING = 4, MOTION_BLOCKING_NO_LEAVES = 5, kHeightmapTypes = 6 };

// per block id flag bits the chunk needs
enum : uint8_t { kFlagAir = 1, kFlagFluid = 2, kFlagBlocksMotion = 4, kFlagLeaves = 8 };

inline bool heightmap_opaque(HeightmapType t, uint8_t f) {
    switch (t) {
        case WORLD_SURFACE_WG: case WORLD_SURFACE: return !(f & kFlagAir);
        case OCEAN_FLOOR_WG: case OCEAN_FLOOR: return f & kFlagBlocksMotion;
        case MOTION_BLOCKING: return (f & kFlagBlocksMotion) || (f & kFlagFluid);
        default: return ((f & kFlagBlocksMotion) || (f & kFlagFluid)) && !(f & kFlagLeaves);
    }
}

struct ChunkBlocks {
    int min_y = 0, height = 0;
    std::vector<uint16_t> b;          // [(y - min_y) * 256 + z * 16 + x]
    std::vector<uint8_t> marks;       // 1 where a fluid needs a tick after generation
    int hm[kHeightmapTypes][256];     // first free y per column and heightmap (Heightmap.getFirstAvailable)
    uint8_t hm_active = 0;            // bit per HeightmapType that is kept up to date
    const uint8_t* flags = nullptr;   // per block id: kFlag*

    bool air(uint16_t id) const { return flags[id] & kFlagAir; }
    bool fluid(uint16_t id) const { return flags[id] & kFlagFluid; }
    bool inside(int y) const { return y >= min_y && y < min_y + height; }
    size_t index(int x, int y, int z) const { return size_t(y - min_y) * 256 + size_t(z & 15) * 16 + size_t(x & 15); }
    uint16_t get(int x, int y, int z) const { return inside(y) ? b[index(x, y, z)] : 0; }   // outside the range reads as air
    int first_free(HeightmapType t, int x, int z) const { return hm[t][(z & 15) * 16 + (x & 15)]; }

    void init(int miny, int h, std::vector<uint16_t>&& blocks, const uint8_t* f) {
        min_y = miny; height = h; b = std::move(blocks); flags = f;
        marks.assign(b.size(), 0);
        hm_active = 0;
        prime(WORLD_SURFACE_WG);
        prime(OCEAN_FLOOR_WG);
    }
    // Heightmap.primeHeightmaps: from now on the heightmap follows set()
    void prime(HeightmapType t) {
        if (hm_active & (1 << t)) return;
        hm_active |= uint8_t(1 << t);
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) recompute(t, x, z, min_y + height - 1);
    }
    void prime_final() { for (int t = WORLD_SURFACE; t < kHeightmapTypes; t++) prime(HeightmapType(t)); }

    // sets a block (ignored outside the range) and updates the active heightmaps like Heightmap.update
    void set(int x, int y, int z, uint16_t id, bool mark_fluid = true) {
        if (!inside(y)) return;
        size_t i = index(x, y, z);
        b[i] = id;
        if (mark_fluid && fluid(id)) marks[i] = 1;
        touch_heightmaps(x, y, z, id);
    }
    // a write that bypasses the heightmaps (what feature code does through LevelChunkSection.setBlockState)
    void set_raw(int x, int y, int z, uint16_t id) { if (inside(y)) b[index(x, y, z)] = id; }

    void touch_heightmaps(int x, int y, int z, uint16_t id) {
        uint8_t f = flags[id];
        for (int t = 0; t < kHeightmapTypes; t++) {
            if (!(hm_active & (1 << t))) continue;
            int& h = hm[t][(z & 15) * 16 + (x & 15)];
            if (y <= h - 2) continue;
            if (heightmap_opaque(HeightmapType(t), f)) { if (y >= h) h = y + 1; }
            else if (h - 1 == y) recompute(HeightmapType(t), x & 15, z & 15, y - 1);
        }
    }
private:
    void recompute(HeightmapType t, int x, int z, int from_y) {
        int h = min_y;
        for (int y = from_y; y >= min_y; y--)
            if (heightmap_opaque(t, flags[b[index(x, y, z)]])) { h = y + 1; break; }
        hm[t][z * 16 + x] = h;
    }
};

}  // namespace pn::vg
