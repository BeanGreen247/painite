// Biome decoration: the order features run in (FeatureSorter) and the per-chunk decoration pass (ChunkGenerator.applyBiomeDecoration).
#pragma once
#include "feature.hpp"
#include <functional>
#include <set>

namespace pn::vg {

class Decorator {
public:
    // biomes: names of the generator's biomes in the order of its biome source (index = biome value)
    Decorator(const Registry& reg, int64_t seed, const std::vector<std::string>& biomes);
    // features of one chunk: the biomes present in the 3x3 chunks around it decide which features run
    // structure_hook(step, random, decoration_seed) runs before the features of each step (11 steps at least), as the structures of that step are placed
    using StructureHook = std::function<void(int step, WorldgenRandomXoro& random, int64_t decoration_seed)>;
    void decorate(FeatureEnv& env, int chunk_x, int chunk_z, const std::set<int>& biomes_in_region, const StructureHook& structure_hook = nullptr) const;
    bool biome_has_feature(int biome, const std::string& placed_feature) const;
    size_t steps() const { return step_features_.size(); }
    const std::vector<std::vector<std::string>>& step_features() const { return step_features_; }
private:
    const Registry& reg_;
    int64_t seed_;
    std::vector<std::vector<std::string>> step_features_;               // per step, the placed features in run order
    std::vector<std::vector<std::vector<int>>> biome_step_indices_;     // [biome][step] -> indices into step_features_[step]
    std::vector<std::set<std::string>> biome_feature_names_;
    std::vector<std::map<std::string, const Json*>> placed_cache_;      // unused placeholder to keep the layout simple
};

}  // namespace pn::vg
