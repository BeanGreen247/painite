// Multiface blocks (glow lichen, sculk veins) and the sculk spreader: Mojang's MultifaceSpreader, MultifaceGrowthFeature, SculkSpreader and SculkPatchFeature.
#pragma once
#include "feature.hpp"

namespace pn::vg {

inline constexpr const char* kFaceNames[6] = {"down", "up", "north", "south", "west", "east"};
inline constexpr Dir kOpposite[6] = {UP, DOWN, SOUTH, NORTH, EAST, WEST};

enum SpreadType { SAME_POSITION, SAME_PLANE, WRAP_AROUND };
struct SpreadPos {
    BlockPos pos;
    int face;
};
// a MultifaceSpreader: the block it grows and the spread types it tries in order; a sculk vein spreader has the vein's own replacement rules
struct Spreader {
    const char* block;
    bool sculk;
    std::vector<SpreadType> types;
};
extern const Spreader kLichenSpreader, kVeinSpreader, kVeinSameSpaceSpreader;

bool mf_has_face(const BlockData& bd, uint16_t state, int dir);
bool mf_can_attach(FeatureEnv& env, int dir_to_neighbour, const BlockPos& neighbour);
// MultifaceBlock.getStateForPlacement; kNoState = null
uint16_t mf_state_for_placement(FeatureEnv& env, const std::string& block, uint16_t old, const BlockPos& pos, int dir);
// MultifaceSpreader.spreadFromFaceTowardRandomDirection: whether something was placed
bool mf_spread_toward_random(FeatureEnv& env, const Spreader& sp, uint16_t state, const BlockPos& pos, int starting_face, Rng& random);
// MultifaceSpreader.spreadAll: how many placements
int mf_spread_all(FeatureEnv& env, const Spreader& sp, uint16_t state, const BlockPos& pos);

bool place_multiface(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin);
bool place_sculk_patch(FeatureEnv& env, const Json& config, Rng& random, const BlockPos& origin);

}  // namespace pn::vg
