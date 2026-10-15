// TemplateStructurePiece: a piece made from a structure template placed with StructurePlaceSettings, plus small helpers the code-built pieces share.
#pragma once
#include "legacy_piece.hpp"
#include "template.hpp"

namespace pn::vg::jc {

// the pieces' entities: id, position and the flags structures set on them
inline void add_entity(JLevel& level, const char* id, double x, double y, double z, bool persistent = true) {
    pn::Nbt e = pn::Nbt::compound();
    e.put("id", pn::Nbt::str(id));
    pn::Nbt pos = pn::Nbt::list_of(pn::Nbt::Double);
    for (double v : {x, y, z}) { pn::Nbt d; d.type = pn::Nbt::Double; d.f = v; pos.list.push_back(d); }
    e.put("Pos", pos);
    if (persistent) e.put("PersistenceRequired", pn::Nbt::make(pn::Nbt::Byte, 1));
    level.L.add_entity(x, z, std::move(e));
}


// TemplateStructurePiece
class TemplatePiece : public LegacyPiece {
public:
    std::string templateName;
    const StructureTemplate* tmpl = nullptr;
    PlaceSettings settings;
    BlockPos templatePosition;

    TemplatePiece(const char* id, int genDepth, TemplateManager& tm, const std::string& location, const PlaceSettings& ps, const BlockPos& position) {
        type = id;
        gen_depth = genDepth;
        tmpl = tm.get(location);
        templateName = location;
        settings = ps;
        templatePosition = position;
        if (tmpl) bb = tmpl->bounding_box(position, ps.rotation, ps.pivot, ps.mirror);
        setOrientation(Direction::NORTH);
        rotation = ps.rotation;
    }

    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos& referencePos) override {
        if (!tmpl) return;
        settings.has_bb = true;
        settings.bb = chunkBB;
        settings.level_seed = level.getSeed();
        bb = tmpl->bounding_box(templatePosition, settings.rotation, settings.pivot, settings.mirror);
        TemplateManager& tm = *level.ctx.senv.templates;
        if (tmpl->place_with(level.L, jc::bd(), &tm, templatePosition, referencePos, settings, random.r())) {
            for (auto& marker : tmpl->filter_blocks(jc::bd(), templatePosition, settings, "minecraft:structure_block", true))
                handleDataMarker(marker.nbt ? marker.nbt->gets("metadata") : std::string(), marker.pos, level, random, chunkBB);
            for (auto& jig : tmpl->filter_blocks(jc::bd(), templatePosition, settings, "minecraft:jigsaw", true)) {
                if (!jig.nbt) continue;
                std::string fs = jig.nbt->gets("final_state");
                if (fs.empty()) fs = "minecraft:air";
                level.L.set(jig.pos, parse_state_string(jc::bd(), fs));
            }
        }
    }
    void move(int dx, int dy, int dz) override { bb.move(dx, dy, dz); templatePosition = templatePosition.offset(dx, dy, dz); }

protected:
    virtual void handleDataMarker(const std::string& markerId, const BlockPos& position, JLevel& level, JRandom random, const BoundingBox& chunkBB) {}
};

inline BlockPos calculate_relative_position(const PlaceSettings& s, const BlockPos& p) { return transform_pos(p, s.mirror, s.rotation, s.pivot); }


// RandomizableContainer.setBlockEntityLootTable: only a container block entity takes the table (and draws a seed)
inline void set_block_entity_loot_table(JLevel& level, JRandom random, const BlockPos& pos, const std::string& table) {
    ChunkBlocks* c = level.L.chunk_for(pos.x, pos.z);
    if (!c) return;
    auto it = c->block_entities.find({pos.x, pos.y, pos.z});
    if (it == c->block_entities.end()) return;
    const std::string id = it->second.gets("id");
    static const char* kContainers[] = {"minecraft:chest", "minecraft:trapped_chest", "minecraft:barrel", "minecraft:hopper", "minecraft:dispenser", "minecraft:dropper", "minecraft:shulker_box", "minecraft:crafter", "minecraft:decorated_pot"};
    bool ok = false;
    for (const char* k : kContainers) if (id == k) ok = true;
    if (!ok) return;
    it->second.put("LootTable", pn::Nbt::str(table));
    it->second.put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, random.nextLong()));
}

}  // namespace pn::vg::jc
