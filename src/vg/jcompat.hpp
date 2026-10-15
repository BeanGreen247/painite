// A thin layer over the generator's types that reads like Mojang's Java (Direction, BlockState, RandomSource, WorldGenLevel), so the structure pieces that
// were written as long runs of generateBox / placeBlock calls can be ported close to line by line (see tools/j2cpp.py).
#pragma once
#include "structure.hpp"
#include <cmath>
#include <string>

namespace pn::vg::jc {

const BlockData& bd();
void set_bd(const BlockData* b);

struct Direction {
    int v = pn::vg::NORTH;   // Dir: DOWN, UP, NORTH, SOUTH, WEST, EAST
    constexpr Direction() = default;
    constexpr explicit Direction(int d) : v(d) {}
    static const Direction DOWN, UP, NORTH, SOUTH, WEST, EAST;
    static const Direction HORIZONTAL[4];   // Direction.Plane.HORIZONTAL: north, east, south, west
    Direction getOpposite() const { return Direction(v ^ 1); }
    Direction getClockWise() const;
    Direction getCounterClockWise() const;
    int getStepX() const { return kDirX[v]; }
    int getStepY() const { return kDirY[v]; }
    int getStepZ() const { return kDirZ[v]; }
    int get2DDataValue() const;             // south 0, west 1, north 2, east 3
    int get3DDataValue() const { return v; }   // down 0, up 1, north 2, south 3, west 4, east 5
    static Direction from3DDataValue(int i) { return Direction(i); }
    static Direction from2DDataValue(int i);
    const char* getName() const;
    char axis() const { return v < 2 ? 'y' : v < 4 ? 'z' : 'x'; }
    bool operator==(const Direction& o) const { return v == o.v; }
    bool operator!=(const Direction& o) const { return v != o.v; }
    static Direction randomHorizontal(Rng& r) { return HORIZONTAL[r.next_int(4)]; }
};

struct BlockRef {
    std::string name;
    BlockRef() = default;
    BlockRef(const char* n) : name(n) {}
    BlockRef(std::string n) : name(std::move(n)) {}
    bool operator==(const BlockRef& o) const { return name == o.name; }
    uint16_t defaultBlockState() const { return bd().default_state(name); }
};

struct Tag { std::string id; Tag(const char* i) : id(i) {} };

// a BlockState: the generator's state id with Java's method names
struct BS {
    uint16_t id = 0;
    BS() = default;
    BS(uint16_t i) : id(i) {}
    operator uint16_t() const { return id; }
    BS setValue(const std::string& prop, const std::string& value) const { return BS(bd().with(id, prop, value)); }
    BS setValue(const std::string& prop, bool value) const { return setValue(prop, std::string(value ? "true" : "false")); }
    BS setValue(const std::string& prop, const char* value) const { return setValue(prop, std::string(value)); }
    BS setValue(const std::string& prop, const Direction& d) const { return setValue(prop, std::string(d.getName())); }
    BS setValue(const std::string& prop, int value) const { return setValue(prop, std::to_string(value)); }
    std::string getValue(const std::string& prop) const { return bd().prop(id, prop); }
    bool hasProperty(const std::string& prop) const { return bd().has_prop(id, prop); }
    BS rotate(Rotation r) const { return BS(bd().rotate(id, int(r))); }
    BS mirror(Mirror m) const { return BS(bd().mirror(id, int(m))); }
    bool is(const BlockRef& b) const { return bd().name(id) == b.name; }
    bool is(const char* b) const { return bd().name(id) == b; }
    bool is(const Tag& t) const { return bd().in_tag(id, t.id); }
    BlockRef getBlock() const { return BlockRef(bd().name(id)); }
    bool isAir() const { return bd().air(id); }
    bool liquid() const { return bd().flags(id) & BF_LIQUID; }
    bool isSolidRender() const { return bd().flags(id) & BF_SOLID_RENDER; }
    bool isFaceSturdy(const Direction& d) const { return bd().sturdy(id, d.v); }
    template <class L> bool isFaceSturdy(const L&, const BlockPos&, const Direction& d) const { return bd().sturdy(id, d.v); }
    bool operator==(const BS& o) const { return id == o.id; }
    bool operator!=(const BS& o) const { return id != o.id; }
};

inline BS blk(const std::string& name) { return BS(bd().default_state(name)); }
inline BS blk(const char* name) { return BS(bd().default_state(name)); }

// RandomSource with Java's names (borrows a generator, or owns one made by a positional factory)
struct JRandom {
    Rng* rp;
    std::shared_ptr<Rng> own;
    explicit JRandom(Rng& rng) : rp(&rng) {}
    explicit JRandom(std::unique_ptr<Rng> o) : rp(o.get()), own(std::move(o)) {}
    Rng& r() const { return *rp; }
    int nextInt() { return rp->next_int(); }
    int nextInt(int bound) { return rp->next_int(bound); }
    int nextInt(int origin, int bound) { return rp->next_int(origin, bound); }
    int nextIntBetweenInclusive(int a, int b) { return rp->next_int_between_inclusive(a, b); }
    bool nextBoolean() { return rp->next_boolean(); }
    float nextFloat() { return rp->next_float(); }
    double nextDouble() { return rp->next_double(); }
    int64_t nextLong() { return rp->next_long(); }
    // RandomSource.createThreadLocalInstance(seed): the plain 48 bit generator
    static JRandom createThreadLocalInstance(int64_t seed) { return JRandom(std::make_unique<LegacyRandom>(seed)); }
};

// RandomSource.createThreadLocalInstance(seed).forkPositional().at(pos)
inline JRandom positionalRandom(int64_t seed, const BlockPos& pos) {
    LegacyRandom base(seed);
    std::unique_ptr<PosFactory> f = base.fork_positional();
    return JRandom(f->at(pos.x, pos.y, pos.z));
}

// the part of WorldGenLevel the pieces use
struct JLevel {
    Level& L;
    PieceCtx& ctx;
    JLevel(Level& l, PieceCtx& c) : L(l), ctx(c) {}
    BS getBlockState(const BlockPos& p) const { return BS(L.get(p)); }
    bool setBlock(const BlockPos& p, const BS& s, int /*flags*/) { return L.set(p, s.id); }
    int getHeight(HeightmapType t, int x, int z) const { return L.height(t, x, z); }
    BlockPos getHeightmapPos(HeightmapType t, const BlockPos& p) const { return BlockPos(p.x, L.height(t, p.x, p.z), p.z); }
    int getMinY() const { return L.min_y(); }
    int getMaxY() const { return L.max_y(); }
    int getSeaLevel() const { return ctx.senv.sea_level; }
    int64_t getSeed() const { return ctx.senv.seed; }
    bool isWater(const BlockPos& p) const { return bd().water(L.get(p)); }
    int biomeAt(const BlockPos& p) const { return L.biome(p); }
    // level.getBiome(pos).is(tag)
    bool biomeIs(const BlockPos& p, const Tag& tag) const;
};

}  // namespace pn::vg::jc
