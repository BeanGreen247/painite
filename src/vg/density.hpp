// Mojang's density functions: the tree described by the data files, evaluated block by block, with the caches, cell interpolation and
// markers of NoiseChunk (see noisechunk.hpp for the loops that drive them).
#pragma once
#include "state.hpp"
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace pn::vg {

struct ChunkNoise;

struct Ctx {
    int x = 0, y = 0, z = 0;
    const ChunkNoise* nc = nullptr;   // set while NoiseChunk's cell loops evaluate, null for single point evaluation
};

class Node {
public:
    virtual ~Node() = default;
    virtual double compute(const Ctx& c) const = 0;
};
using NodeP = std::shared_ptr<Node>;

class Beardifier;   // structure terrain adaptation (beardifier.hpp)

// State of one chunk's noise sampling: the cell grid and the interpolators and cell caches the density functions registered.
struct InterpolatorNode;
struct CacheAllInCellNode;
struct ChunkNoise {
    int cell_width = 4, cell_height = 8, cell_count_xz = 4, cell_count_y = 0, cell_noise_min_y = 0;
    int first_cell_x = 0, first_cell_z = 0, first_noise_x = 0, first_noise_z = 0, noise_size_xz = 4;
    bool interpolating = false, filling_cell = false;
    int cell_start_x = 0, cell_start_y = 0, cell_start_z = 0, in_cell_x = 0, in_cell_y = 0, in_cell_z = 0;
    mutable uint64_t epoch = 1;   // bumped before every root evaluation; CacheOnce results hold within one
    std::vector<InterpolatorNode*> interpolators;
    std::vector<CacheAllInCellNode*> cell_caches;
    const Beardifier* beardifier = nullptr;
    Ctx here() const { return Ctx{cell_start_x + in_cell_x, cell_start_y + in_cell_y, cell_start_z + in_cell_z, this}; }
};

class Compiler {
public:
    // chunk == null compiles the plain version (cache markers removed), as used for the climate sampler
    Compiler(const WorldState& state, ChunkNoise* chunk) : state_(state), chunk_(chunk) {}
    NodeP compile(const Json& j);            // a number, a "minecraft:..." reference or an object with a "type"
    NodeP reference(const std::string& id);
    NodeP constant(double v);
private:
    NodeP object(const Json& j);
    const WorldState& state_;
    ChunkNoise* chunk_;
    std::map<std::string, NodeP> refs_;
};

// the children of a chunk's cell cache / interpolator nodes, driven by NoiseChunk
struct InterpolatorNode : Node {
    InterpolatorNode(ChunkNoise* c, NodeP filler);
    double compute(const Ctx& ctx) const override;
    void fill_slice(bool slice0, int cell_z_index) const;   // corner values for cells along z at the current cell x
    void select_cell_yz(int cy, int cz) const;
    void update_for_y(double f) const;
    void update_for_x(double f) const;
    void update_for_z(double f) const;
    void swap_slices() const { std::swap(slice0, slice1); }
    ChunkNoise* chunk;
    NodeP filler;
    mutable std::vector<std::vector<double>> slice0, slice1;
    mutable double n000 = 0, n001 = 0, n100 = 0, n101 = 0, n010 = 0, n011 = 0, n110 = 0, n111 = 0;
    mutable double vxz00 = 0, vxz10 = 0, vxz01 = 0, vxz11 = 0, vz0 = 0, vz1 = 0, value = 0;
};

struct CacheAllInCellNode : Node {
    CacheAllInCellNode(ChunkNoise* c, NodeP filler);
    double compute(const Ctx& ctx) const override;
    void fill_cell() const;   // evaluates the filler for every block of the selected cell
    ChunkNoise* chunk;
    NodeP filler;
    mutable std::vector<double> values;
};

inline double jmin(double a, double b) { if (a != a) return a; if (a == 0.0 && b == 0.0) return __builtin_signbit(a) ? a : b; return a <= b ? a : b; }
inline double jmax(double a, double b) { if (a != a) return a; if (a == 0.0 && b == 0.0) return __builtin_signbit(a) ? b : a; return a >= b ? a : b; }

}  // namespace pn::vg
