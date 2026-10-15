// What world generation needs to know about block states: Mojang's own per-state facts (data/worldgen/<version>/blockflags.txt), property edits and block tags.
#pragma once
#include "../blocks.hpp"
#include "registry.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace pn::vg {

enum : uint32_t {
    BF_AIR = 1u << 0, BF_BLOCKS_MOTION = 1u << 1, BF_REPLACEABLE = 1u << 2, BF_SOLID = 1u << 3, BF_CAN_OCCLUDE = 1u << 4, BF_FULL_COLLISION = 1u << 5, BF_WATER = 1u << 6, BF_LAVA = 1u << 7,
    BF_STURDY_SHIFT = 8,   // down, up, north, south, east, west
    BF_LIQUID = 1u << 14, BF_SOLID_RENDER = 1u << 15, BF_SKYLIGHT = 1u << 16, BF_IGNITED_BY_LAVA = 1u << 17, BF_SOURCE = 1u << 18,
    BF_EMISSION_SHIFT = 20, BF_COLLISION_FACE_SHIFT = 24, BF_DYNAMIC_SHAPE = 1u << 30,
};

class BlockData {
public:
    // flags_path: blockflags.txt of the version; reg resolves block tags
    BlockData(const std::string& flags_path, const std::string& classes_path, const Registry& reg);
    const Blocks& blocks() const { return B_; }
    uint32_t flags(uint16_t id) const { return flags_[id < flags_.size() ? id : 0]; }
    bool air(uint16_t id) const { return flags(id) & BF_AIR; }
    bool blocks_motion(uint16_t id) const { return flags(id) & BF_BLOCKS_MOTION; }
    bool replaceable(uint16_t id) const { return flags(id) & BF_REPLACEABLE; }
    bool water(uint16_t id) const { return flags(id) & BF_WATER; }      // fluid state is water (water block, waterlogged, kelp, ...)
    bool lava(uint16_t id) const { return flags(id) & BF_LAVA; }
    bool fluid(uint16_t id) const { return flags(id) & (BF_WATER | BF_LAVA); }
    bool sturdy(uint16_t id, int dir) const { return flags(id) & (1u << (BF_STURDY_SHIFT + dir)); }   // dir: 0 down 1 up 2 north 3 south 4 east 5 west
    bool collision_face_full(uint16_t id, int dir) const { return flags(id) & (1u << (BF_COLLISION_FACE_SHIFT + dir)); }
    int emission(uint16_t id) const { return int(flags(id) >> BF_EMISSION_SHIFT) & 15; }
    // BlockState.rotate / mirror: rot 0..3 = none, 90, 180, 270 clockwise; mirror 0..2 = none, left-right, front-back
    uint16_t rotate(uint16_t id, int rot) const { return rot == 0 || id >= B_.count() ? id : xform_[size_t(id) * 5 + size_t(rot - 1)]; }
    uint16_t mirror(uint16_t id, int m) const { return m == 0 || id >= B_.count() ? id : xform_[size_t(id) * 5 + size_t(2 + m)]; }
    const std::string& name(uint16_t id) const { return B_.info(id).name; }
    bool is(uint16_t id, const std::string& block) const { return B_.info(id).name == block; }
    // the per-id bytes ChunkBlocks wants (kFlagAir | kFlagFluid | kFlagBlocksMotion | kFlagLeaves)
    const uint8_t* chunk_flags() const { return chunk_flags_.data(); }
    // block (not state) membership of a tag: "minecraft:leaves" or "#minecraft:leaves"
    bool in_tag(uint16_t id, const std::string& tag) const;
    const std::vector<uint8_t>& tag_table(const std::string& tag) const;
    // property access: "axis" -> "y"
    std::string prop(uint16_t id, const std::string& key) const;
    bool has_prop(uint16_t id, const std::string& key) const;
    uint16_t with(uint16_t id, const std::string& key, const std::string& value) const;   // same block with one property changed (unchanged when absent)
    // class hierarchy of the block, most specific first ("SaplingBlock", "VegetationBlock"): picks survival rules
    const std::vector<std::string>& classes(uint16_t id) const;
    uint16_t find(const std::string& name, const std::string& props = "") const { return B_.find(name, props); }
    uint16_t default_state(const std::string& name) const { return B_.default_state(name); }
    uint16_t state(const Json& block_state) const;                                          // {"Name": ..., "Properties": {...}}
private:
    Blocks& B_;
    const Registry& reg_;
    std::vector<uint32_t> flags_;
    std::vector<uint16_t> xform_;
    std::vector<uint8_t> chunk_flags_;
    std::map<std::string, std::vector<std::string>> classes_;
    std::vector<const std::vector<std::string>*> class_of_;
    mutable std::mutex mu_;
    mutable std::map<std::string, std::unique_ptr<std::vector<uint8_t>>> tags_;
};

}  // namespace pn::vg
