// The 26.3 terrain of a dimension (NoiseBasedChunkGenerator, NoiseChunk, Aquifer, MaterialSystem and the material rules): density to blocks, then the surface layers.
#pragma once
#include "blockdata.hpp"
#include "chunkdata.hpp"
#include "df3.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pn::vg::v3 {

struct BiomeData3 {
    std::string name;            // minecraft:plains
    float temperature = 0.8f;
    bool frozen = false;         // temperature_modifier: frozen
};

// Biome.getHeightAdjustedTemperature with the 26.3 noises
float biome_temperature3(const BiomeData3& b, int x, int y, int z, int sea_level);

struct TerrainSettings {
    int min_y = -64, height = 384, sea_level = 63;   // the noise range after clamping to the dimension
    int dim_min_y = -64, dim_height = 384;
    bool has_aquifers = false;
};

struct NoiseChunk3;
struct NoiseChunk3Deleter { void operator()(NoiseChunk3* p) const; };
using NoiseChunkP = std::unique_ptr<NoiseChunk3, NoiseChunk3Deleter>;

class Terrain3 {
public:
    // settings_name: noise_settings entry ("overworld"); dim_min_y/dim_height clamp the noise range like clampToHeightAccessor
    Terrain3(const Registry& reg, const BlockData& bd, int64_t seed, const std::string& settings_name, std::vector<BiomeData3> biomes, int dim_min_y, int dim_height);
    ~Terrain3();
    const TerrainSettings& settings() const { return ns_; }
    RandomState3& random_state() { return *rs_; }
    DfCompiler& compiler() { return *comp_; }
    const std::vector<BiomeData3>& biomes() const { return biomes_; }
    // the six climate router functions (Climate.Sampler)
    struct Climate6 { const Sampler *temperature = nullptr, *vegetation = nullptr, *continents = nullptr, *erosion = nullptr, *depth = nullptr, *ridges = nullptr; };
    const Climate6& climate_samplers() const { return climate_; }
    uint16_t water_id() const { return water_; }

    // NoiseChunk: the volume, the caching samplers and the aquifer of one chunk (beardifier may be null)
    NoiseChunkP make_chunk(int chunk_x, int chunk_z, const Sampler* beardifier) const;

    // doFill: density, aquifer and the default block into `out` (heightmaps primed, fluid update marks set)
    void do_fill(NoiseChunk3& nc, ChunkBlocks& out) const;

    // MaterialSystem.buildSurface; `quart_biome(qx, qy, qz)` gives the climate value of a quart cell (any chunk)
    void build_surface(NoiseChunk3& nc, ChunkBlocks& blocks, const std::function<int(int, int, int)>& quart_biome) const;

    // MaterialSystem.topMaterial: what the surface rules put at one block (carvers re-grass dirt with it); returns false when no rule matches
    bool top_material(NoiseChunk3& nc, ChunkBlocks& blocks, const std::function<int(int, int, int)>& quart_biome, int x, int y, int z, bool under_fluid, uint16_t& out) const;

    // the aquifer of a noise chunk (carvers): -1 = no override, else the block id
    int aquifer_substance(NoiseChunk3& nc, int x, int y, int z, double density) const;
    bool aquifer_should_schedule(const NoiseChunk3& nc) const;

    // NoiseBasedChunkGenerator.iterateNoiseColumn: y + 1 of the first block `tester` accepts scanning down from the top (min_y when none); `states` (optional) gets the column from the bottom
    int iterate_column(int bx, int bz, const std::function<bool(uint16_t)>& tester, std::vector<uint16_t>* states = nullptr) const;

    // BiomeManager.getBiome over the quart cells (the fuzzy 3d choice)
    int biome_at(int x, int y, int z, const std::function<int(int, int, int)>& quart_biome) const;

    struct Impl;
private:
    const Registry& reg_;
    const BlockData& bd_;
    TerrainSettings ns_;
    std::vector<BiomeData3> biomes_;
    uint16_t water_ = 0;
    Climate6 climate_;
    std::unique_ptr<RandomState3> rs_;
    std::unique_ptr<DfCompiler> comp_;
    std::unique_ptr<Impl> p_;
    friend struct NoiseChunk3;
};

}  // namespace pn::vg::v3
