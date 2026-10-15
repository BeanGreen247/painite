// BlockState.updateShape for the blocks whose shape depends on their neighbours and that world generation leaves to be fixed at the end
// (StructurePiece marks fences, torches, ladders and iron bars): Block.updateFromNeighbourShapes.
#pragma once
#include "level.hpp"

namespace pn::vg {

// the state after every neighbour (west, east, north, south, down, up) has had its say
uint16_t update_from_neighbour_shapes(const BlockData& bd, const Level& level, const BlockPos& pos, uint16_t state);

}  // namespace pn::vg
