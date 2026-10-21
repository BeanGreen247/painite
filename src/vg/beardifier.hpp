// Terrain adaptation around structures (Mojang's Beardifier): a density added to the terrain near the pieces of structures that bend the ground.
#pragma once
#include "structure.hpp"

namespace pn::vg {

class Beardifier {
public:
    struct Rigid { BoundingBox box; int adjustment; int ground_level_delta; };
    // Beardifier.forStructuresInChunk: `starts` are the starts that touch the chunk (StructureState::starts_touching)
    Beardifier(const std::vector<std::shared_ptr<const StructureStart>>& starts, int chunk_x, int chunk_z);
    bool empty() const { return !has_box_; }
    double compute(int x, int y, int z) const;
    float compute_f(int x, int y, int z) const;   // the 26.3 version: single precision throughout
private:
    std::vector<Rigid> pieces_;
    std::vector<PieceJunction> junctions_;
    BoundingBox affected_;
    bool has_box_ = false;
};

}  // namespace pn::vg
