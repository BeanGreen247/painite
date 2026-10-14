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
    const std::vector<std::vector<std::string>>& decorator_steps() const;   // the features of each decoration step in run order (debugging)
    int min_y() const;
    int height() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace pn::vg
