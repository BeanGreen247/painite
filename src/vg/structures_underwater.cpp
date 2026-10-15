// Shipwrecks and ocean ruins (ShipwreckStructure/Pieces, OceanRuinStructure/Pieces): template pieces that settle on the sea floor or the beach.
#include "template_piece.hpp"
#include <algorithm>

namespace pn::vg {

using namespace jc;

namespace {

const Rotation kRots[4] = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};

// ---------------------------------------------------------------- shipwreck

const BlockPos kShipPivot(4, 0, 15);

const char* const kBeached[] = {"with_mast", "sideways_full", "sideways_fronthalf", "sideways_backhalf", "rightsideup_full", "rightsideup_fronthalf", "rightsideup_backhalf", "with_mast_degraded",
                                "rightsideup_full_degraded", "rightsideup_fronthalf_degraded", "rightsideup_backhalf_degraded"};
const char* const kOcean[] = {"with_mast", "upsidedown_full", "upsidedown_fronthalf", "upsidedown_backhalf", "sideways_full", "sideways_fronthalf", "sideways_backhalf", "rightsideup_full",
                              "rightsideup_fronthalf", "rightsideup_backhalf", "with_mast_degraded", "upsidedown_full_degraded", "upsidedown_fronthalf_degraded", "upsidedown_backhalf_degraded",
                              "sideways_full_degraded", "sideways_fronthalf_degraded", "sideways_backhalf_degraded", "rightsideup_full_degraded", "rightsideup_fronthalf_degraded", "rightsideup_backhalf_degraded"};

PlaceSettings shipwreck_settings(Rotation rotation) {
    PlaceSettings s;
    s.rotation = rotation;
    s.mirror = Mirror::NONE;
    s.pivot = kShipPivot;
    s.processors.push_back(block_ignore_processor(true, true));
    return s;
}

class ShipwreckPiece : public TemplatePiece {
public:
    bool isBeached;
    bool heightAdjusted = false;
    ShipwreckPiece(TemplateManager& tm, const std::string& loc, const BlockPos& position, Rotation rotation, bool beached) : TemplatePiece("minecraft:shipwreck", 0, tm, loc, shipwreck_settings(rotation), position), isBeached(beached) {}
    bool isTooBigToFitInWorldGenRegion() const { return tmpl->size_x > 32 || tmpl->size_y > 32; }
    int calculateBeachedPosition(int minY, JRandom random) const { return minY - tmpl->size_y / 2 - random.nextInt(3); }
    void adjustPositionHeight(int newHeight) {
        heightAdjusted = true;
        templatePosition = BlockPos(templatePosition.x, newHeight, templatePosition.z);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
        if (!tmpl) return;
        if (heightAdjusted || isTooBigToFitInWorldGenRegion()) { TemplatePiece::postProcess(level, random, chunkBB, chunkPos, referencePos); return; }
        int minY = level.getMaxY() + 1;
        int mean = 0;
        HeightmapType hm = isBeached ? WORLD_SURFACE_WG : OCEAN_FLOOR_WG;
        int baseSize = tmpl->size_x * tmpl->size_z;
        if (baseSize == 0) mean = level.getHeight(hm, templatePosition.x, templatePosition.z);
        else {
            BlockPos corner = templatePosition.offset(tmpl->size_x - 1, 0, tmpl->size_z - 1);
            for (int y = templatePosition.y; y <= corner.y; ++y)
                for (int z = templatePosition.z; z <= corner.z; ++z)
                    for (int x = templatePosition.x; x <= corner.x; ++x) {
                        int h = level.getHeight(hm, x, z);
                        mean += h;
                        minY = std::min(minY, h);
                    }
            mean /= baseSize;
        }
        adjustPositionHeight(isBeached ? calculateBeachedPosition(minY, random) : mean);
        TemplatePiece::postProcess(level, random, chunkBB, chunkPos, referencePos);
    }
protected:
    void handleDataMarker(const std::string& markerId, const BlockPos& position, JLevel& level, JRandom random, const BoundingBox&) override {
        const char* table = markerId == "map_chest" ? "minecraft:chests/shipwreck_map" : markerId == "treasure_chest" ? "minecraft:chests/shipwreck_treasure" : markerId == "supply_chest" ? "minecraft:chests/shipwreck_supply" : nullptr;
        if (table) set_block_entity_loot_table(level, random, position.below(), table);
    }
};

class ShipwreckStructure : public Structure {
public:
    bool isBeached = false;
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        HeightmapType type = isBeached ? WORLD_SURFACE_WG : OCEAN_FLOOR_WG;
        return on_top_of_chunk_center(ctx, type, [this, &ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
            JRandom random(ctx.random);
            Rotation rotation = kRots[random.nextInt(4)];
            BlockPos offset(ctx.chunk.x * 16, 90, ctx.chunk.z * 16);
            std::string loc = std::string("minecraft:shipwreck/") + (isBeached ? kBeached[random.nextInt(11)] : kOcean[random.nextInt(20)]);
            auto piece = std::make_shared<ShipwreckPiece>(*ctx.env.templates, loc, offset, rotation, isBeached);
            out.push_back(piece);
            if (piece->tmpl && piece->isTooBigToFitInWorldGenRegion()) {
                int height;
                const BoundingBox& bb = piece->bb;
                if (isBeached) {
                    int minY = structure_lowest_y(ctx, bb.x0, bb.z0, bb.x_span(), bb.z_span());
                    height = piece->calculateBeachedPosition(minY, random);
                } else height = structure_mean_first_occupied_height(ctx, bb.x0, bb.x_span(), bb.z0, bb.z_span());
                piece->adjustPositionHeight(height);
            }
        });
    }
};

// ---------------------------------------------------------------- ocean ruins

const char* const kWarm[] = {"warm_1", "warm_2", "warm_3", "warm_4", "warm_5", "warm_6", "warm_7", "warm_8"};
const char* const kBrick[] = {"brick_1", "brick_2", "brick_3", "brick_4", "brick_5", "brick_6", "brick_7", "brick_8"};
const char* const kCracked[] = {"cracked_1", "cracked_2", "cracked_3", "cracked_4", "cracked_5", "cracked_6", "cracked_7", "cracked_8"};
const char* const kMossy[] = {"mossy_1", "mossy_2", "mossy_3", "mossy_4", "mossy_5", "mossy_6", "mossy_7", "mossy_8"};
const char* const kBigBrick[] = {"big_brick_1", "big_brick_2", "big_brick_3", "big_brick_8"};
const char* const kBigMossy[] = {"big_mossy_1", "big_mossy_2", "big_mossy_3", "big_mossy_8"};
const char* const kBigCracked[] = {"big_cracked_1", "big_cracked_2", "big_cracked_3", "big_cracked_8"};
const char* const kBigWarm[] = {"big_warm_4", "big_warm_5", "big_warm_6", "big_warm_7"};

PlaceSettings ocean_ruin_settings(TemplateManager& tm, Rotation rotation, float integrity, bool cold) {
    PlaceSettings s;
    s.rotation = rotation;
    s.mirror = Mirror::NONE;
    s.processors.push_back(block_rot_processor(integrity));
    s.processors.push_back(block_ignore_processor(true, true));
    if (cold) s.processors.push_back(archy_rule_processor(tm.bd(), "minecraft:gravel", "minecraft:suspicious_gravel", "minecraft:archaeology/ocean_ruin_cold"));
    else s.processors.push_back(archy_rule_processor(tm.bd(), "minecraft:sand", "minecraft:suspicious_sand", "minecraft:archaeology/ocean_ruin_warm"));
    return s;
}

class OceanRuinPiece : public TemplatePiece {
public:
    bool isLarge;
    OceanRuinPiece(TemplateManager& tm, const std::string& loc, const BlockPos& position, Rotation rotation, float integrity, bool cold, bool large)
        : TemplatePiece("minecraft:orp", 0, tm, loc, ocean_ruin_settings(tm, rotation, integrity, cold), position), isLarge(large) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
        if (!tmpl) return;
        int height = level.getHeight(OCEAN_FLOOR_WG, templatePosition.x, templatePosition.z);
        templatePosition = BlockPos(templatePosition.x, height, templatePosition.z);
        BlockPos rel = transform_pos(BlockPos(tmpl->size_x - 1, 0, tmpl->size_z - 1), Mirror::NONE, settings.rotation, BlockPos(0, 0, 0));
        BlockPos corner = templatePosition.offset(rel.x, rel.y, rel.z);
        templatePosition = BlockPos(templatePosition.x, getHeight(templatePosition, level, corner), templatePosition.z);
        TemplatePiece::postProcess(level, random, chunkBB, chunkPos, referencePos);
    }
protected:
    void handleDataMarker(const std::string& markerId, const BlockPos& position, JLevel& level, JRandom random, const BoundingBox&) override {
        if (markerId == "chest") {
            bool water = jc::bd().water(level.L.get(position));
            level.L.set(position, blk("minecraft:chest").setValue("waterlogged", water).id);
            pn::Nbt be = pn::Nbt::compound();
            be.put("id", pn::Nbt::str("minecraft:chest"));
            be.put("x", pn::Nbt::make(pn::Nbt::Int, position.x));
            be.put("y", pn::Nbt::make(pn::Nbt::Int, position.y));
            be.put("z", pn::Nbt::make(pn::Nbt::Int, position.z));
            be.put("LootTable", pn::Nbt::str(isLarge ? "minecraft:chests/underwater_ruin_big" : "minecraft:chests/underwater_ruin_small"));
            be.put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, random.nextLong()));
            level.L.set_block_entity(position, std::move(be));
        } else if (markerId == "drowned") {
            add_entity(level, "minecraft:drowned", double(position.x) + 0.5, position.y, double(position.z) + 0.5);
            if (position.y > level.getSeaLevel()) level.L.set(position, blk("minecraft:air").id);
            else level.L.set(position, blk("minecraft:water").id);
        }
    }
private:
    int getHeight(const BlockPos& pos, JLevel& level, const BlockPos& corner) {
        int newY = pos.y;
        int minY = 512;
        int topY = newY - 1;
        int area = 0;
        int x0 = std::min(pos.x, corner.x), x1 = std::max(pos.x, corner.x), z0 = std::min(pos.z, corner.z), z1 = std::max(pos.z, corner.z);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                int floorY = pos.y - 1;
                BS state = level.getBlockState(BlockPos(x, floorY, z));
                while ((state.isAir() || jc::bd().water(state.id) || state.is(Tag("#minecraft:ice"))) && floorY > level.getMinY() + 1) {
                    state = level.getBlockState(BlockPos(x, --floorY, z));
                }
                minY = std::min(minY, floorY);
                if (floorY >= topY - 2) continue;
                ++area;
            }
        int width = std::abs(pos.x - corner.x);
        if (topY - minY > 2 && area > width - 2) newY = minY + 1;
        return newY;
    }
};

class OceanRuinStructure : public Structure {
public:
    bool cold = false;
    float largeProbability = 0.3f, clusterProbability = 0.9f;
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        return on_top_of_chunk_center(ctx, OCEAN_FLOOR_WG, [this, &ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
            JRandom random(ctx.random);
            BlockPos offset(ctx.chunk.x * 16, 90, ctx.chunk.z * 16);
            Rotation rotation = kRots[random.nextInt(4)];
            TemplateManager& tm = *ctx.env.templates;
            bool isLarge = random.nextFloat() <= largeProbability;
            float baseIntegrity = isLarge ? 0.9f : 0.8f;
            addPiece(tm, offset, rotation, out, random, isLarge, baseIntegrity);
            if (isLarge && random.nextFloat() <= clusterProbability) addClusterRuins(tm, random, rotation, offset, out);
        });
    }
private:
    static std::string ruin(const char* name) { return std::string("minecraft:underwater_ruin/") + name; }
    void addPiece(TemplateManager& tm, const BlockPos& position, Rotation rotation, std::vector<std::shared_ptr<StructurePiece>>& out, JRandom random, bool isLarge, float baseIntegrity) const {
        if (!cold) {
            std::string start = isLarge ? ruin(kBigWarm[random.nextInt(4)]) : ruin(kWarm[random.nextInt(8)]);
            out.push_back(std::make_shared<OceanRuinPiece>(tm, start, position, rotation, baseIntegrity, false, isLarge));
        } else {
            const char* const* bricks = isLarge ? kBigBrick : kBrick;
            const char* const* cracked = isLarge ? kBigCracked : kCracked;
            const char* const* mossy = isLarge ? kBigMossy : kMossy;
            int idx = random.nextInt(isLarge ? 4 : 8);
            out.push_back(std::make_shared<OceanRuinPiece>(tm, ruin(bricks[idx]), position, rotation, baseIntegrity, true, isLarge));
            out.push_back(std::make_shared<OceanRuinPiece>(tm, ruin(cracked[idx]), position, rotation, 0.7f, true, isLarge));
            out.push_back(std::make_shared<OceanRuinPiece>(tm, ruin(mossy[idx]), position, rotation, 0.5f, true, isLarge));
        }
    }
    static int nextInt(JRandom random, int lo, int hi) { return lo + random.nextInt(hi - lo + 1); }   // Mth.nextInt(random, min, max)
    void addClusterRuins(TemplateManager& tm, JRandom random, Rotation rotation, const BlockPos& p, std::vector<std::shared_ptr<StructurePiece>>& out) const {
        BlockPos parentPos(p.x, 90, p.z);
        BlockPos rel = transform_pos(BlockPos(15, 0, 15), Mirror::NONE, rotation, BlockPos(0, 0, 0));
        BlockPos parentCorner(rel.x + parentPos.x, rel.y + parentPos.y, rel.z + parentPos.z);
        BoundingBox parentBB(parentPos.x, parentPos.y, parentPos.z, parentCorner.x, parentCorner.y, parentCorner.z);
        BlockPos origin(std::min(parentPos.x, parentCorner.x), parentPos.y, std::min(parentPos.z, parentCorner.z));
        std::vector<BlockPos> all;
        all.push_back(origin.offset(-16 + nextInt(random, 1, 8), 0, 16 + nextInt(random, 1, 7)));
        all.push_back(origin.offset(-16 + nextInt(random, 1, 8), 0, nextInt(random, 1, 7)));
        all.push_back(origin.offset(-16 + nextInt(random, 1, 8), 0, -16 + nextInt(random, 4, 8)));
        all.push_back(origin.offset(nextInt(random, 1, 7), 0, 16 + nextInt(random, 1, 7)));
        all.push_back(origin.offset(nextInt(random, 1, 7), 0, -16 + nextInt(random, 4, 6)));
        all.push_back(origin.offset(16 + nextInt(random, 1, 7), 0, 16 + nextInt(random, 3, 8)));
        all.push_back(origin.offset(16 + nextInt(random, 1, 7), 0, nextInt(random, 1, 7)));
        all.push_back(origin.offset(16 + nextInt(random, 1, 7), 0, -16 + nextInt(random, 4, 8)));
        int ruins = nextInt(random, 4, 8);
        for (int i = 0; i < ruins; ++i) {
            if (all.empty()) continue;
            int idx = random.nextInt(int(all.size()));
            BlockPos pos = all[size_t(idx)];
            all.erase(all.begin() + idx);
            Rotation nextRotation = kRots[random.nextInt(4)];
            BlockPos r2 = transform_pos(BlockPos(5, 0, 6), Mirror::NONE, nextRotation, BlockPos(0, 0, 0));
            BlockPos nextCorner(r2.x + pos.x, r2.y + pos.y, r2.z + pos.z);
            BoundingBox nextBB(pos.x, pos.y, pos.z, nextCorner.x, nextCorner.y, nextCorner.z);
            if (nextBB.intersects(parentBB)) continue;
            addPiece(tm, pos, nextRotation, out, random, false, 0.8f);
        }
    }
};

struct RegisterUnderwater {
    RegisterUnderwater() {
        structure_factories()["minecraft:shipwreck"] = [](const Json& j, const Registry&) -> std::unique_ptr<Structure> {
            auto s = std::make_unique<ShipwreckStructure>();
            s->isBeached = j.get_bool("is_beached", false);
            return s;
        };
        structure_factories()["minecraft:ocean_ruin"] = [](const Json& j, const Registry&) -> std::unique_ptr<Structure> {
            auto s = std::make_unique<OceanRuinStructure>();
            s->cold = j.get_string("biome_temp") == "cold";
            s->largeProbability = j.at("large_probability").float_value();
            s->clusterProbability = j.at("cluster_probability").float_value();
            return s;
        };
    }
} register_underwater;

}  // namespace

}  // namespace pn::vg
