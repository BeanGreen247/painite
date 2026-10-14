// Structures (villages, strongholds, nether fortresses, ...). Each structure is fixed by the seed and its start chunk, built once as a list of
// boxes and painted into every chunk it touches, so chunks stay independent of each other and can be generated in parallel.
#pragma once
#include "world.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn {

class WorldGen;

enum PrimMode : uint8_t {
    P_FILL,        // set every block of the box
    P_SOFT,        // only where the block is air, a plant, leaves or fluid
    P_FOUND,       // column fill from y1 down to y0, stopping at the first solid block (foundations, pillars)
    P_SURFACE      // the top solid block of the column (x0, z0) becomes `block` (paths)
};

struct StructPrim {
    int x0, y0, z0, x1, y1, z1;
    uint16_t block;
    PrimMode mode;
};

struct Structure {
    int minx = 0, minz = 0, maxx = 0, maxz = 0;
    std::vector<StructPrim> prims;
    std::string kind;
    void add(int x0, int y0, int z0, int x1, int y1, int z1, uint16_t b, PrimMode m = P_FILL);
};

class StructureGen {
public:
    StructureGen(const WorldGen& g, int dim);
    void paint(Chunk& c) const;                                                   // thread-safe
    bool locate(const std::string& kind, int x, int z, int& ox, int& oz) const;  // nearest start of a structure, block coordinates
    std::vector<std::string> kinds() const;

    struct Kind;

private:
    std::shared_ptr<const Structure> get(const Kind& k, int cx, int cz) const;
    bool start_of(const Kind& k, int rx, int rz, int& cx, int& cz) const;
    std::shared_ptr<const Structure> build(const Kind& k, int cx, int cz) const;
    const WorldGen& g_;
    int dim_;
    std::vector<std::pair<int, int>> strongholds_;   // start chunks, first rings of vanilla's layout
    mutable std::mutex mu_;
    mutable std::unordered_map<uint64_t, std::shared_ptr<const Structure>> cache_;
};

}  // namespace pn
