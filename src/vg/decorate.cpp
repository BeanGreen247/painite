#include "decorate.hpp"
#include <algorithm>
#include <stdexcept>

namespace pn::vg {

namespace {

struct FeatureData {
    int index, step;
    std::string feature;
};
struct FeatureLess {
    bool operator()(const FeatureData& a, const FeatureData& b) const { return a.step != b.step ? a.step < b.step : a.index < b.index; }
};
using EdgeSet = std::set<FeatureData, FeatureLess>;

// Graph.depthFirstSearch: true when a cycle is found
bool dfs(const std::map<FeatureData, EdgeSet, FeatureLess>& edges, EdgeSet& discovered, EdgeSet& visiting, std::vector<FeatureData>& out, const FeatureData& current) {
    if (discovered.count(current)) return false;
    if (visiting.count(current)) return true;
    visiting.insert(current);
    auto it = edges.find(current);
    if (it != edges.end())
        for (auto& next : it->second)
            if (dfs(edges, discovered, visiting, out, next)) return true;
    visiting.erase(current);
    discovered.insert(current);
    out.push_back(current);
    return false;
}

}  // namespace

Decorator::Decorator(const Registry& reg, int64_t seed, const std::vector<std::string>& biomes) : reg_(reg), seed_(seed) {
    // FeatureSorter.buildFeaturesPerStep
    std::map<std::string, int> feature_index;
    int next_index = 0;
    std::map<FeatureData, EdgeSet, FeatureLess> edges;
    size_t max_step = 0;
    std::vector<std::vector<std::vector<std::string>>> per_biome;   // [biome][step] -> feature ids
    for (auto& name : biomes) {
        std::vector<std::vector<std::string>> steps;
        const Json& bj = reg.get("biome", name);
        if (const Json* f = bj.find("features"))
            for (auto& step : f->arr) {
                std::vector<std::string> list;
                for (auto& e : step.arr) list.push_back(e.str);
                steps.push_back(std::move(list));
            }
        per_biome.push_back(steps);
        std::vector<FeatureData> flat;
        max_step = std::max(max_step, steps.size());
        for (size_t i = 0; i < steps.size(); i++)
            for (auto& id : steps[i]) {
                auto it = feature_index.find(id);
                if (it == feature_index.end()) it = feature_index.emplace(id, next_index++).first;
                flat.push_back(FeatureData{it->second, int(i), id});
            }
        for (size_t i = 0; i < flat.size(); i++) {
            EdgeSet& data = edges[flat[i]];
            if (i + 1 < flat.size()) data.insert(flat[i + 1]);
        }
    }
    EdgeSet discovered, visiting;
    std::vector<FeatureData> sorted;
    for (auto& kv : edges) {
        if (!visiting.empty()) throw std::runtime_error("feature sorter: iteration finished with a non-empty in-progress set");
        if (!discovered.count(kv.first) && dfs(edges, discovered, visiting, sorted, kv.first)) throw std::runtime_error("feature order cycle found");
    }
    std::reverse(sorted.begin(), sorted.end());
    step_features_.assign(max_step, {});
    for (auto& fd : sorted) step_features_[size_t(fd.step)].push_back(fd.feature);
    // index of each feature within its step
    std::vector<std::map<std::string, int>> position(max_step);
    for (size_t s = 0; s < max_step; s++)
        for (size_t i = 0; i < step_features_[s].size(); i++) position[s][step_features_[s][i]] = int(i);
    biome_step_indices_.resize(biomes.size());
    biome_feature_names_.resize(biomes.size());
    for (size_t b = 0; b < biomes.size(); b++) {
        biome_step_indices_[b].resize(max_step);
        for (size_t s = 0; s < per_biome[b].size(); s++)
            for (auto& id : per_biome[b][s]) {
                biome_step_indices_[b][s].push_back(position[s][id]);
                biome_feature_names_[b].insert(id);
            }
    }
}

bool Decorator::biome_has_feature(int biome, const std::string& placed_feature) const {
    return biome >= 0 && size_t(biome) < biome_feature_names_.size() && biome_feature_names_[size_t(biome)].count(placed_feature) != 0;
}

void Decorator::decorate(FeatureEnv& env, int chunk_x, int chunk_z, const std::set<int>& biomes_in_region) const {
    const int origin_x = chunk_x * 16, origin_z = chunk_z * 16;
    // WorldgenRandom(XoroshiroRandomSource).setDecorationSeed
    WorldgenRandomXoro random(seed_);
    int64_t x_scale = random.next_long() | 1;
    int64_t z_scale = random.next_long() | 1;
    int64_t decoration_seed = int64_t(uint64_t(int64_t(origin_x)) * uint64_t(x_scale) + uint64_t(int64_t(origin_z)) * uint64_t(z_scale)) ^ seed_;
    random.set_seed(decoration_seed);
    for (size_t step = 0; step < step_features_.size(); step++) {
        // (structures of this step would be placed here)
        std::set<int> indices;
        for (int b : biomes_in_region)
            if (size_t(b) < biome_step_indices_.size() && step < biome_step_indices_[size_t(b)].size())
                for (int i : biome_step_indices_[size_t(b)][step]) indices.insert(i);
        for (int idx : indices) {
            random.set_seed(decoration_seed + int64_t(idx) + int64_t(10000) * int64_t(step));
            const std::string& id = step_features_[step][size_t(idx)];
            const Json& placed = reg_.get("placed_feature", id);
            place_placed_feature(env, placed, random, BlockPos(origin_x, env.level.min_y(), origin_z), &id);
        }
    }
}

}  // namespace pn::vg
