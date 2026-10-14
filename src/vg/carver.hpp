// Mojang's world carvers: caves, canyons and the nether caves, cut into a chunk from the carver seeds of the 17x17 chunks around it.
#pragma once
#include "chunkdata.hpp"
#include "noisegen.hpp"
#include "surface.hpp"
#include "values.hpp"

namespace pn::vg {

struct CarveBlocks {
    uint16_t air = 0, cave_air = 0, lava = 0, water = 0, grass_block = 0, mycelium = 0, dirt = 0;
};

class Carvers {
public:
    // blocks_count: number of block states; name_of(id) gives "minecraft:stone" for an id
    Carvers(const Registry& reg, const WorldState& state, const SurfaceSystem* surface, const CarveBlocks& cb, size_t blocks_count, std::function<std::string(uint16_t)> name_of, GenRange range,
            std::vector<std::vector<std::string>> carvers_of_biome);
    ~Carvers();
    // source_biome(quartX, quartZ) -> biome value index for the chunk's carver list (sampled at y 0)
    void carve(ChunkBlocks& blocks, int chunk_x, int chunk_z, ChunkFill& fill, const std::function<int(int, int)>& source_biome, const std::function<int(int, int, int)>* lookup = nullptr) const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace pn::vg
