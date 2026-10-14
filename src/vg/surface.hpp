// Mojang's SurfaceSystem and SurfaceRules: grass, sand, terracotta bands, bedrock floors and the other top layers, from the noise settings' surface_rule.
#pragma once
#include "biomeinfo.hpp"
#include "chunkdata.hpp"
#include "noisegen.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pn::vg {

using SurfaceBiome = BiomeInfo;

// what the surface code needs to know about blocks, supplied by the caller
struct SurfaceBlocks {
    std::function<uint16_t(const Json&)> resolve;                 // {"Name": ..., "Properties": ...} -> id
    std::function<uint16_t(const std::string&)> by_name;          // default state of minecraft:x
    const uint8_t* flags = nullptr;                               // per id: bit 0 air, bit 1 fluid (water or lava)
    uint16_t water = 0;
};

struct SurfaceRuleTree;

class SurfaceSystem {
public:
    // noise_biome(quartX, quartY, quartZ) -> index into `biomes`
    SurfaceSystem(const WorldState& state, const Json& surface_rule, const SurfaceBlocks& blocks, uint16_t default_block, int sea_level, int min_y, int height, bool legacy_random_source,
                  std::vector<SurfaceBiome> biomes, std::function<int(int, int, int)> noise_biome);
    ~SurfaceSystem();
    // rewrites the top layers of the chunk in place; `fill` supplies preliminary surface levels
    // `lookup` (optional) supplies the quart-cell biomes of the chunk and its borders instead of sampling the climate noise again
    void build(ChunkBlocks& blocks, int chunk_x, int chunk_z, ChunkFill& fill, const std::function<int(int, int, int)>* lookup = nullptr) const;
    // SurfaceSystem.topMaterial: what the surface rules would put at one block (used by carvers to re-grass dirt)
    bool top_material(const ChunkBlocks& blocks, int chunk_x, int chunk_z, ChunkFill& fill, int x, int y, int z, bool under_fluid, uint16_t& out, const std::function<int(int, int, int)>* lookup = nullptr) const;
    // the biome the surface rules see at a block (BiomeManager.getBiome: quart cells with the position fuzz)
    int biome_at(int x, int y, int z) const;
    // the same with the quart-cell lookup supplied by the caller (chunks that already hold their biomes)
    int biome_with(int x, int y, int z, const std::function<int(int, int, int)>& lookup) const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}  // namespace pn::vg
