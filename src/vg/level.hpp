// What features see while they generate: the 3x3 chunks around the centre chunk (Mojang's WorldGenRegion), with the write radius, heightmaps and biome lookup.
#pragma once
#include "blockdata.hpp"
#include "chunkdata.hpp"
#include "values.hpp"
#include <functional>

namespace pn::vg {

struct BlockPos {
    int x = 0, y = 0, z = 0;
    BlockPos() = default;
    BlockPos(int x_, int y_, int z_) : x(x_), y(y_), z(z_) {}
    BlockPos offset(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
    BlockPos above(int n = 1) const { return {x, y + n, z}; }
    BlockPos below(int n = 1) const { return {x, y - n, z}; }
    bool operator==(const BlockPos& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const BlockPos& o) const { return !(*this == o); }
    bool operator<(const BlockPos& o) const { return y != o.y ? y < o.y : z != o.z ? z < o.z : x < o.x; }
    int hash() const { return (y + z * 31) * 31 + x; }   // Vec3i.hashCode
};

// Direction order of Mojang's Direction.values(): down, up, north, south, west, east
inline constexpr int kDirX[6] = {0, 0, 0, 0, -1, 1};
inline constexpr int kDirY[6] = {-1, 1, 0, 0, 0, 0};
inline constexpr int kDirZ[6] = {0, 0, -1, 1, 0, 0};
enum Dir { DOWN = 0, UP = 1, NORTH = 2, SOUTH = 3, WEST = 4, EAST = 5 };
inline BlockPos operator+(const BlockPos& p, Dir d) { return {p.x + kDirX[d], p.y + kDirY[d], p.z + kDirZ[d]}; }

class Level {
public:
    Level(const BlockData& bd, int center_x, int center_z, ChunkBlocks* const chunks[9], GenRange range, std::function<int(int, int, int)> biome_at)
        : bd_(bd), cx_(center_x), cz_(center_z), range_(range), biome_at_(std::move(biome_at)) { for (int i = 0; i < 9; i++) chunks_[i] = chunks[i]; }
    const BlockData& bd() const { return bd_; }
    int center_x() const { return cx_; }
    int center_z() const { return cz_; }
    int min_y() const { return range_.min_y; }
    int max_y() const { return range_.min_y + range_.height - 1; }
    const GenRange& range() const { return range_; }
    bool outside_build_height(int y) const { return y < range_.min_y || y > max_y(); }

    ChunkBlocks* chunk_for(int x, int z) const {
        int dx = (x >> 4) - cx_ + 1, dz = (z >> 4) - cz_ + 1;
        if (dx < 0 || dx > 2 || dz < 0 || dz > 2) return nullptr;
        return chunks_[dz * 3 + dx];
    }
    uint16_t get(int x, int y, int z) const {
        ChunkBlocks* c = chunk_for(x, z);
        return c ? c->get(x, y, z) : 0;
    }
    uint16_t get(const BlockPos& p) const { return get(p.x, p.y, p.z); }
    bool can_write(int x, int z) const {   // WorldGenRegion.ensureCanWrite: within one chunk of the centre
        int dx = (x >> 4) - cx_, dz = (z >> 4) - cz_;
        return dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1;
    }
    bool can_write(const BlockPos& p) const { return can_write(p.x, p.z); }
    // WorldGenRegion.setBlock: false when outside the write zone
    bool set(int x, int y, int z, uint16_t id) {
        if (!can_write(x, z)) return false;
        ChunkBlocks* c = chunk_for(x, z);
        if (!c) return false;
        c->set(x, y, z, id);
        return true;
    }
    bool set(const BlockPos& p, uint16_t id) { return set(p.x, p.y, p.z, id); }
    // BlockEntity data of a block placed during generation (the chunk it is in keeps it)
    void set_block_entity(const BlockPos& p, pn::Nbt nbt) { if (ChunkBlocks* c = chunk_for(p.x, p.z)) c->block_entities[{p.x, p.y, p.z}] = std::move(nbt); }
    void remove_block_entity(const BlockPos& p) { if (ChunkBlocks* c = chunk_for(p.x, p.z)) c->block_entities.erase({p.x, p.y, p.z}); }
    // writes that skip the heightmaps (sections written to directly)
    void set_raw(int x, int y, int z, uint16_t id) { if (ChunkBlocks* c = chunk_for(x, z)) c->set_raw(x, y, z, id); }
    // Level.getHeight: first free y of the heightmap
    int height(HeightmapType t, int x, int z) const {
        ChunkBlocks* c = chunk_for(x, z);
        return c ? c->first_free(t, x, z) : range_.min_y;
    }
    bool is_air(const BlockPos& p) const { return bd_.air(get(p)); }
    // the biome the features see (BiomeManager.getBiome with the position fuzz): index into the generator's biome list
    int biome(int x, int y, int z) const { return biome_at_(x, y, z); }
    int biome(const BlockPos& p) const { return biome_at_(p.x, p.y, p.z); }
private:
    const BlockData& bd_;
    int cx_, cz_;
    ChunkBlocks* chunks_[9];
    GenRange range_;
    std::function<int(int, int, int)> biome_at_;
};

}  // namespace pn::vg
