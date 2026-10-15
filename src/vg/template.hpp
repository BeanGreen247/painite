// Structure templates (the .nbt files of data/minecraft/structure) and Mojang's template system: placement with rotation and mirror, processors,
// rule tests, waterlogging. Mirrors StructureTemplate, StructurePlaceSettings and the templatesystem package.
#pragma once
#include "structure.hpp"
#include "../nbt.hpp"
#include <map>
#include <memory>
#include <mutex>

namespace pn::vg {

class StructureTemplate;
struct PlaceSettings;

struct TemplateBlock {
    BlockPos pos;
    uint16_t state = 0;
    std::shared_ptr<const pn::Nbt> nbt;   // block entity data
};

struct JigsawInfo {
    TemplateBlock info;
    bool rollable = false;                // joint type
    std::string name, pool, target;       // "minecraft:..." ids
    int placement_priority = 0, selection_priority = 0;
};

struct TemplateEntity {
    double x = 0, y = 0, z = 0;
    BlockPos block_pos;
    pn::Nbt nbt;
};

struct TemplatePalette {
    std::vector<TemplateBlock> blocks;    // full blocks, then the rest, then block entities (Mojang's order), each sorted by y, x, z
    std::vector<JigsawInfo> jigsaws;      // the jigsaw blocks of `blocks`
};

BlockPos transform_pos(const BlockPos& pos, Mirror mirror, Rotation rotation, const BlockPos& pivot);
BlockPos zero_position_with_transform(const BlockPos& zero, Mirror mirror, Rotation rotation, int size_x, int size_z);

struct RuleTest {
    enum Kind { ALWAYS_TRUE, BLOCK_MATCH, BLOCKSTATE_MATCH, RANDOM_BLOCK_MATCH, RANDOM_BLOCKSTATE_MATCH, TAG_MATCH } kind = ALWAYS_TRUE;
    uint16_t state = 0;                   // blockstate_match: exact state; block_match: any state of this block (by name)
    std::string block;                    // block name or tag id
    float probability = 1.0f;
    bool test(const BlockData& bd, uint16_t s, Rng& random) const;
};

struct PosRuleTest {
    enum Kind { ALWAYS_TRUE, LINEAR, AXIS_ALIGNED_LINEAR } kind = ALWAYS_TRUE;
    float min_chance = 0, max_chance = 0;
    int min_dist = 0, max_dist = 0;
    int axis = 0;                         // 0 x, 1 y, 2 z
    bool test(const BlockPos& world, const BlockPos& reference, Rng& random) const;
};

struct ProcessorRule {
    RuleTest input, location;
    PosRuleTest position;
    uint16_t output = 0;
    enum Modifier { PASSTHROUGH, CLEAR, APPEND_STATIC, APPEND_LOOT } modifier = PASSTHROUGH;
    pn::Nbt modifier_data;                // append_static: data, append_loot: LootTable name in "loot_table"
};

// StructureProcessor
class StructureProcessor {
public:
    virtual ~StructureProcessor() = default;
    // false = the block is dropped. `rel` is the block's position inside the template; `info.pos` is its world position.
    virtual bool process_block(const Level& level, const BlockData& bd, const BlockPos& position, const BlockPos& reference, const BlockPos& rel, TemplateBlock& info, const PlaceSettings& s) const { return true; }
    virtual bool evaluates_entire_piece_state() const { return false; }
    virtual void finalize_processing(const Level&, const BlockData&, const BlockPos&, const BlockPos&, const std::vector<TemplateBlock>&, std::vector<TemplateBlock>&, const PlaceSettings&) const {}
};
using ProcessorP = std::shared_ptr<const StructureProcessor>;

struct PlaceSettings {
    Mirror mirror = Mirror::NONE;
    Rotation rotation = Rotation::NONE;
    BlockPos pivot;
    bool ignore_entities = false;
    bool has_bb = false;
    BoundingBox bb;
    bool apply_waterlogging = true;       // LiquidSettings.APPLY_WATERLOGGING
    Rng* random = nullptr;                // null: positional LegacyRandom per block (Mth.getSeed(pos))
    bool known_shape = false;
    bool finalize_entities = false;
    int64_t level_seed = 0;
    std::vector<ProcessorP> processors;
    int palette_choice(size_t n, const BlockPos& pos) const;   // StructurePlaceSettings.getRandomPalette
};

class StructureTemplate {
public:
    int size_x = 0, size_y = 0, size_z = 0;
    std::vector<TemplatePalette> palettes;
    std::vector<TemplateEntity> entities;
    bool load(const std::string& path, const BlockData& bd);
    BoundingBox bounding_box(const BlockPos& position, Rotation rot, const BlockPos& pivot, Mirror mirror) const;
    void size(Rotation r, int& sx, int& sy, int& sz) const { bool sw = r == Rotation::CLOCKWISE_90 || r == Rotation::COUNTERCLOCKWISE_90; sx = sw ? size_z : size_x; sy = size_y; sz = sw ? size_x : size_z; }
    // getJigsaws: the jigsaw blocks with positions rotated and moved
    std::vector<JigsawInfo> jigsaws(const BlockData& bd, const BlockPos& position, Rotation rotation) const;
    // blocks of one block type (e.g. structure blocks), as filterBlocks
    std::vector<TemplateBlock> filter_blocks(const BlockData& bd, const BlockPos& position, const PlaceSettings& s, const std::string& block, bool absolute) const;
    // placeInWorld: false when nothing was placed; `tm` (optional) supplies the block entity facts
    bool place(Level& level, const BlockData& bd, const BlockPos& position, const BlockPos& reference, const PlaceSettings& s, Rng& random) const;
    bool place_with(Level& level, const BlockData& bd, class TemplateManager* tm, const BlockPos& position, const BlockPos& reference, const PlaceSettings& s, Rng& random) const;
};

// worldgen/template_pool: pool elements and pools (StructurePoolElement, StructureTemplatePool)
struct PoolElement {
    enum Type { SINGLE, LEGACY, LIST, FEATURE, EMPTY } type = EMPTY;
    std::string location;                 // template id (single, legacy)
    std::vector<ProcessorP> processors;
    bool rigid = false;                   // StructureTemplatePool.Projection.RIGID, else TERRAIN_MATCHING
    int liquid_override = -1;             // -1 none, 0 apply_waterlogging, 1 ignore_waterlogging
    std::vector<std::shared_ptr<PoolElement>> elements;   // list
    std::string feature;                  // placed feature id (feature)
    void set_projection(bool is_rigid) { rigid = is_rigid; for (auto& e : elements) e->set_projection(is_rigid); }
};
using PoolElementP = std::shared_ptr<PoolElement>;

struct Pool {
    std::vector<PoolElementP> templates;  // expanded by weight, in file order
    std::string fallback = "minecraft:empty";
    int max_size = INT32_MIN;             // cached StructureTemplatePool.getMaxSize
};

// the processor lists of worldgen/processor_list and the templates of data/minecraft/structure, cached
class TemplateManager {
public:
    TemplateManager(const Registry& reg, const BlockData& bd);
    const StructureTemplate* get(const std::string& id);                 // "minecraft:village/plains/houses/x"; null when missing
    const std::vector<ProcessorP>& processor_list(const std::string& id); // "minecraft:mossify_10_percent"
    Pool* pool(const std::string& id);                                   // "minecraft:village/plains/town_centers"; null when missing
    const PoolElementP& empty_element() const { return empty_; }
    std::vector<ProcessorP> processors_from(const Json& j);              // a list reference, an inline list or a single processor
    const BlockData& bd() const { return bd_; }
    const Registry& reg() const { return reg_; }
    bool is_randomizable_container(uint16_t state) const;                // block entities that draw a loot table seed
    bool has_block_entity(uint16_t state) const;
    std::string block_entity_type(uint16_t state) const;                 // "minecraft:chest"
private:
    ProcessorP make_processor(const Json& j);
    RuleTest make_rule_test(const Json& j);
    uint16_t state_of(const Json& j) const;
    const Registry& reg_;
    const BlockData& bd_;
    std::mutex mu_;
    std::map<std::string, std::unique_ptr<StructureTemplate>> templates_;
    std::map<std::string, std::vector<ProcessorP>> lists_;
    std::map<std::string, std::unique_ptr<Pool>> pools_;
    PoolElementP empty_ = std::make_shared<PoolElement>();
    PoolElementP make_element(const Json& j);
    std::vector<uint8_t> randomizable_;   // per state: 1 has a block entity, 3 and holds a loot table seed
    std::map<std::string, std::string> be_types_;
};

// the processors of code-made pieces
ProcessorP block_ignore_processor(bool air, bool structure_block);
ProcessorP jigsaw_replacement_processor();
ProcessorP gravity_processor(HeightmapType type, int offset);

}  // namespace pn::vg
