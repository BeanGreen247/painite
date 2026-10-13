// Terrain generation for new worlds: seeded noise terrain with biomes, oceans, caves, ores and trees (normal, large_biomes, amplified),
// plus superflat. Each chunk depends only on the seed and its own coordinates, so chunks can be generated in any order and in parallel.
#pragma once
#include "world.hpp"
#include <cstdint>
#include <string>

namespace pn {

enum class GenKind { Void, Flat, Normal, LargeBiomes, Amplified };

class WorldGen {
public:
    WorldGen(GenKind kind, uint64_t seed, const std::vector<std::string>& biome_names);
    void generate(Chunk& c) const;
    int surface_height(int x, int z) const;     // terrain height (highest solid block) without carving
    bool find_spawn(int& x, int& z) const;      // nearest dry land to the origin
    GenKind kind() const { return kind_; }
    uint64_t seed() const { return seed_; }

private:
    double noise2(double x, double z, int ch) const;
    double noise3(double x, double y, double z, int ch) const;
    double fbm2(double x, double z, int oct, int ch) const;
    struct Column { int height; int biome; double temp, humid; };
    Column column(int x, int z) const;
    void flat(Chunk& c) const;
    void terrain(Chunk& c) const;
    GenKind kind_;
    uint64_t seed_;
    uint8_t perm_[4][512];
    uint8_t b_plains_, b_forest_, b_desert_, b_snowy_, b_ocean_, b_deep_ocean_, b_beach_, b_cold_ocean_;
    uint16_t air_, bedrock_, stone_, deepslate_, dirt_, grass_, sand_, sandstone_, gravel_, water_, lava_, snow_, snow_block_, log_, leaves_, short_grass_,
        dandelion_, poppy_, coal_, iron_, copper_, gold_, redstone_, diamond_, lapis_, deep_coal_, deep_iron_, deep_gold_, deep_diamond_, deep_redstone_, deep_lapis_, deep_copper_,
        cactus_, red_sand_, clay_;
    std::vector<std::string> names_;
};

// "123", "-5" or any text (Java's String.hashCode, like vanilla); empty = random
uint64_t seed_from_string(const std::string& s);

}  // namespace pn
