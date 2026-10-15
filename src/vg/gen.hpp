// Mojang's world generation as one chunk generator for the server: biomes, terrain, surface, carvers and features from the data files and the ported algorithms.
#pragma once
#include "../world.hpp"
#include <memory>
#include <string>
#include <vector>

namespace pn::vg {

struct GenOptions {
    std::string version = "26.3";       // data/worldgen/<version>
    int dim = 0;                        // 0 overworld, 1 nether, 2 end
    int64_t seed = 0;
    std::string level_type = "normal";  // normal, large_biomes, amplified
    std::string stage = "full";         // noise, surface, carvers, features, full: what to run (for comparing the port stage by stage)
    std::string overlay;                // a datapack folder (data/minecraft/...) that overrides the worldgen files, for reference worlds made with datapacks
    std::string climate_dump;           // development: parameter lists written by tools/oracle/DumpClimate.java
};

class VanillaGen {
public:
    // data_dir holds worldgen/<version>/...; biome_names is the server's biome list (internal index = position)
    VanillaGen(const std::string& data_dir, const GenOptions& opt, const std::vector<std::string>& biome_names);
    ~VanillaGen();
    void generate(Chunk& c) const;                 // thread-safe
    void decorate(int cx, int cz) const;           // decorates one chunk (its neighbours' states are built as needed); generate() does this for the 3x3 around its chunk
    const std::vector<std::vector<std::string>>& decorator_steps() const;   // the features of each decoration step in run order (debugging)
    // development: the structure starts created in a chunk range, one line each ("id chunkX chunkZ pieces bounds")
    std::vector<std::string> describe_structures(int cx0, int cz0, int cx1, int cz1) const;
    // development: the starts created at one chunk and the description of each piece
    std::vector<std::pair<std::string, std::vector<std::string>>> structure_starts_detail(int cx, int cz) const;
    int debug_base_height(int x, int z, int heightmap) const;   // NoiseBasedChunkGenerator.getBaseHeight (0 WORLD_SURFACE_WG, 2 OCEAN_FLOOR_WG)
    // development: decorate every chunk that one of the structure starts created at (cx, cz) touches, so its pieces are placed (they adjust their boxes while they are)
    void place_structures_of(int cx, int cz) const;
    int min_y() const;
    int height() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace pn::vg
