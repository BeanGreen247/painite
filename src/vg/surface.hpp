// Mojang's SurfaceSystem and SurfaceRules: grass, sand, terracotta bands, bedrock floors and the other top layers, from the noise settings' surface_rule.
#pragma once
#include "chunkdata.hpp"
#include "noisegen.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pn::vg {

struct SurfaceBiome {
    std::string name;       // minecraft:plains
    float temperature = 0.8f;
    bool frozen = false;    // temperature_modifier: frozen
};

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
    void build(ChunkBlocks& blocks, int chunk_x, int chunk_z, ChunkFill& fill) const;
    // SurfaceSystem.topMaterial: what the surface rules would put at one block (used by carvers to re-grass dirt)
    bool top_material(const ChunkBlocks& blocks, int chunk_x, int chunk_z, ChunkFill& fill, int x, int y, int z, bool under_fluid, uint16_t& out) const;
    // the biome the surface rules see at a block (BiomeManager.getBiome: quart cells with the position fuzz)
    int biome_at(int x, int y, int z) const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}  // namespace pn::vg
