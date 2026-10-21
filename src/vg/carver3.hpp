// The 26.3 world carvers (CaveWorldCarver, CanyonWorldCarver, CarvingMask and NoiseBasedChunkGenerator.applyCarvingMask): the carvers fill a mask, the mask is then applied through the aquifer.
#pragma once
#include "terrain3.hpp"
#include "values.hpp"

namespace pn::vg::v3 {

class Carvers3 {
public:
    // carvers_of_biome: carver ids ("minecraft:cave") of each climate value; range: the generated height range
    Carvers3(const Registry& reg, const BlockData& bd, int64_t seed, GenRange range, std::vector<std::vector<std::string>> carvers_of_biome);
    ~Carvers3();
    // source_biome(quartX, quartZ): the carver biome of a chunk (point sample at y 0); lookup: quart cell biomes for the surface rules
    void carve(const Terrain3& t, NoiseChunk3& nc, ChunkBlocks& blocks, int chunk_x, int chunk_z, const std::function<int(int, int)>& source_biome, const std::function<int(int, int, int)>& lookup) const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace pn::vg::v3
