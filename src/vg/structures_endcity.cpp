// End cities (EndCityStructure and EndCityPieces): a house with towers and bridges, made of templates stuck together at their connection points.
#include "template_piece.hpp"
#include <algorithm>

namespace pn::vg {

using namespace jc;

namespace {

PlaceSettings endcity_settings(bool overwrite, Rotation rotation) {
    PlaceSettings s;
    s.ignore_entities = true;
    s.processors.push_back(block_ignore_processor(!overwrite, true));
    s.rotation = rotation;
    return s;
}

class EndCityPiece : public TemplatePiece {
public:
    bool overwrite;
    EndCityPiece(TemplateManager& tm, const std::string& templateName, const BlockPos& position, Rotation rotation, bool ow)
        : TemplatePiece("minecraft:ecp", 0, tm, "minecraft:end_city/" + templateName, endcity_settings(ow, rotation), position), overwrite(ow) {}
protected:
    void handleDataMarker(const std::string& markerId, const BlockPos& position, JLevel& level, JRandom random, const BoundingBox& chunkBB) override {
        if (markerId.rfind("Chest", 0) == 0) {
            BlockPos chest = position.below();
            if (chunkBB.inside(chest)) set_block_entity_loot_table(level, random, chest, "minecraft:chests/end_city_treasure");
        } else if (chunkBB.inside(position)) {
            if (markerId.rfind("Sentry", 0) == 0) {
                add_entity(level, "minecraft:shulker", double(position.x) + 0.5, position.y, double(position.z) + 0.5, false);
            } else if (markerId.rfind("Elytra", 0) == 0) {
                pn::Nbt e = pn::Nbt::compound();
                e.put("id", pn::Nbt::str("minecraft:item_frame"));
                static const char* faces[] = {"down", "up", "north", "south", "west", "east"};
                Direction facing = Direction::SOUTH;
                switch (settings.rotation) {
                    case Rotation::CLOCKWISE_90: facing = Direction::WEST; break;
                    case Rotation::CLOCKWISE_180: facing = Direction::NORTH; break;
                    case Rotation::COUNTERCLOCKWISE_90: facing = Direction::EAST; break;
                    default: break;
                }
                e.put("Facing", pn::Nbt::make(pn::Nbt::Byte, facing.v == pn::vg::DOWN ? 0 : facing.v == pn::vg::UP ? 1 : facing.get2DDataValue() + 2));
                (void)faces;
                pn::Nbt item = pn::Nbt::compound();
                item.put("id", pn::Nbt::str("minecraft:elytra"));
                item.put("count", pn::Nbt::make(pn::Nbt::Int, 1));
                e.put("Item", item);
                e.put("TileX", pn::Nbt::make(pn::Nbt::Int, position.x));
                e.put("TileY", pn::Nbt::make(pn::Nbt::Int, position.y));
                e.put("TileZ", pn::Nbt::make(pn::Nbt::Int, position.z));
                level.L.add_entity(position.x, position.z, std::move(e));
            }
        }
    }
};

enum Generator { HOUSE_TOWER, TOWER, TOWER_BRIDGE, FAT_TOWER };

struct EndCityState { bool shipCreated = false; };

struct Pending { std::vector<std::shared_ptr<StructurePiece>>& pieces; };

std::shared_ptr<EndCityPiece> addPiece(TemplateManager& tm, EndCityPiece* parent, const BlockPos& offset, const std::string& name, Rotation rotation, bool overwrite) {
    auto child = std::make_shared<EndCityPiece>(tm, name, parent->templatePosition, rotation, overwrite);
    BlockPos m1 = calculate_relative_position(parent->settings, offset);
    BlockPos m2 = calculate_relative_position(child->settings, BlockPos(0, 0, 0));
    BlockPos origin(m1.x - m2.x, m1.y - m2.y, m1.z - m2.z);
    child->move(origin.x, origin.y, origin.z);
    return child;
}

std::shared_ptr<EndCityPiece> addHelper(std::vector<std::shared_ptr<StructurePiece>>& pieces, std::shared_ptr<EndCityPiece> p) { pieces.push_back(p); return p; }

Rotation rotated(Rotation a, Rotation b) {   // Rotation.getRotated
    return Rotation((int(a) + int(b)) & 3);
}

bool generate(TemplateManager& tm, EndCityState& st, Generator g, int genDepth, EndCityPiece* parent, const BlockPos* offset, std::vector<std::shared_ptr<StructurePiece>>& pieces, JRandom random);

bool recursiveChildren(TemplateManager& tm, EndCityState& st, Generator g, int genDepth, EndCityPiece* parent, const BlockPos* offset, std::vector<std::shared_ptr<StructurePiece>>& pieces, JRandom random) {
    if (genDepth > 8) return false;
    std::vector<std::shared_ptr<StructurePiece>> childPieces;
    if (generate(tm, st, g, genDepth, parent, offset, childPieces, random)) {
        bool collision = false;
        int childTag = random.nextInt();
        for (auto& child : childPieces) {
            child->gen_depth = childTag;
            StructurePiece* collisionPiece = nullptr;
            for (auto& p : pieces) if (p->bb.intersects(child->bb)) { collisionPiece = p.get(); break; }
            if (!collisionPiece || collisionPiece->gen_depth == parent->gen_depth) continue;
            collision = true;
            break;
        }
        if (!collision) {
            pieces.insert(pieces.end(), childPieces.begin(), childPieces.end());
            return true;
        }
    }
    return false;
}

bool generate(TemplateManager& tm, EndCityState& st, Generator g, int genDepth, EndCityPiece* parent, const BlockPos* offset, std::vector<std::shared_ptr<StructurePiece>>& pieces, JRandom random) {
    Rotation rotation = parent->settings.rotation;
    switch (g) {
        case HOUSE_TOWER: {
            if (genDepth > 8) return false;
            auto lastPiece = addHelper(pieces, addPiece(tm, parent, *offset, "base_floor", rotation, true));
            int numFloors = random.nextInt(3);
            if (numFloors == 0) {
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 4, -1), "base_roof", rotation, true));
            } else if (numFloors == 1) {
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 0, -1), "second_floor_2", rotation, false));
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 8, -1), "second_roof", rotation, false));
                recursiveChildren(tm, st, TOWER, genDepth + 1, lastPiece.get(), nullptr, pieces, random);
            } else if (numFloors == 2) {
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 0, -1), "second_floor_2", rotation, false));
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 4, -1), "third_floor_2", rotation, false));
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 8, -1), "third_roof", rotation, true));
                recursiveChildren(tm, st, TOWER, genDepth + 1, lastPiece.get(), nullptr, pieces, random);
            }
            return true;
        }
        case TOWER: {
            static const std::pair<Rotation, BlockPos> kBridges[4] = {{Rotation::NONE, BlockPos(1, -1, 0)}, {Rotation::CLOCKWISE_90, BlockPos(6, -1, 1)}, {Rotation::COUNTERCLOCKWISE_90, BlockPos(0, -1, 5)}, {Rotation::CLOCKWISE_180, BlockPos(5, -1, 6)}};
            std::shared_ptr<EndCityPiece> lastPiece;
            EndCityPiece* last = parent;
            int bx = 3 + random.nextInt(2);
            int bz = 3 + random.nextInt(2);
            lastPiece = addHelper(pieces, addPiece(tm, last, BlockPos(bx, -3, bz), "tower_base", rotation, true));
            lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(0, 7, 0), "tower_piece", rotation, true));
            EndCityPiece* bridgePiece = random.nextInt(3) == 0 ? lastPiece.get() : nullptr;
            int towerHeight = 1 + random.nextInt(3);
            for (int i = 0; i < towerHeight; ++i) {
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(0, 4, 0), "tower_piece", rotation, true));
                if (i >= towerHeight - 1 || !random.nextBoolean()) continue;
                bridgePiece = lastPiece.get();
            }
            if (bridgePiece) {
                for (auto& bridge : kBridges) {
                    if (!random.nextBoolean()) continue;
                    auto bridgeStart = addHelper(pieces, addPiece(tm, bridgePiece, bridge.second, "bridge_end", rotated(rotation, bridge.first), true));
                    recursiveChildren(tm, st, TOWER_BRIDGE, genDepth + 1, bridgeStart.get(), nullptr, pieces, random);
                }
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 4, -1), "tower_top", rotation, true));
            } else if (genDepth == 7) {
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 4, -1), "tower_top", rotation, true));
            } else {
                return recursiveChildren(tm, st, FAT_TOWER, genDepth + 1, lastPiece.get(), nullptr, pieces, random);
            }
            return true;
        }
        case TOWER_BRIDGE: {
            int bridgeLength = random.nextInt(4) + 1;
            auto lastPiece = addHelper(pieces, addPiece(tm, parent, BlockPos(0, 0, -4), "bridge_piece", rotation, true));
            lastPiece->gen_depth = -1;
            int nextY = 0;
            for (int i = 0; i < bridgeLength; ++i) {
                if (random.nextBoolean()) {
                    lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(0, nextY, -4), "bridge_piece", rotation, true));
                    nextY = 0;
                    continue;
                }
                lastPiece = random.nextBoolean() ? addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(0, nextY, -4), "bridge_steep_stairs", rotation, true))
                                                 : addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(0, nextY, -8), "bridge_gentle_stairs", rotation, true));
                nextY = 4;
            }
            if (st.shipCreated || random.nextInt(10 - genDepth) != 0) {
                BlockPos off(-3, nextY + 1, -11);
                if (!recursiveChildren(tm, st, HOUSE_TOWER, genDepth + 1, lastPiece.get(), &off, pieces, random)) return false;
            } else {
                int sx = -8 + random.nextInt(8);
                int sz = -70 + random.nextInt(10);
                addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(sx, nextY, sz), "ship", rotation, true));
                st.shipCreated = true;
            }
            lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(4, nextY, 0), "bridge_end", rotated(rotation, Rotation::CLOCKWISE_180), true));
            lastPiece->gen_depth = -1;
            return true;
        }
        case FAT_TOWER: {
            static const std::pair<Rotation, BlockPos> kBridges[4] = {{Rotation::NONE, BlockPos(4, -1, 0)}, {Rotation::CLOCKWISE_90, BlockPos(12, -1, 4)}, {Rotation::COUNTERCLOCKWISE_90, BlockPos(0, -1, 8)}, {Rotation::CLOCKWISE_180, BlockPos(8, -1, 12)}};
            auto lastPiece = addHelper(pieces, addPiece(tm, parent, BlockPos(-3, 4, -3), "fat_tower_base", rotation, true));
            lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(0, 4, 0), "fat_tower_middle", rotation, true));
            for (int i = 0; i < 2 && random.nextInt(3) != 0; ++i) {
                lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(0, 8, 0), "fat_tower_middle", rotation, true));
                for (auto& bridge : kBridges) {
                    if (!random.nextBoolean()) continue;
                    auto bridgeStart = addHelper(pieces, addPiece(tm, lastPiece.get(), bridge.second, "bridge_end", rotated(rotation, bridge.first), true));
                    recursiveChildren(tm, st, TOWER_BRIDGE, genDepth + 1, bridgeStart.get(), nullptr, pieces, random);
                }
            }
            lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-2, 8, -2), "fat_tower_top", rotation, true));
            return true;
        }
    }
    return false;
}

class EndCityStructure : public Structure {
public:
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        static const Rotation kRots[4] = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
        Rotation rotation = kRots[ctx.random.next_int(4)];
        BlockPos startPos = structure_lowest_y_in_5by5_box_offset_7(ctx, rotation);
        if (startPos.y < 60) return std::nullopt;
        GenerationStub stub;
        stub.position = startPos;
        stub.generate = [&ctx, startPos, rotation](std::vector<std::shared_ptr<StructurePiece>>& out) {
            TemplateManager& tm = *ctx.env.templates;
            JRandom random(ctx.random);
            EndCityState st;
            std::vector<std::shared_ptr<StructurePiece>> pieces;
            auto lastPiece = addHelper(pieces, std::make_shared<EndCityPiece>(tm, "base_floor", startPos, rotation, true));
            lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 0, -1), "second_floor_1", rotation, false));
            lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 4, -1), "third_floor_1", rotation, false));
            lastPiece = addHelper(pieces, addPiece(tm, lastPiece.get(), BlockPos(-1, 8, -1), "third_roof", rotation, true));
            recursiveChildren(tm, st, TOWER, 1, lastPiece.get(), nullptr, pieces, random);
            out = pieces;
        };
        return stub;
    }
};

struct RegisterEndCity {
    RegisterEndCity() {
        structure_factories()["minecraft:end_city"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<EndCityStructure>(); };
    }
} register_endcity;

}  // namespace

}  // namespace pn::vg
