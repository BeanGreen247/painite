// Terrain generation for new worlds: seeded noise terrain with biomes, oceans, caves, ores and trees (normal, large_biomes, amplified),
// plus superflat. Each chunk depends only on the seed and its own coordinates, so chunks can be generated in any order and in parallel.
#pragma once
#include "world.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace pn {

enum class GenKind { Void, Flat, Normal, LargeBiomes, Amplified };

class WorldGen {
public:
    WorldGen(GenKind kind, uint64_t seed, const std::vector<std::string>& biome_names);
    void generate(Chunk& c) const;
    int surface_height(int x, int z) const;     // terrain height (highest solid block) without carving
    int sample(int x, int z, int& biome) const;   // terrain height and the generator's biome number (debugging, tests/gen_stats.cpp)
    bool find_spawn(int& x, int& z) const;      // nearest dry land to the origin
    GenKind kind() const { return kind_; }
    uint64_t seed() const { return seed_; }

private:
    double noise2(double x, double z, int ch) const;
    double noise3(double x, double y, double z, int ch) const;
    double fbm2(double x, double z, int oct, int ch) const;
    struct Column { int height; int bm; double cont, eros, weird, pv, temp, humid, mount, river; };
    Column column(int x, int z) const;
    int pick_biome(const Column& c, int h) const;
    void flat(Chunk& c) const;
    void terrain(Chunk& c) const;
    uint16_t surface_block(int bm, int depth, int y, int wx, int wz, double patch) const;
    struct Placed { int8_t dx, dz; int16_t dy; uint16_t b; };
    void build_tree(int kind, uint64_t& rs, std::vector<Placed>& out) const;
    void put_trees(Chunk& c, const Column (*cols)[18]) const;
    GenKind kind_;
    uint64_t seed_;
    uint8_t perm_[6][512];
    uint8_t bi_[64];
    uint16_t air_, bedrock_, stone_, deepslate_, dirt_, grass_, sand_, sandstone_, gravel_, water_, lava_, snow_, snow_block_, short_grass_, tall_grass_lo_, tall_grass_hi_,
        fern_, large_fern_lo_, large_fern_hi_, dandelion_, poppy_, azure_, oxeye_, cornflower_, allium_, orchid_, lily_valley_, lilac_lo_, lilac_hi_, rose_lo_, rose_hi_, peony_lo_, peony_hi_,
        sunflower_lo_, sunflower_hi_, coal_, iron_, copper_, gold_, redstone_, diamond_, lapis_, deep_coal_, deep_iron_, deep_gold_, deep_diamond_, deep_redstone_, deep_lapis_, deep_copper_,
        cactus_, red_sand_, clay_, podzol_, mycelium_, coarse_dirt_, mud_, ice_, packed_ice_, sugar_cane_, lily_pad_, kelp_, kelp_plant_, seagrass_, tall_seagrass_lo_, tall_seagrass_hi_,
        dead_bush_, pumpkin_, melon_, granite_, diorite_, andesite_, tuff_, calcite_, terracotta_[8], brown_mushroom_, red_mushroom_, moss_block_, bamboo_, blue_ice_;
    struct Wood { uint16_t log, leaf; };
    Wood oak_, birch_, spruce_, jungle_, acacia_, dark_oak_, cherry_, mangrove_;
    uint8_t bands_[64];
    std::vector<uint8_t> soft_;   // blocks a tree may overwrite: air, plants, snow layers, leaves
    std::vector<std::string> names_;
};

// "123", "-5" or any text (Java's String.hashCode, like vanilla); empty = random
uint64_t seed_from_string(const std::string& s);

}  // namespace pn
