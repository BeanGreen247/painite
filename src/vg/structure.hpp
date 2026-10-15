// Mojang's structure system: bounding boxes, structure sets and their placement (random spread, concentric rings), structure starts made of pieces,
// and the per-dimension state that creates starts on demand. The structure types themselves live in structures_*.cpp.
#pragma once
#include "feature.hpp"
#include "level.hpp"
#include "random.hpp"
#include "registry.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pn::vg {

class TemplateManager;

struct BoundingBox {
    int x0 = 0, y0 = 0, z0 = 0, x1 = -1, y1 = -1, z1 = -1;   // inclusive corners
    BoundingBox() = default;
    BoundingBox(int ax, int ay, int az, int bx, int by, int bz) : x0(std::min(ax, bx)), y0(std::min(ay, by)), z0(std::min(az, bz)), x1(std::max(ax, bx)), y1(std::max(ay, by)), z1(std::max(az, bz)) {}
    static BoundingBox infinite() { return BoundingBox(INT32_MAX, INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, INT32_MIN, true); }
    bool intersects(const BoundingBox& o) const { return x1 >= o.x0 && x0 <= o.x1 && z1 >= o.z0 && z0 <= o.z1 && y1 >= o.y0 && y0 <= o.y1; }
    bool intersects(int ax, int az, int bx, int bz) const { return x1 >= ax && x0 <= bx && z1 >= az && z0 <= bz; }
    bool inside(int x, int y, int z) const { return x >= x0 && x <= x1 && z >= z0 && z <= z1 && y >= y0 && y <= y1; }
    bool inside(const BlockPos& p) const { return inside(p.x, p.y, p.z); }
    int x_span() const { return x1 - x0 + 1; }
    int y_span() const { return y1 - y0 + 1; }
    int z_span() const { return z1 - z0 + 1; }
    BlockPos center() const { return BlockPos(x0 + (x1 - x0 + 1) / 2, y0 + (y1 - y0 + 1) / 2, z0 + (z1 - z0 + 1) / 2); }
    BoundingBox& encapsulate(const BoundingBox& o) {
        x0 = std::min(x0, o.x0); y0 = std::min(y0, o.y0); z0 = std::min(z0, o.z0);
        x1 = std::max(x1, o.x1); y1 = std::max(y1, o.y1); z1 = std::max(z1, o.z1);
        return *this;
    }
    BoundingBox& encapsulate(const BlockPos& p) {
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); z0 = std::min(z0, p.z);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); z1 = std::max(z1, p.z);
        return *this;
    }
    BoundingBox& move(int dx, int dy, int dz) { x0 += dx; y0 += dy; z0 += dz; x1 += dx; y1 += dy; z1 += dz; return *this; }
    BoundingBox moved(int dx, int dy, int dz) const { BoundingBox b = *this; return b.move(dx, dy, dz); }
    BoundingBox inflated_by(int n) const { BoundingBox b = *this; b.x0 -= n; b.y0 -= n; b.z0 -= n; b.x1 += n; b.y1 += n; b.z1 += n; return b; }
    bool operator==(const BoundingBox& o) const { return x0 == o.x0 && y0 == o.y0 && z0 == o.z0 && x1 == o.x1 && y1 == o.y1 && z1 == o.z1; }
private:
    BoundingBox(int ax, int ay, int az, int bx, int by, int bz, bool) : x0(ax), y0(ay), z0(az), x1(bx), y1(by), z1(bz) {}
};

// Mojang's Rotation / Mirror
enum class Rotation { NONE = 0, CLOCKWISE_90 = 1, CLOCKWISE_180 = 2, COUNTERCLOCKWISE_90 = 3 };
enum class Mirror { NONE = 0, LEFT_RIGHT = 1, FRONT_BACK = 2 };

struct ChunkPos {
    int x = 0, z = 0;
    bool operator==(const ChunkPos& o) const { return x == o.x && z == o.z; }
    bool operator<(const ChunkPos& o) const { return x != o.x ? x < o.x : z < o.z; }
};

// Mojang's WorldgenRandom over the legacy LCG, with the seeding helpers structures use.
class WorldRandom : public LegacyRandom {
public:
    WorldRandom() : LegacyRandom(0) {}
    void set_large_feature_seed(int64_t seed, int chunk_x, int chunk_z) {
        set_seed(seed);
        int64_t xs = next_long(), zs = next_long();
        set_seed(int64_t(uint64_t(int64_t(chunk_x)) * uint64_t(xs)) ^ int64_t(uint64_t(int64_t(chunk_z)) * uint64_t(zs)) ^ seed);
    }
    void set_large_feature_with_salt(int64_t seed, int x, int z, int salt) {
        set_seed(int64_t(uint64_t(int64_t(x)) * 341873128712ULL + uint64_t(int64_t(z)) * 132897987541ULL + uint64_t(seed) + uint64_t(int64_t(salt))));
    }
};

// What a structure sees while it picks its start: the generator's biomes and terrain heights without any chunk being built.
struct StructureEnv {
    int64_t seed = 0;
    int dim = 0;                              // 0 overworld, 1 nether, 2 end
    int min_y = -64, height = 384, sea_level = 63;
    std::function<int(int, int, int)> biome_at;                         // quart coordinates -> biome name index
    const std::vector<std::string>* biome_names = nullptr;              // "minecraft:plains", ...
    // NoiseBasedChunkGenerator.getBaseHeight: first free y of the column for a heightmap (WORLD_SURFACE_WG or OCEAN_FLOOR_WG)
    std::function<int(int, int, HeightmapType)> base_height;
    // NoiseBasedChunkGenerator.getBaseColumn: blocks of the column, bottom to top, as (state is solid?) flags per y from min_y
    std::function<void(int, int, std::vector<uint16_t>&)> base_column;
    const Registry* reg = nullptr;
    const BlockData* bd = nullptr;
    TemplateManager* templates = nullptr;   // owned by the StructureState
};

// What a piece sees while it is placed into a chunk
struct PieceCtx {
    FeatureEnv& fenv;                         // the 3x3 chunk region the decoration pass works on
    Level& level;
    const StructureEnv& senv;
    TemplateManager& templates;
    Rng& random;                              // the decoration random (seeded per structure)
    BoundingBox chunk_bb;                     // the chunk being decorated
    ChunkPos chunk;
};

struct PieceJunction { int source_x, source_ground_y, source_z, delta_y; bool rigid; };

// One piece of a structure (StructurePiece): a bounding box, a type id for the saved form, and what it places into a chunk.
class StructurePiece {
public:
    virtual ~StructurePiece() = default;
    std::string type;                         // "minecraft:..." piece id as saved in chunks
    BoundingBox bb;
    int gen_depth = 0;
    int orientation = -1;                     // Direction ordinal or -1
    Rotation rotation = Rotation::NONE;
    Mirror mirror = Mirror::NONE;
    // writes the part inside `ctx.chunk_bb` (StructurePiece.postProcess)
    virtual void post_process(PieceCtx& ctx, const BlockPos& pivot) = 0;
    virtual void move(int dx, int dy, int dz) { bb.move(dx, dy, dz); }
    // one line describing the piece the way its saved form reads (development: compared with Mojang's chunk NBT)
    virtual std::string describe() const;
    // the terrain adaptation of a piece (Beardifier input): which boxes, and how the terrain bends around them
    int terrain_adaptation = 0;               // 0 none, 1 beard_thin, 2 beard_box, 3 bury, 4 encapsulate
    int ground_level_delta = 0;
    // jigsaw pieces (PoolElementStructurePiece): rigid projection and the junctions the Beardifier reads
    bool is_pool = false, rigid = false;
    std::vector<PieceJunction> junctions;
};

struct StructureStart {
    std::string structure;                    // "minecraft:village_plains"
    ChunkPos pos;
    int references = 0;
    int step = 0;                             // GenerationStep.Decoration ordinal of the structure
    int terrain_adaptation = 0;
    std::vector<std::shared_ptr<StructurePiece>> pieces;
    bool valid() const { return !pieces.empty(); }
    BoundingBox piece_bounds() const {
        BoundingBox b = BoundingBox::infinite();
        for (auto& p : pieces) b.encapsulate(p->bb);
        return b;
    }
    // StructureStart.getBoundingBox: the pieces' box, inflated by 12 when the structure adapts the terrain
    BoundingBox bounds() const { BoundingBox b = piece_bounds(); return terrain_adaptation ? b.inflated_by(12) : b; }
};

// Structure.GenerationContext
struct StructureContext {
    const StructureEnv& env;
    WorldRandom& random;                      // seeded with setLargeFeatureSeed(seed, chunk)
    ChunkPos chunk;
    std::function<bool(int)> valid_biome;     // biome name index -> allowed by the structure's biomes
    int first_free_height(int x, int z, HeightmapType hm) const { return env.base_height(x, z, hm); }
    int first_occupied_height(int x, int z, HeightmapType hm) const { return env.base_height(x, z, hm) - 1; }
    int biome_name_at(int qx, int qy, int qz) const { return env.biome_at(qx, qy, qz); }
    int middle_x() const { return chunk.x * 16 + 8; }
    int middle_z() const { return chunk.z * 16 + 8; }
    bool could_structure_exist_in_column(int x, int z, int min_y, int max_y) const;
    bool could_valid_biome_exist_on_top_of_chunk_center() const { return could_structure_exist_in_column(middle_x(), middle_z(), env.min_y - 1, env.min_y + env.height - 1); }
};

struct GenerationStub {
    BlockPos position;
    std::function<void(std::vector<std::shared_ptr<StructurePiece>>&)> generate;
};

class Structure {
public:
    virtual ~Structure() = default;
    std::string id;                           // "minecraft:village_plains"
    std::set<int> biomes;                     // allowed biome name indices
    int step = 3;                             // GenerationStep.Decoration: SURFACE_STRUCTURES by default
    int terrain_adaptation = 0;
    virtual std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const = 0;
    // Structure.afterPlace: runs once per chunk after the pieces of a start were placed in it
    virtual void after_place(PieceCtx& ctx, const StructureStart& start) const {}
};

// HeightProvider (constant or uniform over vertical anchors): sample(random, min_y, height)
std::function<int(Rng&, int, int)> make_height_provider(const Json& j);

// Structure.onTopOfChunkCenter: a start at the middle of the chunk on the terrain (needs a valid biome somewhere in that column)
std::optional<GenerationStub> on_top_of_chunk_center(const StructureContext& ctx, HeightmapType heightmap, std::function<void(std::vector<std::shared_ptr<StructurePiece>>&)> generator);
std::optional<GenerationStub> on_top_of_chunk_center_without_biome_check(const StructureContext& ctx, HeightmapType heightmap, std::function<void(std::vector<std::shared_ptr<StructurePiece>>&)> generator);
// Structure.getLowestY / getMeanFirstOccupiedHeight over a size_x by size_z area from the chunk's minimum corner
int structure_lowest_y(const StructureContext& ctx, int size_x, int size_z);
int structure_lowest_y(const StructureContext& ctx, int min_x, int min_z, int size_x, int size_z);
int structure_mean_first_occupied_height(const StructureContext& ctx, int min_x, int size_x, int min_z, int size_z);


struct StructurePlacement {
    enum Type { RANDOM_SPREAD, CONCENTRIC_RINGS } type = RANDOM_SPREAD;
    int salt = 0;
    float frequency = 1.0f;
    int frequency_method = 0;                 // 0 default, 1..3 legacy_type_N
    int locate_x = 0, locate_y = 0, locate_z = 0;
    std::string exclusion_set;                // other structure set id (empty = none)
    int exclusion_chunks = 0;
    // random_spread
    int spacing = 0, separation = 0;
    bool triangular = false;
    // concentric_rings
    int distance = 0, spread = 0, count = 0;
    std::set<int> preferred_biomes;
    std::vector<ChunkPos> rings;              // filled in by the generator state
};

struct StructureSelection { std::string structure; int weight = 1; };

struct StructureSetData {
    std::string id;                           // "minecraft:villages"
    StructurePlacement placement;
    std::vector<StructureSelection> structures;
};


// Per dimension: every structure set, its structures, and the starts created so far. Starts are a pure function of (seed, chunk), so they are made on demand
// from any thread and remembered.
class StructureState {
public:
    StructureState(const StructureEnv& env, const std::vector<std::string>& climate_biomes);
    ~StructureState();
    // the valid starts created at chunk (cx, cz) (ChunkGenerator.createStructures)
    std::vector<std::shared_ptr<const StructureStart>> starts_at(int cx, int cz);
    // the starts whose bounds touch chunk (cx, cz), in structure registry order within each step (createReferences + StructureManager.startsForStructure)
    std::vector<std::shared_ptr<const StructureStart>> starts_touching(int cx, int cz);
    const StructureEnv& env() const { return env_; }
    TemplateManager& templates() { return *templates_; }
    const std::vector<StructureSetData>& sets() const { return sets_; }
    // StructureStart.placeInChunk for every structure of a decoration step, in registry order (the index of a structure counts every structure of the step)
    void place_step(int step, const std::vector<std::shared_ptr<const StructureStart>>& touching, FeatureEnv& fenv, Rng& random, int64_t decoration_seed, int cx, int cz);
    bool has_structures() const { return !sets_.empty(); }
private:
    bool is_structure_chunk(const StructureSetData& set, int cx, int cz);
    std::vector<ChunkPos> ring_positions(const StructurePlacement& p);
    bool try_generate(const std::string& structure, int cx, int cz, std::vector<std::shared_ptr<const StructureStart>>& out);
    StructureEnv env_;
    std::unique_ptr<TemplateManager> templates_;
    std::vector<StructureSetData> sets_;
    std::map<std::string, std::unique_ptr<Structure>> structures_;
    std::vector<std::vector<std::string>> by_step_;       // structures of this dimension per step
    std::vector<std::vector<std::string>> all_by_step_;   // every structure of the registry per step, alphabetical
    std::mutex mu_;
    std::map<ChunkPos, std::vector<std::shared_ptr<const StructureStart>>> cache_;
};

// the structure types (structures_*.cpp) register themselves here: JSON "type" -> constructor
using StructureFactory = std::function<std::unique_ptr<Structure>(const Json& j, const Registry& reg)>;
std::map<std::string, StructureFactory>& structure_factories();

}  // namespace pn::vg
