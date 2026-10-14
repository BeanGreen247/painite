// Mojang's feature placement: placed features (placement modifiers), configured features, block predicates and block state providers, interpreted from the worldgen JSON.
#pragma once
#include "biomeinfo.hpp"
#include "level.hpp"
#include "noise.hpp"
#include "random.hpp"
#include "registry.hpp"
#include <map>
#include <memory>
#include <mutex>

namespace pn::vg {

constexpr uint16_t kNoState = 0xffff;   // a provider that supplies nothing

// shared between the features of one generator: lazily created noises of noise-based providers
struct FeatureShared {
    std::mutex mu;
    std::map<const Json*, std::unique_ptr<NormalNoise>> noises;
    std::map<const Json*, std::unique_ptr<NormalNoise>> slow_noises;
    const NormalNoise& noise_of(const Json* key, const Json& provider, bool slow = false);
    std::unique_ptr<NormalNoise> geode_noise;   // NormalNoise.create(WorldgenRandom(LegacyRandomSource(seed)), -4, 1.0)
};

struct FeatureEnv {
    Level& level;
    const Registry& reg;
    FeatureShared& shared;
    // BiomeFilter: does the biome at this position list the placed feature (by id)?
    std::function<bool(int biome, const std::string& placed_feature)> biome_has_feature;
    const std::vector<BiomeInfo>* biomes = nullptr;   // by biome value
    int sea_level = 63;
    int64_t seed = 0;    // the world seed
    const BlockData& bd() const { return level.bd(); }
    const std::string* top_feature = nullptr;   // the placed feature being placed (for the biome filter)
};

// "minecraft:x" -> the JSON of that registry entry, or the inline object itself
const Json& resolve_ref(FeatureEnv& env, const char* kind, const Json& ref);

bool place_placed_feature(FeatureEnv& env, const Json& placed, Rng& random, const BlockPos& origin, const std::string* top_feature_id);   // PlacedFeature.place(WithBiomeCheck)
bool place_configured_feature(FeatureEnv& env, const Json& configured, Rng& random, const BlockPos& origin);                             // ConfiguredFeature.place
// each feature type: returns whether it placed anything; the config is the "config" object
bool place_feature_type(FeatureEnv& env, const std::string& type, const Json& config, Rng& random, const BlockPos& origin);

bool test_predicate(FeatureEnv& env, const Json& predicate, const BlockPos& origin);                    // BlockPredicate.test
uint16_t provide_state(FeatureEnv& env, const Json& provider, Rng& random, const BlockPos& pos);        // BlockStateProvider.getOptionalState (kNoState = null)
bool rule_test(FeatureEnv& env, const Json& test, uint16_t state, Rng& random);                         // RuleTest.test
bool state_can_survive(FeatureEnv& env, uint16_t state, const BlockPos& pos);                           // BlockState.canSurvive (survive.cpp)
bool state_in_set(FeatureEnv& env, uint16_t state, const Json& blocks);                                 // "minecraft:x" | ["minecraft:x"] | "#minecraft:tag"
int heightmap_from_name(const std::string& name);
bool place_feature_type2(FeatureEnv& env, const std::string& type, const Json& config, Rng& random, const BlockPos& origin, bool& handled);   // features2.cpp
bool place_feature_type3(FeatureEnv& env, const std::string& type, const Json& config, Rng& random, const BlockPos& origin, bool& handled);   // features3.cpp
bool place_tree_feature(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin);   // feat_tree.cpp

}  // namespace pn::vg
