// Ocean monuments (OceanMonumentStructure and OceanMonumentPieces): the building and its rooms, laid out on a 5 x 3 x 5 grid of room definitions.
#include "legacy_piece.hpp"
#include <algorithm>
#include <set>
#include <functional>

namespace pn::vg {

using namespace jc;

namespace {

#define BASE_GRAY (blk("minecraft:prismarine"))
#define BASE_LIGHT (blk("minecraft:prismarine_bricks"))
#define BASE_BLACK (blk("minecraft:dark_prismarine"))
#define DOT_DECO_DATA BASE_LIGHT
#define LAMP_BLOCK (blk("minecraft:sea_lantern"))
#define FILL_BLOCK (blk("minecraft:water"))

struct RoomDefinition {
    int index;
    RoomDefinition* connections[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    bool hasOpening[6] = {false, false, false, false, false, false};
    bool claimed = false, isSource = false;
    int scanIndex = 0;
    explicit RoomDefinition(int roomIndex) : index(roomIndex) {}
    void setConnection(const Direction& direction, RoomDefinition* definition) {
        connections[direction.get3DDataValue()] = definition;
        definition->connections[direction.getOpposite().get3DDataValue()] = this;
    }
    void updateOpenings() { for (int i = 0; i < 6; ++i) hasOpening[i] = connections[i] != nullptr; }
    bool findSource(int scanIndex_) {
        if (isSource) return true;
        scanIndex = scanIndex_;
        for (int i = 0; i < 6; ++i) {
            if (!connections[i] || !hasOpening[i] || connections[i]->scanIndex == scanIndex_ || !connections[i]->findSource(scanIndex_)) continue;
            return true;
        }
        return false;
    }
    bool isSpecial() const { return index >= 75; }
    int countOpenings() const { int c = 0; for (int i = 0; i < 6; ++i) if (hasOpening[i]) ++c; return c; }
};

class OceanMonumentPiece : public LegacyPiece {
public:
    RoomDefinition* roomDefinition = nullptr;
    static constexpr int GRIDROOM_SOURCE_INDEX = 2;
    static int getRoomIndex(int roomX, int roomY, int roomZ) { return roomY * 25 + roomZ * 5 + roomX; }
protected:
    static constexpr int GRIDROOM_TOP_CONNECT_INDEX = 2 * 25 + 2 * 5 + 0 * 1 - 0;   // getRoomIndex(2, 2, 0)
    OceanMonumentPiece(const char* id, const Direction& orientation, int genDepth, const BoundingBox& box) { type = id; gen_depth = genDepth; bb = box; setOrientation(orientation); }
    OceanMonumentPiece(const char* id, int genDepth, const Direction& orientation, RoomDefinition* def, int roomWidth, int roomHeight, int roomDepth) {
        type = id;
        gen_depth = genDepth;
        bb = makeRoomBox(orientation, def, roomWidth, roomHeight, roomDepth);
        setOrientation(orientation);
        roomDefinition = def;
    }
    static BoundingBox makeRoomBox(const Direction& orientation, RoomDefinition* def, int roomWidth, int roomHeight, int roomDepth) {
        int roomIndex = def->index;
        int roomX = roomIndex % 5, roomZ = roomIndex / 5 % 5, roomY = roomIndex / 25;
        BoundingBox box = makeBoundingBox(0, 0, 0, orientation, roomWidth * 8, roomHeight * 4, roomDepth * 8);
        switch (orientation.v) {
            case pn::vg::NORTH: box.move(roomX * 8, roomY * 4, -(roomZ + roomDepth) * 8 + 1); break;
            case pn::vg::SOUTH: box.move(roomX * 8, roomY * 4, roomZ * 8); break;
            case pn::vg::WEST: box.move(-(roomZ + roomDepth) * 8 + 1, roomY * 4, roomX * 8); break;
            default: box.move(roomZ * 8, roomY * 4, roomX * 8);
        }
        return box;
    }
    void generateWaterBox(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z) {
                    BS block = getBlock(level, x, y, z, chunkBB);
                    if (block.is("minecraft:ice") || block.is("minecraft:packed_ice") || block.is("minecraft:blue_ice") || block.is("minecraft:water")) {
                        if (!block.is("minecraft:water")) continue;   // FILL_KEEP: ice stays, water too (below)
                    }
                    if (block.is("minecraft:ice") || block.is("minecraft:packed_ice") || block.is("minecraft:blue_ice")) continue;
                    if (block.is("minecraft:water")) continue;   // FILL_KEEP contains the fill block's block
                    if (getWorldY(y) >= level.getSeaLevel() && block != FILL_BLOCK) { placeBlock(level, blk("minecraft:air"), x, y, z, chunkBB); continue; }
                    placeBlock(level, FILL_BLOCK, x, y, z, chunkBB);
                }
    }
    void generateDefaultFloor(JLevel& level, const BoundingBox& chunkBB, int xOff, int zOff, bool downOpening) {
        if (downOpening) {
            generateBox(level, chunkBB, xOff + 0, 0, zOff + 0, xOff + 2, 0, zOff + 8 - 1, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, xOff + 5, 0, zOff + 0, xOff + 8 - 1, 0, zOff + 8 - 1, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, xOff + 3, 0, zOff + 0, xOff + 4, 0, zOff + 2, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, xOff + 3, 0, zOff + 5, xOff + 4, 0, zOff + 8 - 1, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, xOff + 3, 0, zOff + 2, xOff + 4, 0, zOff + 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, xOff + 3, 0, zOff + 5, xOff + 4, 0, zOff + 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, xOff + 2, 0, zOff + 3, xOff + 2, 0, zOff + 4, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, xOff + 5, 0, zOff + 3, xOff + 5, 0, zOff + 4, BASE_LIGHT, BASE_LIGHT, false);
        } else {
            generateBox(level, chunkBB, xOff + 0, 0, zOff + 0, xOff + 8 - 1, 0, zOff + 8 - 1, BASE_GRAY, BASE_GRAY, false);
        }
    }
    void generateBoxOnFillOnly(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, const BS& targetBlock) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z) {
                    if (getBlock(level, x, y, z, chunkBB) != FILL_BLOCK) continue;
                    placeBlock(level, targetBlock, x, y, z, chunkBB);
                }
    }
    bool chunkIntersects(const BoundingBox& chunkBB, int x0, int z0, int x1, int z1) {
        int wx0 = getWorldX(x0, z0), wz0 = getWorldZ(x0, z0), wx1 = getWorldX(x1, z1), wz1 = getWorldZ(x1, z1);
        return chunkBB.intersects(std::min(wx0, wx1), std::min(wz0, wz1), std::max(wx0, wx1), std::max(wz0, wz1));
    }
    void spawnElder(JLevel& level, const BoundingBox& chunkBB, int x, int y, int z) {
        BlockPos pos = getWorldPos(x, y, z);
        if (!chunkBB.inside(pos)) return;
        pn::Nbt e = pn::Nbt::compound();
        e.put("id", pn::Nbt::str("minecraft:elder_guardian"));
        pn::Nbt p = pn::Nbt::list_of(pn::Nbt::Double);
        for (double v : {double(pos.x) + 0.5, double(pos.y), double(pos.z) + 0.5}) { pn::Nbt d; d.type = pn::Nbt::Double; d.f = v; p.list.push_back(d); }
        e.put("Pos", p);
        e.put("PersistenceRequired", pn::Nbt::make(pn::Nbt::Byte, 1));
        level.L.add_entity(double(pos.x) + 0.5, double(pos.z) + 0.5, std::move(e));
    }
};

class OceanMonumentPenthouse : public OceanMonumentPiece {
public:
    OceanMonumentPenthouse(const Direction& orientation, const BoundingBox& bb) : OceanMonumentPiece("minecraft:ompenthouse", orientation, 1, bb) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 2, -1, 2, 11, -1, 11, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 0, -1, 0, 1, -1, 11, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 12, -1, 0, 13, -1, 11, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 2, -1, 0, 11, -1, 1, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 2, -1, 12, 11, -1, 13, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 0, 0, 0, 0, 0, 13, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 13, 0, 0, 13, 0, 13, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 0, 0, 12, 0, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 0, 13, 12, 0, 13, BASE_LIGHT, BASE_LIGHT, false);
            for (int i = 2; i <= 11; i += 3) {
                placeBlock(level, LAMP_BLOCK, 0, 0, i, chunkBB);
                placeBlock(level, LAMP_BLOCK, 13, 0, i, chunkBB);
                placeBlock(level, LAMP_BLOCK, i, 0, 0, chunkBB);
            }
            generateBox(level, chunkBB, 2, 0, 3, 4, 0, 9, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 9, 0, 3, 11, 0, 9, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 4, 0, 9, 9, 0, 11, BASE_LIGHT, BASE_LIGHT, false);
            placeBlock(level, BASE_LIGHT, 5, 0, 8, chunkBB);
            placeBlock(level, BASE_LIGHT, 8, 0, 8, chunkBB);
            placeBlock(level, BASE_LIGHT, 10, 0, 10, chunkBB);
            placeBlock(level, BASE_LIGHT, 3, 0, 10, chunkBB);
            generateBox(level, chunkBB, 3, 0, 3, 3, 0, 7, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 10, 0, 3, 10, 0, 7, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 6, 0, 10, 7, 0, 10, BASE_BLACK, BASE_BLACK, false);
            int x = 3;
            for (int i = 0; i < 2; ++i) {
                for (int z = 2; z <= 8; z += 3) {
                    generateBox(level, chunkBB, x, 0, z, x, 2, z, BASE_LIGHT, BASE_LIGHT, false);
                }
                x = 10;
            }
            generateBox(level, chunkBB, 5, 0, 10, 5, 2, 10, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 8, 0, 10, 8, 2, 10, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, -1, 7, 7, -1, 8, BASE_BLACK, BASE_BLACK, false);
            generateWaterBox(level, chunkBB, 6, -1, 3, 7, -1, 4);
            spawnElder(level, chunkBB, 6, 1, 6);
    }
};

class OceanMonumentWingRoom : public OceanMonumentPiece {
public:
    int mainDesign;
    OceanMonumentWingRoom(const Direction& orientation, const BoundingBox& bb, int randomValue) : OceanMonumentPiece("minecraft:omwr", orientation, 1, bb) {mainDesign = randomValue & 1;}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            if (mainDesign == 0) {
                for (int i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, 10 - i, 3 - i, 20 - i, 12 + i, 3 - i, 20, BASE_LIGHT, BASE_LIGHT, false);
                }
                generateBox(level, chunkBB, 7, 0, 6, 15, 0, 16, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 6, 0, 6, 6, 3, 20, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 16, 0, 6, 16, 3, 20, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 7, 1, 7, 7, 1, 20, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 15, 1, 7, 15, 1, 20, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 7, 1, 6, 9, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 13, 1, 6, 15, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 8, 1, 7, 9, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 13, 1, 7, 14, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 9, 0, 5, 13, 0, 5, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 10, 0, 7, 12, 0, 7, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 8, 0, 10, 8, 0, 12, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 14, 0, 10, 14, 0, 12, BASE_BLACK, BASE_BLACK, false);
                for (int z = 18; z >= 7; z -= 3) {
                    placeBlock(level, LAMP_BLOCK, 6, 3, z, chunkBB);
                    placeBlock(level, LAMP_BLOCK, 16, 3, z, chunkBB);
                }
                placeBlock(level, LAMP_BLOCK, 10, 0, 10, chunkBB);
                placeBlock(level, LAMP_BLOCK, 12, 0, 10, chunkBB);
                placeBlock(level, LAMP_BLOCK, 10, 0, 12, chunkBB);
                placeBlock(level, LAMP_BLOCK, 12, 0, 12, chunkBB);
                placeBlock(level, LAMP_BLOCK, 8, 3, 6, chunkBB);
                placeBlock(level, LAMP_BLOCK, 14, 3, 6, chunkBB);
                placeBlock(level, BASE_LIGHT, 4, 2, 4, chunkBB);
                placeBlock(level, LAMP_BLOCK, 4, 1, 4, chunkBB);
                placeBlock(level, BASE_LIGHT, 4, 0, 4, chunkBB);
                placeBlock(level, BASE_LIGHT, 18, 2, 4, chunkBB);
                placeBlock(level, LAMP_BLOCK, 18, 1, 4, chunkBB);
                placeBlock(level, BASE_LIGHT, 18, 0, 4, chunkBB);
                placeBlock(level, BASE_LIGHT, 4, 2, 18, chunkBB);
                placeBlock(level, LAMP_BLOCK, 4, 1, 18, chunkBB);
                placeBlock(level, BASE_LIGHT, 4, 0, 18, chunkBB);
                placeBlock(level, BASE_LIGHT, 18, 2, 18, chunkBB);
                placeBlock(level, LAMP_BLOCK, 18, 1, 18, chunkBB);
                placeBlock(level, BASE_LIGHT, 18, 0, 18, chunkBB);
                placeBlock(level, BASE_LIGHT, 9, 7, 20, chunkBB);
                placeBlock(level, BASE_LIGHT, 13, 7, 20, chunkBB);
                generateBox(level, chunkBB, 6, 0, 21, 7, 4, 21, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 15, 0, 21, 16, 4, 21, BASE_LIGHT, BASE_LIGHT, false);
                spawnElder(level, chunkBB, 11, 2, 16);
            } else if (mainDesign == 1) {
                int i;
                generateBox(level, chunkBB, 9, 3, 18, 13, 3, 20, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 9, 0, 18, 9, 2, 18, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 13, 0, 18, 13, 2, 18, BASE_LIGHT, BASE_LIGHT, false);
                int x = 9;
                int z = 20;
                int y = 5;
                for (i = 0; i < 2; ++i) {
                    placeBlock(level, BASE_LIGHT, x, 6, 20, chunkBB);
                    placeBlock(level, LAMP_BLOCK, x, 5, 20, chunkBB);
                    placeBlock(level, BASE_LIGHT, x, 4, 20, chunkBB);
                    x = 13;
                }
                generateBox(level, chunkBB, 7, 3, 7, 15, 3, 14, BASE_LIGHT, BASE_LIGHT, false);
                x = 10;
                for (i = 0; i < 2; ++i) {
                    generateBox(level, chunkBB, x, 0, 10, x, 6, 10, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, x, 0, 12, x, 6, 12, BASE_LIGHT, BASE_LIGHT, false);
                    placeBlock(level, LAMP_BLOCK, x, 0, 10, chunkBB);
                    placeBlock(level, LAMP_BLOCK, x, 0, 12, chunkBB);
                    placeBlock(level, LAMP_BLOCK, x, 4, 10, chunkBB);
                    placeBlock(level, LAMP_BLOCK, x, 4, 12, chunkBB);
                    x = 12;
                }
                x = 8;
                for (i = 0; i < 2; ++i) {
                    generateBox(level, chunkBB, x, 0, 7, x, 2, 7, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, x, 0, 14, x, 2, 14, BASE_LIGHT, BASE_LIGHT, false);
                    x = 14;
                }
                generateBox(level, chunkBB, 8, 3, 8, 8, 3, 13, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 14, 3, 8, 14, 3, 13, BASE_BLACK, BASE_BLACK, false);
                spawnElder(level, chunkBB, 11, 5, 13);
            }
    }
};

class OceanMonumentCoreRoom : public OceanMonumentPiece {
public:
    OceanMonumentCoreRoom(const Direction& orientation, RoomDefinition* definition) : OceanMonumentPiece("minecraft:omcr", 1, orientation, definition, 2, 2, 2) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBoxOnFillOnly(level, chunkBB, 1, 8, 0, 14, 8, 14, BASE_GRAY);
            int y = 7;
            BS block = BASE_LIGHT;
            generateBox(level, chunkBB, 0, 7, 0, 0, 7, 15, block, block, false);
            generateBox(level, chunkBB, 15, 7, 0, 15, 7, 15, block, block, false);
            generateBox(level, chunkBB, 1, 7, 0, 15, 7, 0, block, block, false);
            generateBox(level, chunkBB, 1, 7, 15, 14, 7, 15, block, block, false);
            for (y = 1; y <= 6; ++y) {
                block = BASE_LIGHT;
                if (y == 2 || y == 6) {
                    block = BASE_GRAY;
                }
                for (int x = 0; x <= 15; x += 15) {
                    generateBox(level, chunkBB, x, y, 0, x, y, 1, block, block, false);
                    generateBox(level, chunkBB, x, y, 6, x, y, 9, block, block, false);
                    generateBox(level, chunkBB, x, y, 14, x, y, 15, block, block, false);
                }
                generateBox(level, chunkBB, 1, y, 0, 1, y, 0, block, block, false);
                generateBox(level, chunkBB, 6, y, 0, 9, y, 0, block, block, false);
                generateBox(level, chunkBB, 14, y, 0, 14, y, 0, block, block, false);
                generateBox(level, chunkBB, 1, y, 15, 14, y, 15, block, block, false);
            }
            generateBox(level, chunkBB, 6, 3, 6, 9, 6, 9, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 7, 4, 7, 8, 5, 8, blk("minecraft:gold_block"), blk("minecraft:gold_block"), false);
            for (y = 3; y <= 6; y += 3) {
                for (int x = 6; x <= 9; x += 3) {
                    placeBlock(level, LAMP_BLOCK, x, y, 6, chunkBB);
                    placeBlock(level, LAMP_BLOCK, x, y, 9, chunkBB);
                }
            }
            generateBox(level, chunkBB, 5, 1, 6, 5, 2, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 1, 9, 5, 2, 9, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 1, 6, 10, 2, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 1, 9, 10, 2, 9, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 1, 5, 6, 2, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 9, 1, 5, 9, 2, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 1, 10, 6, 2, 10, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 9, 1, 10, 9, 2, 10, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 2, 5, 5, 6, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 2, 10, 5, 6, 10, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 2, 5, 10, 6, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 2, 10, 10, 6, 10, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 7, 1, 5, 7, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 7, 1, 10, 7, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 7, 9, 5, 7, 14, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 7, 9, 10, 7, 14, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 7, 5, 6, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 7, 10, 6, 7, 10, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 9, 7, 5, 14, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 9, 7, 10, 14, 7, 10, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 2, 1, 2, 2, 1, 3, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 3, 1, 2, 3, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 13, 1, 2, 13, 1, 3, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 12, 1, 2, 12, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 2, 1, 12, 2, 1, 13, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 3, 1, 13, 3, 1, 13, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 13, 1, 12, 13, 1, 13, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 12, 1, 13, 12, 1, 13, BASE_LIGHT, BASE_LIGHT, false);
    }
};

class OceanMonumentDoubleYZRoom : public OceanMonumentPiece {
public:
    OceanMonumentDoubleYZRoom(const Direction& orientation, RoomDefinition* definition) : OceanMonumentPiece("minecraft:omdyzr", 1, orientation, definition, 1, 2, 2) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            BS block;
            int y;
            RoomDefinition* north= roomDefinition->connections[Direction::NORTH.get3DDataValue()];
            RoomDefinition* south= roomDefinition;
            RoomDefinition* northUp= north->connections[Direction::UP.get3DDataValue()];
            RoomDefinition* southUp= south->connections[Direction::UP.get3DDataValue()];
            if (roomDefinition->index / 25 > 0) {
                generateDefaultFloor(level, chunkBB, 0, 8, north->hasOpening[Direction::DOWN.get3DDataValue()]);
                generateDefaultFloor(level, chunkBB, 0, 0, south->hasOpening[Direction::DOWN.get3DDataValue()]);
            }
            if (southUp->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 6, 8, 7, BASE_GRAY);
            }
            if (northUp->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 8, 8, 6, 8, 14, BASE_GRAY);
            }
            for (y = 1; y <= 7; ++y) {
                block = BASE_LIGHT;
                if (y == 2 || y == 6) {
                    block = BASE_GRAY;
                }
                generateBox(level, chunkBB, 0, y, 0, 0, y, 15, block, block, false);
                generateBox(level, chunkBB, 7, y, 0, 7, y, 15, block, block, false);
                generateBox(level, chunkBB, 1, y, 0, 6, y, 0, block, block, false);
                generateBox(level, chunkBB, 1, y, 15, 6, y, 15, block, block, false);
            }
            for (y = 1; y <= 7; ++y) {
                block = BASE_BLACK;
                if (y == 2 || y == 6) {
                    block = LAMP_BLOCK;
                }
                generateBox(level, chunkBB, 3, y, 7, 4, y, 8, block, block, false);
            }
            if (south->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
            }
            if (south->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 7, 1, 3, 7, 2, 4);
            }
            if (south->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
            }
            if (north->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 15, 4, 2, 15);
            }
            if (north->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 1, 11, 0, 2, 12);
            }
            if (north->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 7, 1, 11, 7, 2, 12);
            }
            if (southUp->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 5, 0, 4, 6, 0);
            }
            if (southUp->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 7, 5, 3, 7, 6, 4);
                generateBox(level, chunkBB, 5, 4, 2, 6, 4, 5, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 6, 1, 2, 6, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 6, 1, 5, 6, 3, 5, BASE_LIGHT, BASE_LIGHT, false);
            }
            if (southUp->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 5, 3, 0, 6, 4);
                generateBox(level, chunkBB, 1, 4, 2, 2, 4, 5, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 1, 1, 2, 1, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 1, 1, 5, 1, 3, 5, BASE_LIGHT, BASE_LIGHT, false);
            }
            if (northUp->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 5, 15, 4, 6, 15);
            }
            if (northUp->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 5, 11, 0, 6, 12);
                generateBox(level, chunkBB, 1, 4, 10, 2, 4, 13, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 1, 1, 10, 1, 3, 10, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 1, 1, 13, 1, 3, 13, BASE_LIGHT, BASE_LIGHT, false);
            }
            if (northUp->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 7, 5, 11, 7, 6, 12);
                generateBox(level, chunkBB, 5, 4, 10, 6, 4, 13, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 6, 1, 10, 6, 3, 10, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 6, 1, 13, 6, 3, 13, BASE_LIGHT, BASE_LIGHT, false);
            }
    }
};

class OceanMonumentDoubleXYRoom : public OceanMonumentPiece {
public:
    OceanMonumentDoubleXYRoom(const Direction& orientation, RoomDefinition* definition) : OceanMonumentPiece("minecraft:omdxyr", 1, orientation, definition, 2, 2, 1) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            RoomDefinition* east= roomDefinition->connections[Direction::EAST.get3DDataValue()];
            RoomDefinition* west= roomDefinition;
            RoomDefinition* westUp= west->connections[Direction::UP.get3DDataValue()];
            RoomDefinition* eastUp= east->connections[Direction::UP.get3DDataValue()];
            if (roomDefinition->index / 25 > 0) {
                generateDefaultFloor(level, chunkBB, 8, 0, east->hasOpening[Direction::DOWN.get3DDataValue()]);
                generateDefaultFloor(level, chunkBB, 0, 0, west->hasOpening[Direction::DOWN.get3DDataValue()]);
            }
            if (westUp->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 7, 8, 6, BASE_GRAY);
            }
            if (eastUp->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 8, 8, 1, 14, 8, 6, BASE_GRAY);
            }
            for (int y = 1; y <= 7; ++y) {
                BS block = BASE_LIGHT;
                if (y == 2 || y == 6) {
                    block = BASE_GRAY;
                }
                generateBox(level, chunkBB, 0, y, 0, 0, y, 7, block, block, false);
                generateBox(level, chunkBB, 15, y, 0, 15, y, 7, block, block, false);
                generateBox(level, chunkBB, 1, y, 0, 15, y, 0, block, block, false);
                generateBox(level, chunkBB, 1, y, 7, 14, y, 7, block, block, false);
            }
            generateBox(level, chunkBB, 2, 1, 3, 2, 7, 4, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 3, 1, 2, 4, 7, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 3, 1, 5, 4, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 13, 1, 3, 13, 7, 4, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 11, 1, 2, 12, 7, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 11, 1, 5, 12, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 1, 3, 5, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 1, 3, 10, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 7, 2, 10, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 5, 2, 5, 7, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 5, 2, 10, 7, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 5, 5, 5, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 10, 5, 5, 10, 7, 5, BASE_LIGHT, BASE_LIGHT, false);
            placeBlock(level, BASE_LIGHT, 6, 6, 2, chunkBB);
            placeBlock(level, BASE_LIGHT, 9, 6, 2, chunkBB);
            placeBlock(level, BASE_LIGHT, 6, 6, 5, chunkBB);
            placeBlock(level, BASE_LIGHT, 9, 6, 5, chunkBB);
            generateBox(level, chunkBB, 5, 4, 3, 6, 4, 4, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 9, 4, 3, 10, 4, 4, BASE_LIGHT, BASE_LIGHT, false);
            placeBlock(level, LAMP_BLOCK, 5, 4, 2, chunkBB);
            placeBlock(level, LAMP_BLOCK, 5, 4, 5, chunkBB);
            placeBlock(level, LAMP_BLOCK, 10, 4, 2, chunkBB);
            placeBlock(level, LAMP_BLOCK, 10, 4, 5, chunkBB);
            if (west->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
            }
            if (west->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 7, 4, 2, 7);
            }
            if (west->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
            }
            if (east->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 11, 1, 0, 12, 2, 0);
            }
            if (east->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 11, 1, 7, 12, 2, 7);
            }
            if (east->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 15, 1, 3, 15, 2, 4);
            }
            if (westUp->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 5, 0, 4, 6, 0);
            }
            if (westUp->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 5, 7, 4, 6, 7);
            }
            if (westUp->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 5, 3, 0, 6, 4);
            }
            if (eastUp->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 11, 5, 0, 12, 6, 0);
            }
            if (eastUp->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 11, 5, 7, 12, 6, 7);
            }
            if (eastUp->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 15, 5, 3, 15, 6, 4);
            }
    }
};

class OceanMonumentDoubleZRoom : public OceanMonumentPiece {
public:
    OceanMonumentDoubleZRoom(const Direction& orientation, RoomDefinition* definition) : OceanMonumentPiece("minecraft:omdzr", 1, orientation, definition, 1, 1, 2) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            RoomDefinition* north= roomDefinition->connections[Direction::NORTH.get3DDataValue()];
            RoomDefinition* south= roomDefinition;
            if (roomDefinition->index / 25 > 0) {
                generateDefaultFloor(level, chunkBB, 0, 8, north->hasOpening[Direction::DOWN.get3DDataValue()]);
                generateDefaultFloor(level, chunkBB, 0, 0, south->hasOpening[Direction::DOWN.get3DDataValue()]);
            }
            if (south->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 7, BASE_GRAY);
            }
            if (north->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 4, 8, 6, 4, 14, BASE_GRAY);
            }
            generateBox(level, chunkBB, 0, 3, 0, 0, 3, 15, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 7, 3, 0, 7, 3, 15, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 3, 0, 7, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 3, 15, 6, 3, 15, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 0, 2, 0, 0, 2, 15, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 7, 2, 0, 7, 2, 15, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 1, 2, 0, 7, 2, 0, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 1, 2, 15, 6, 2, 15, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 0, 1, 0, 0, 1, 15, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 7, 1, 0, 7, 1, 15, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 0, 7, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 15, 6, 1, 15, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 1, 1, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 1, 1, 6, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 3, 1, 1, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 3, 1, 6, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 13, 1, 1, 14, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 1, 13, 6, 1, 14, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 3, 13, 1, 3, 14, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 3, 13, 6, 3, 14, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 2, 1, 6, 2, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 1, 6, 5, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 2, 1, 9, 2, 3, 9, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 1, 9, 5, 3, 9, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 3, 2, 6, 4, 2, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 3, 2, 9, 4, 2, 9, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 2, 2, 7, 2, 2, 8, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 2, 7, 5, 2, 8, BASE_LIGHT, BASE_LIGHT, false);
            placeBlock(level, LAMP_BLOCK, 2, 2, 5, chunkBB);
            placeBlock(level, LAMP_BLOCK, 5, 2, 5, chunkBB);
            placeBlock(level, LAMP_BLOCK, 2, 2, 10, chunkBB);
            placeBlock(level, LAMP_BLOCK, 5, 2, 10, chunkBB);
            placeBlock(level, BASE_LIGHT, 2, 3, 5, chunkBB);
            placeBlock(level, BASE_LIGHT, 5, 3, 5, chunkBB);
            placeBlock(level, BASE_LIGHT, 2, 3, 10, chunkBB);
            placeBlock(level, BASE_LIGHT, 5, 3, 10, chunkBB);
            if (south->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
            }
            if (south->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 7, 1, 3, 7, 2, 4);
            }
            if (south->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
            }
            if (north->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 15, 4, 2, 15);
            }
            if (north->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 1, 11, 0, 2, 12);
            }
            if (north->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 7, 1, 11, 7, 2, 12);
            }
    }
};

class OceanMonumentDoubleXRoom : public OceanMonumentPiece {
public:
    OceanMonumentDoubleXRoom(const Direction& orientation, RoomDefinition* definition) : OceanMonumentPiece("minecraft:omdxr", 1, orientation, definition, 2, 1, 1) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            RoomDefinition* east= roomDefinition->connections[Direction::EAST.get3DDataValue()];
            RoomDefinition* west= roomDefinition;
            if (roomDefinition->index / 25 > 0) {
                generateDefaultFloor(level, chunkBB, 8, 0, east->hasOpening[Direction::DOWN.get3DDataValue()]);
                generateDefaultFloor(level, chunkBB, 0, 0, west->hasOpening[Direction::DOWN.get3DDataValue()]);
            }
            if (west->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 7, 4, 6, BASE_GRAY);
            }
            if (east->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 8, 4, 1, 14, 4, 6, BASE_GRAY);
            }
            generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 15, 3, 0, 15, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 3, 0, 15, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 3, 7, 14, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 15, 2, 0, 15, 2, 7, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 1, 2, 0, 15, 2, 0, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 1, 2, 7, 14, 2, 7, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 15, 1, 0, 15, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 0, 15, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 7, 14, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 1, 0, 10, 1, 4, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 2, 0, 9, 2, 3, BASE_GRAY, BASE_GRAY, false);
            generateBox(level, chunkBB, 5, 3, 0, 10, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
            placeBlock(level, LAMP_BLOCK, 6, 2, 3, chunkBB);
            placeBlock(level, LAMP_BLOCK, 9, 2, 3, chunkBB);
            if (west->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
            }
            if (west->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 7, 4, 2, 7);
            }
            if (west->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
            }
            if (east->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 11, 1, 0, 12, 2, 0);
            }
            if (east->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 11, 1, 7, 12, 2, 7);
            }
            if (east->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 15, 1, 3, 15, 2, 4);
            }
    }
};

class OceanMonumentDoubleYRoom : public OceanMonumentPiece {
public:
    OceanMonumentDoubleYRoom(const Direction& orientation, RoomDefinition* definition) : OceanMonumentPiece("minecraft:omdyr", 1, orientation, definition, 1, 2, 1) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            if (roomDefinition->index / 25 > 0) {
                generateDefaultFloor(level, chunkBB, 0, 0, roomDefinition->hasOpening[Direction::DOWN.get3DDataValue()]);
            }
            RoomDefinition* above= roomDefinition->connections[Direction::UP.get3DDataValue()];
            if (above->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 6, 8, 6, BASE_GRAY);
            }
            generateBox(level, chunkBB, 0, 4, 0, 0, 4, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 7, 4, 0, 7, 4, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 4, 0, 6, 4, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 4, 7, 6, 4, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 2, 4, 1, 2, 4, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 4, 2, 1, 4, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 4, 1, 5, 4, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 4, 2, 6, 4, 2, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 2, 4, 5, 2, 4, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 4, 5, 1, 4, 5, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 4, 5, 5, 4, 6, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 4, 5, 6, 4, 5, BASE_LIGHT, BASE_LIGHT, false);
            RoomDefinition* definition= roomDefinition;
            for (int y = 1; y <= 5; y += 4) {
                int z = 0;
                if (definition->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                    generateBox(level, chunkBB, 2, y, z, 2, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 5, y, z, 5, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 3, y + 2, z, 4, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
                } else {
                    generateBox(level, chunkBB, 0, y, z, 7, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 0, y + 1, z, 7, y + 1, z, BASE_GRAY, BASE_GRAY, false);
                }
                z = 7;
                if (definition->hasOpening[Direction::NORTH.get3DDataValue()]) {
                    generateBox(level, chunkBB, 2, y, z, 2, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 5, y, z, 5, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 3, y + 2, z, 4, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
                } else {
                    generateBox(level, chunkBB, 0, y, z, 7, y + 2, z, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 0, y + 1, z, 7, y + 1, z, BASE_GRAY, BASE_GRAY, false);
                }
                int x = 0;
                if (definition->hasOpening[Direction::WEST.get3DDataValue()]) {
                    generateBox(level, chunkBB, x, y, 2, x, y + 2, 2, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, x, y, 5, x, y + 2, 5, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, x, y + 2, 3, x, y + 2, 4, BASE_LIGHT, BASE_LIGHT, false);
                } else {
                    generateBox(level, chunkBB, x, y, 0, x, y + 2, 7, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, x, y + 1, 0, x, y + 1, 7, BASE_GRAY, BASE_GRAY, false);
                }
                x = 7;
                if (definition->hasOpening[Direction::EAST.get3DDataValue()]) {
                    generateBox(level, chunkBB, x, y, 2, x, y + 2, 2, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, x, y, 5, x, y + 2, 5, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, x, y + 2, 3, x, y + 2, 4, BASE_LIGHT, BASE_LIGHT, false);
                } else {
                    generateBox(level, chunkBB, x, y, 0, x, y + 2, 7, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, x, y + 1, 0, x, y + 1, 7, BASE_GRAY, BASE_GRAY, false);
                }
                definition = above;
            }
    }
};

class OceanMonumentSimpleTopRoom : public OceanMonumentPiece {
public:
    OceanMonumentSimpleTopRoom(const Direction& orientation, RoomDefinition* definition) : OceanMonumentPiece("minecraft:omsimplet", 1, orientation, definition, 1, 1, 1) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            if (roomDefinition->index / 25 > 0) {
                generateDefaultFloor(level, chunkBB, 0, 0, roomDefinition->hasOpening[Direction::DOWN.get3DDataValue()]);
            }
            if (roomDefinition->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 6, BASE_GRAY);
            }
            for (int x = 1; x <= 6; ++x) {
                for (int z = 1; z <= 6; ++z) {
                    if (random.nextInt(3) == 0) continue;
                    int y0 = 2 + (random.nextInt(4) == 0 ? 0 : 1);
                    BS wetSponge = blk("minecraft:wet_sponge");
                    generateBox(level, chunkBB, x, y0, z, x, 3, z, wetSponge, wetSponge, false);
                }
            }
            generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 7, 2, 0, 7, 2, 7, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 7, 3, 0, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 0, 1, 3, 0, 2, 4, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 7, 1, 3, 7, 2, 4, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 3, 1, 0, 4, 2, 0, BASE_BLACK, BASE_BLACK, false);
            generateBox(level, chunkBB, 3, 1, 7, 4, 2, 7, BASE_BLACK, BASE_BLACK, false);
            if (roomDefinition->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
            }
    }
};

class OceanMonumentSimpleRoom : public OceanMonumentPiece {
public:
    int mainDesign;
    OceanMonumentSimpleRoom(const Direction& orientation, RoomDefinition* definition, JRandom random) : OceanMonumentPiece("minecraft:omsimple", 1, orientation, definition, 1, 1, 1) {mainDesign = random.nextInt(3);}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            bool centerPillar;
            if (roomDefinition->index / 25 > 0) {
                generateDefaultFloor(level, chunkBB, 0, 0, roomDefinition->hasOpening[Direction::DOWN.get3DDataValue()]);
            }
            if (roomDefinition->connections[Direction::UP.get3DDataValue()] == nullptr) {
                generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 6, BASE_GRAY);
            }
            bool bl = centerPillar = mainDesign != 0 && random.nextBoolean() && !roomDefinition->hasOpening[Direction::DOWN.get3DDataValue()] && !roomDefinition->hasOpening[Direction::UP.get3DDataValue()] && roomDefinition->countOpenings() > 1;
            if (mainDesign == 0) {
                generateBox(level, chunkBB, 0, 1, 0, 2, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 3, 0, 2, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 2, 0, 0, 2, 2, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 1, 2, 0, 2, 2, 0, BASE_GRAY, BASE_GRAY, false);
                placeBlock(level, LAMP_BLOCK, 1, 2, 1, chunkBB);
                generateBox(level, chunkBB, 5, 1, 0, 7, 1, 2, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 5, 3, 0, 7, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 7, 2, 0, 7, 2, 2, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 5, 2, 0, 6, 2, 0, BASE_GRAY, BASE_GRAY, false);
                placeBlock(level, LAMP_BLOCK, 6, 2, 1, chunkBB);
                generateBox(level, chunkBB, 0, 1, 5, 2, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 3, 5, 2, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 2, 5, 0, 2, 7, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 1, 2, 7, 2, 2, 7, BASE_GRAY, BASE_GRAY, false);
                placeBlock(level, LAMP_BLOCK, 1, 2, 6, chunkBB);
                generateBox(level, chunkBB, 5, 1, 5, 7, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 5, 3, 5, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 7, 2, 5, 7, 2, 7, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 5, 2, 7, 6, 2, 7, BASE_GRAY, BASE_GRAY, false);
                placeBlock(level, LAMP_BLOCK, 6, 2, 6, chunkBB);
                if (roomDefinition->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                    generateBox(level, chunkBB, 3, 3, 0, 4, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
                } else {
                    generateBox(level, chunkBB, 3, 3, 0, 4, 3, 1, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 3, 2, 0, 4, 2, 0, BASE_GRAY, BASE_GRAY, false);
                    generateBox(level, chunkBB, 3, 1, 0, 4, 1, 1, BASE_LIGHT, BASE_LIGHT, false);
                }
                if (roomDefinition->hasOpening[Direction::NORTH.get3DDataValue()]) {
                    generateBox(level, chunkBB, 3, 3, 7, 4, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                } else {
                    generateBox(level, chunkBB, 3, 3, 6, 4, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 3, 2, 7, 4, 2, 7, BASE_GRAY, BASE_GRAY, false);
                    generateBox(level, chunkBB, 3, 1, 6, 4, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                }
                if (roomDefinition->hasOpening[Direction::WEST.get3DDataValue()]) {
                    generateBox(level, chunkBB, 0, 3, 3, 0, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
                } else {
                    generateBox(level, chunkBB, 0, 3, 3, 1, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 0, 2, 3, 0, 2, 4, BASE_GRAY, BASE_GRAY, false);
                    generateBox(level, chunkBB, 0, 1, 3, 1, 1, 4, BASE_LIGHT, BASE_LIGHT, false);
                }
                if (roomDefinition->hasOpening[Direction::EAST.get3DDataValue()]) {
                    generateBox(level, chunkBB, 7, 3, 3, 7, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
                } else {
                    generateBox(level, chunkBB, 6, 3, 3, 7, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 7, 2, 3, 7, 2, 4, BASE_GRAY, BASE_GRAY, false);
                    generateBox(level, chunkBB, 6, 1, 3, 7, 1, 4, BASE_LIGHT, BASE_LIGHT, false);
                }
            } else if (mainDesign == 1) {
                generateBox(level, chunkBB, 2, 1, 2, 2, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 2, 1, 5, 2, 3, 5, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 5, 1, 5, 5, 3, 5, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 5, 1, 2, 5, 3, 2, BASE_LIGHT, BASE_LIGHT, false);
                placeBlock(level, LAMP_BLOCK, 2, 2, 2, chunkBB);
                placeBlock(level, LAMP_BLOCK, 2, 2, 5, chunkBB);
                placeBlock(level, LAMP_BLOCK, 5, 2, 5, chunkBB);
                placeBlock(level, LAMP_BLOCK, 5, 2, 2, chunkBB);
                generateBox(level, chunkBB, 0, 1, 0, 1, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 1, 1, 0, 3, 1, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 1, 7, 1, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 1, 6, 0, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 6, 1, 7, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 7, 1, 6, 7, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 6, 1, 0, 7, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 7, 1, 1, 7, 3, 1, BASE_LIGHT, BASE_LIGHT, false);
                placeBlock(level, BASE_GRAY, 1, 2, 0, chunkBB);
                placeBlock(level, BASE_GRAY, 0, 2, 1, chunkBB);
                placeBlock(level, BASE_GRAY, 1, 2, 7, chunkBB);
                placeBlock(level, BASE_GRAY, 0, 2, 6, chunkBB);
                placeBlock(level, BASE_GRAY, 6, 2, 7, chunkBB);
                placeBlock(level, BASE_GRAY, 7, 2, 6, chunkBB);
                placeBlock(level, BASE_GRAY, 6, 2, 0, chunkBB);
                placeBlock(level, BASE_GRAY, 7, 2, 1, chunkBB);
                if (!roomDefinition->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                    generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, BASE_GRAY, BASE_GRAY, false);
                    generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
                }
                if (!roomDefinition->hasOpening[Direction::NORTH.get3DDataValue()]) {
                    generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, BASE_GRAY, BASE_GRAY, false);
                    generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                }
                if (!roomDefinition->hasOpening[Direction::WEST.get3DDataValue()]) {
                    generateBox(level, chunkBB, 0, 3, 1, 0, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 0, 2, 1, 0, 2, 6, BASE_GRAY, BASE_GRAY, false);
                    generateBox(level, chunkBB, 0, 1, 1, 0, 1, 6, BASE_LIGHT, BASE_LIGHT, false);
                }
                if (!roomDefinition->hasOpening[Direction::EAST.get3DDataValue()]) {
                    generateBox(level, chunkBB, 7, 3, 1, 7, 3, 6, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 7, 2, 1, 7, 2, 6, BASE_GRAY, BASE_GRAY, false);
                    generateBox(level, chunkBB, 7, 1, 1, 7, 1, 6, BASE_LIGHT, BASE_LIGHT, false);
                }
            } else if (mainDesign == 2) {
                generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 7, 2, 0, 7, 2, 7, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 7, 3, 0, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 0, 1, 3, 0, 2, 4, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 7, 1, 3, 7, 2, 4, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 3, 1, 0, 4, 2, 0, BASE_BLACK, BASE_BLACK, false);
                generateBox(level, chunkBB, 3, 1, 7, 4, 2, 7, BASE_BLACK, BASE_BLACK, false);
                if (roomDefinition->hasOpening[Direction::SOUTH.get3DDataValue()]) {
                    generateWaterBox(level, chunkBB, 3, 1, 0, 4, 2, 0);
                }
                if (roomDefinition->hasOpening[Direction::NORTH.get3DDataValue()]) {
                    generateWaterBox(level, chunkBB, 3, 1, 7, 4, 2, 7);
                }
                if (roomDefinition->hasOpening[Direction::WEST.get3DDataValue()]) {
                    generateWaterBox(level, chunkBB, 0, 1, 3, 0, 2, 4);
                }
                if (roomDefinition->hasOpening[Direction::EAST.get3DDataValue()]) {
                    generateWaterBox(level, chunkBB, 7, 1, 3, 7, 2, 4);
                }
            }
            if (centerPillar) {
                generateBox(level, chunkBB, 3, 1, 3, 4, 1, 4, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 3, 2, 3, 4, 2, 4, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 3, 3, 3, 4, 3, 4, BASE_LIGHT, BASE_LIGHT, false);
            }
    }
};

class OceanMonumentEntryRoom : public OceanMonumentPiece {
public:
    OceanMonumentEntryRoom(const Direction& orientation, RoomDefinition* definition) : OceanMonumentPiece("minecraft:omentry", 1, orientation, definition, 1, 1, 1) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 0, 3, 0, 2, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 3, 0, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 0, 2, 0, 1, 2, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 6, 2, 0, 7, 2, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 0, 1, 7, 7, 3, 7, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 1, 1, 0, 2, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
            generateBox(level, chunkBB, 5, 1, 0, 6, 3, 0, BASE_LIGHT, BASE_LIGHT, false);
            if (roomDefinition->hasOpening[Direction::NORTH.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 3, 1, 7, 4, 2, 7);
            }
            if (roomDefinition->hasOpening[Direction::WEST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 0, 1, 3, 1, 2, 4);
            }
            if (roomDefinition->hasOpening[Direction::EAST.get3DDataValue()]) {
                generateWaterBox(level, chunkBB, 6, 1, 3, 7, 2, 4);
            }
    }
};



// ---- room fitters

struct MonumentRoomFitter {
    virtual ~MonumentRoomFitter() = default;
    virtual bool fits(RoomDefinition* definition) = 0;
    virtual OceanMonumentPiece* create(const Direction& orientation, RoomDefinition* definition, JRandom random) = 0;
};

#define N3 Direction::NORTH.get3DDataValue()
#define S3 Direction::SOUTH.get3DDataValue()
#define E3 Direction::EAST.get3DDataValue()
#define W3 Direction::WEST.get3DDataValue()
#define U3 Direction::UP.get3DDataValue()

struct FitDoubleYZRoom : MonumentRoomFitter {
    bool fits(RoomDefinition* d) override {
        if (d->hasOpening[N3] && !d->connections[N3]->claimed && d->hasOpening[U3] && !d->connections[U3]->claimed) {
            RoomDefinition* north = d->connections[N3];
            return north->hasOpening[U3] && !north->connections[U3]->claimed;
        }
        return false;
    }
    OceanMonumentPiece* create(const Direction& o, RoomDefinition* d, JRandom) override {
        d->claimed = true; d->connections[N3]->claimed = true; d->connections[U3]->claimed = true; d->connections[N3]->connections[U3]->claimed = true;
        return new OceanMonumentDoubleYZRoom(o, d);
    }
};
struct FitDoubleXYRoom : MonumentRoomFitter {
    bool fits(RoomDefinition* d) override {
        if (d->hasOpening[E3] && !d->connections[E3]->claimed && d->hasOpening[U3] && !d->connections[U3]->claimed) {
            RoomDefinition* east = d->connections[E3];
            return east->hasOpening[U3] && !east->connections[U3]->claimed;
        }
        return false;
    }
    OceanMonumentPiece* create(const Direction& o, RoomDefinition* d, JRandom) override {
        d->claimed = true; d->connections[E3]->claimed = true; d->connections[U3]->claimed = true; d->connections[E3]->connections[U3]->claimed = true;
        return new OceanMonumentDoubleXYRoom(o, d);
    }
};
struct FitDoubleZRoom : MonumentRoomFitter {
    bool fits(RoomDefinition* d) override { return d->hasOpening[N3] && !d->connections[N3]->claimed; }
    OceanMonumentPiece* create(const Direction& o, RoomDefinition* d, JRandom) override {
        RoomDefinition* source = d;
        if (!d->hasOpening[N3] || d->connections[N3]->claimed) source = d->connections[S3];
        source->claimed = true;
        source->connections[N3]->claimed = true;
        return new OceanMonumentDoubleZRoom(o, source);
    }
};
struct FitDoubleXRoom : MonumentRoomFitter {
    bool fits(RoomDefinition* d) override { return d->hasOpening[E3] && !d->connections[E3]->claimed; }
    OceanMonumentPiece* create(const Direction& o, RoomDefinition* d, JRandom) override {
        d->claimed = true; d->connections[E3]->claimed = true;
        return new OceanMonumentDoubleXRoom(o, d);
    }
};
struct FitDoubleYRoom : MonumentRoomFitter {
    bool fits(RoomDefinition* d) override { return d->hasOpening[U3] && !d->connections[U3]->claimed; }
    OceanMonumentPiece* create(const Direction& o, RoomDefinition* d, JRandom) override {
        d->claimed = true; d->connections[U3]->claimed = true;
        return new OceanMonumentDoubleYRoom(o, d);
    }
};
struct FitSimpleTopRoom : MonumentRoomFitter {
    bool fits(RoomDefinition* d) override { return !d->hasOpening[W3] && !d->hasOpening[E3] && !d->hasOpening[N3] && !d->hasOpening[S3] && !d->hasOpening[U3]; }
    OceanMonumentPiece* create(const Direction& o, RoomDefinition* d, JRandom) override { d->claimed = true; return new OceanMonumentSimpleTopRoom(o, d); }
};
struct FitSimpleRoom : MonumentRoomFitter {
    bool fits(RoomDefinition*) override { return true; }
    OceanMonumentPiece* create(const Direction& o, RoomDefinition* d, JRandom random) override { d->claimed = true; return new OceanMonumentSimpleRoom(o, d, random); }
};

class MonumentBuilding : public OceanMonumentPiece {
public:
    RoomDefinition* sourceRoom = nullptr;
    RoomDefinition* coreRoom = nullptr;
    std::vector<std::shared_ptr<OceanMonumentPiece>> childPieces;
    std::vector<std::unique_ptr<RoomDefinition>> storage;   // owns the room definitions

    MonumentBuilding(JRandom random, int west, int north, const Direction& direction) : OceanMonumentPiece("minecraft:omb", direction, 0, makeBoundingBox(west, 39, north, direction, 58, 23, 58)) {
        setOrientation(direction);
        std::vector<RoomDefinition*> roomDefinitions = generateRoomGraph(random);
        sourceRoom->claimed = true;
        childPieces.push_back(std::shared_ptr<OceanMonumentPiece>(new OceanMonumentEntryRoom(direction, sourceRoom)));
        childPieces.push_back(std::shared_ptr<OceanMonumentPiece>(new OceanMonumentCoreRoom(direction, coreRoom)));
        std::vector<std::unique_ptr<MonumentRoomFitter>> fitters;
        fitters.push_back(std::make_unique<FitDoubleXYRoom>());
        fitters.push_back(std::make_unique<FitDoubleYZRoom>());
        fitters.push_back(std::make_unique<FitDoubleZRoom>());
        fitters.push_back(std::make_unique<FitDoubleXRoom>());
        fitters.push_back(std::make_unique<FitDoubleYRoom>());
        fitters.push_back(std::make_unique<FitSimpleTopRoom>());
        fitters.push_back(std::make_unique<FitSimpleRoom>());
        for (RoomDefinition* roomDefinition : roomDefinitions) {
            if (roomDefinition->claimed || roomDefinition->isSpecial()) continue;
            for (auto& fitter : fitters) {
                if (!fitter->fits(roomDefinition)) continue;
                childPieces.push_back(std::shared_ptr<OceanMonumentPiece>(fitter->create(direction, roomDefinition, random)));
                break;
            }
        }
        BlockPos offset = getWorldPos(9, 0, 22);
        for (auto& child : childPieces) child->bb.move(offset.x, offset.y, offset.z);
        BlockPos a = getWorldPos(1, 1, 1), b = getWorldPos(23, 8, 21);
        BoundingBox boundingBox(a.x, a.y, a.z, b.x, b.y, b.z);
        a = getWorldPos(34, 1, 1); b = getWorldPos(56, 8, 21);
        BoundingBox rightWing(a.x, a.y, a.z, b.x, b.y, b.z);
        a = getWorldPos(22, 13, 22); b = getWorldPos(35, 17, 35);
        BoundingBox penthouse(a.x, a.y, a.z, b.x, b.y, b.z);
        int wingRandom = random.nextInt();
        childPieces.push_back(std::shared_ptr<OceanMonumentPiece>(new OceanMonumentWingRoom(direction, boundingBox, wingRandom++)));
        childPieces.push_back(std::shared_ptr<OceanMonumentPiece>(new OceanMonumentWingRoom(direction, rightWing, wingRandom++)));
        childPieces.push_back(std::shared_ptr<OceanMonumentPiece>(new OceanMonumentPenthouse(direction, penthouse)));
    }

    std::vector<RoomDefinition*> generateRoomGraph(JRandom random) {
        RoomDefinition* roomGrid[75] = {};
        auto make = [&](int pos) { storage.push_back(std::make_unique<RoomDefinition>(pos)); roomGrid[pos] = storage.back().get(); };
        for (int x = 0; x < 5; ++x) for (int z = 0; z < 4; ++z) make(getRoomIndex(x, 0, z));
        for (int x = 0; x < 5; ++x) for (int z = 0; z < 4; ++z) make(getRoomIndex(x, 1, z));
        for (int x = 1; x < 4; ++x) for (int z = 0; z < 2; ++z) make(getRoomIndex(x, 2, z));
        sourceRoom = roomGrid[getRoomIndex(2, 0, 0)];
        for (int x = 0; x < 5; ++x)
            for (int z = 0; z < 5; ++z)
                for (int y = 0; y < 3; ++y) {
                    int pos = getRoomIndex(x, y, z);
                    if (!roomGrid[pos]) continue;
                    for (int d = 0; d < 6; ++d) {
                        Direction direction(d);
                        int neighX = x + direction.getStepX(), neighY = y + direction.getStepY(), neighZ = z + direction.getStepZ();
                        if (neighX < 0 || neighX >= 5 || neighZ < 0 || neighZ >= 5 || neighY < 0 || neighY >= 3) continue;
                        int neighPos = getRoomIndex(neighX, neighY, neighZ);
                        if (!roomGrid[neighPos]) continue;
                        if (neighZ == z) roomGrid[pos]->setConnection(direction, roomGrid[neighPos]);
                        else roomGrid[pos]->setConnection(direction.getOpposite(), roomGrid[neighPos]);
                    }
                }
        storage.push_back(std::make_unique<RoomDefinition>(1003)); RoomDefinition* roofRoom = storage.back().get();
        storage.push_back(std::make_unique<RoomDefinition>(1001)); RoomDefinition* leftWing = storage.back().get();
        storage.push_back(std::make_unique<RoomDefinition>(1002)); RoomDefinition* rightWing = storage.back().get();
        roomGrid[getRoomIndex(2, 2, 0)]->setConnection(Direction::UP, roofRoom);
        roomGrid[getRoomIndex(0, 1, 0)]->setConnection(Direction::SOUTH, leftWing);
        roomGrid[getRoomIndex(4, 1, 0)]->setConnection(Direction::SOUTH, rightWing);
        roofRoom->claimed = true;
        leftWing->claimed = true;
        rightWing->claimed = true;
        sourceRoom->isSource = true;
        coreRoom = roomGrid[getRoomIndex(random.nextInt(4), 0, 2)];
        coreRoom->claimed = true;
        coreRoom->connections[E3]->claimed = true;
        coreRoom->connections[N3]->claimed = true;
        coreRoom->connections[E3]->connections[N3]->claimed = true;
        coreRoom->connections[U3]->claimed = true;
        coreRoom->connections[E3]->connections[U3]->claimed = true;
        coreRoom->connections[N3]->connections[U3]->claimed = true;
        coreRoom->connections[E3]->connections[N3]->connections[U3]->claimed = true;
        std::vector<RoomDefinition*> roomDefs;
        for (RoomDefinition* definition : roomGrid) {
            if (!definition) continue;
            definition->updateOpenings();
            roomDefs.push_back(definition);
        }
        roofRoom->updateOpenings();
        for (int i = int(roomDefs.size()); i > 1; --i) std::swap(roomDefs[size_t(i - 1)], roomDefs[size_t(random.nextInt(i))]);
        int scanIndex = 1;
        for (RoomDefinition* definition : roomDefs) {
            int closeCount = 0;
            for (int attemptCount = 0; closeCount < 2 && attemptCount < 5; ++attemptCount) {
                int f = random.nextInt(6);
                if (!definition->hasOpening[f]) continue;
                int of = Direction::from3DDataValue(f).getOpposite().get3DDataValue();
                definition->hasOpening[f] = false;
                definition->connections[f]->hasOpening[of] = false;
                if (definition->findSource(scanIndex++) && definition->connections[f]->findSource(scanIndex++)) { ++closeCount; continue; }
                definition->hasOpening[f] = true;
                definition->connections[f]->hasOpening[of] = true;
            }
        }
        roomDefs.push_back(roofRoom);
        roomDefs.push_back(leftWing);
        roomDefs.push_back(rightWing);
        return roomDefs;
    }

    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {

            int waterHeight = std::max(level.getSeaLevel(), 64) - bb.y0;
            generateWaterBox(level, chunkBB, 0, 0, 0, 58, waterHeight, 58);
            generateWing(false, 0, level, random, chunkBB);
            generateWing(true, 33, level, random, chunkBB);
            generateEntranceArchs(level, random, chunkBB);
            generateEntranceWall(level, random, chunkBB);
            generateRoofPiece(level, random, chunkBB);
            generateLowerWall(level, random, chunkBB);
            generateMiddleWall(level, random, chunkBB);
            generateUpperWall(level, random, chunkBB);
            for (int pillarX = 0; pillarX < 7; ++pillarX) {
                int pillarZ = 0;
                while (pillarZ < 7) {
                    if (pillarZ == 0 && pillarX == 3) {
                        pillarZ = 6;
                    }
                    int bx = pillarX * 9;
                    int bz = pillarZ * 9;
                    for (int w = 0; w < 4; ++w) {
                        for (int d = 0; d < 4; ++d) {
                            placeBlock(level, BASE_LIGHT, bx + w, 0, bz + d, chunkBB);
                            fillColumnDown(level, BASE_LIGHT, bx + w, -1, bz + d, chunkBB);
                        }
                    }
                    if (pillarX == 0 || pillarX == 6) {
                        ++pillarZ;
                        continue;
                    }
                    pillarZ += 6;
                }
            }
            for (int i = 0; i < 5; ++i) {
                generateWaterBox(level, chunkBB, -1 - i, 0 + i * 2, -1 - i, -1 - i, 23, 58 + i);
                generateWaterBox(level, chunkBB, 58 + i, 0 + i * 2, -1 - i, 58 + i, 23, 58 + i);
                generateWaterBox(level, chunkBB, 0 - i, 0 + i * 2, -1 - i, 57 + i, 23, -1 - i);
                generateWaterBox(level, chunkBB, 0 - i, 0 + i * 2, 58 + i, 57 + i, 23, 58 + i);
            }
            for (auto& childp : childPieces) { OceanMonumentPiece* child = childp.get();
                if (!child->bb.intersects(chunkBB)) continue;
                child->postProcess(level, random, chunkBB, chunkPos, referencePos);
            }
    }
private:
    void generateWing(bool isFlipped, int xoff, JLevel& level, JRandom random, const BoundingBox& chunkBB) {

            int sectionWidth = 24;
            if (chunkIntersects(chunkBB, xoff, 0, xoff + 23, 20)) {
                int z;
                generateBox(level, chunkBB, xoff + 0, 0, 0, xoff + 24, 0, 20, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, xoff + 0, 1, 0, xoff + 24, 10, 20);
                for (int i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, xoff + i, i + 1, i, xoff + i, i + 1, 20, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, xoff + i + 7, i + 5, i + 7, xoff + i + 7, i + 5, 20, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, xoff + 17 - i, i + 5, i + 7, xoff + 17 - i, i + 5, 20, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, xoff + 24 - i, i + 1, i, xoff + 24 - i, i + 1, 20, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, xoff + i + 1, i + 1, i, xoff + 23 - i, i + 1, i, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, xoff + i + 8, i + 5, i + 7, xoff + 16 - i, i + 5, i + 7, BASE_LIGHT, BASE_LIGHT, false);
                }
                generateBox(level, chunkBB, xoff + 4, 4, 4, xoff + 6, 4, 20, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, xoff + 7, 4, 4, xoff + 17, 4, 6, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, xoff + 18, 4, 4, xoff + 20, 4, 20, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, xoff + 11, 8, 11, xoff + 13, 8, 20, BASE_GRAY, BASE_GRAY, false);
                placeBlock(level, DOT_DECO_DATA, xoff + 12, 9, 12, chunkBB);
                placeBlock(level, DOT_DECO_DATA, xoff + 12, 9, 15, chunkBB);
                placeBlock(level, DOT_DECO_DATA, xoff + 12, 9, 18, chunkBB);
                int leftPos = xoff + (isFlipped ? 19 : 5);
                int rightPos = xoff + (isFlipped ? 5 : 19);
                for (z = 20; z >= 5; z -= 3) {
                    placeBlock(level, DOT_DECO_DATA, leftPos, 5, z, chunkBB);
                }
                for (z = 19; z >= 7; z -= 3) {
                    placeBlock(level, DOT_DECO_DATA, rightPos, 5, z, chunkBB);
                }
                for (int i = 0; i < 4; ++i) {
                    int pos = isFlipped ? xoff + 24 - (17 - i * 3) : xoff + 17 - i * 3;
                    placeBlock(level, DOT_DECO_DATA, pos, 5, 5, chunkBB);
                }
                placeBlock(level, DOT_DECO_DATA, rightPos, 5, 5, chunkBB);
                generateBox(level, chunkBB, xoff + 11, 1, 12, xoff + 13, 7, 12, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, xoff + 12, 1, 11, xoff + 12, 7, 13, BASE_GRAY, BASE_GRAY, false);
            }
    }
    void generateEntranceArchs(JLevel& level, JRandom random, const BoundingBox& chunkBB) {

            if (chunkIntersects(chunkBB, 22, 5, 35, 17)) {
                generateWaterBox(level, chunkBB, 25, 0, 0, 32, 8, 20);
                for (int i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, 24, 2, 5 + i * 4, 24, 4, 5 + i * 4, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 22, 4, 5 + i * 4, 23, 4, 5 + i * 4, BASE_LIGHT, BASE_LIGHT, false);
                    placeBlock(level, BASE_LIGHT, 25, 5, 5 + i * 4, chunkBB);
                    placeBlock(level, BASE_LIGHT, 26, 6, 5 + i * 4, chunkBB);
                    placeBlock(level, LAMP_BLOCK, 26, 5, 5 + i * 4, chunkBB);
                    generateBox(level, chunkBB, 33, 2, 5 + i * 4, 33, 4, 5 + i * 4, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 34, 4, 5 + i * 4, 35, 4, 5 + i * 4, BASE_LIGHT, BASE_LIGHT, false);
                    placeBlock(level, BASE_LIGHT, 32, 5, 5 + i * 4, chunkBB);
                    placeBlock(level, BASE_LIGHT, 31, 6, 5 + i * 4, chunkBB);
                    placeBlock(level, LAMP_BLOCK, 31, 5, 5 + i * 4, chunkBB);
                    generateBox(level, chunkBB, 27, 6, 5 + i * 4, 30, 6, 5 + i * 4, BASE_GRAY, BASE_GRAY, false);
                }
            }
    }
    void generateEntranceWall(JLevel& level, JRandom random, const BoundingBox& chunkBB) {

            if (chunkIntersects(chunkBB, 15, 20, 42, 21)) {
                int i;
                generateBox(level, chunkBB, 15, 0, 21, 42, 0, 21, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 26, 1, 21, 31, 3, 21);
                generateBox(level, chunkBB, 21, 12, 21, 36, 12, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 17, 11, 21, 40, 11, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 16, 10, 21, 41, 10, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 15, 7, 21, 42, 9, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 16, 6, 21, 41, 6, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 17, 5, 21, 40, 5, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 21, 4, 21, 36, 4, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 22, 3, 21, 26, 3, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 31, 3, 21, 35, 3, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 23, 2, 21, 25, 2, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 32, 2, 21, 34, 2, 21, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 28, 4, 20, 29, 4, 21, BASE_LIGHT, BASE_LIGHT, false);
                placeBlock(level, BASE_LIGHT, 27, 3, 21, chunkBB);
                placeBlock(level, BASE_LIGHT, 30, 3, 21, chunkBB);
                placeBlock(level, BASE_LIGHT, 26, 2, 21, chunkBB);
                placeBlock(level, BASE_LIGHT, 31, 2, 21, chunkBB);
                placeBlock(level, BASE_LIGHT, 25, 1, 21, chunkBB);
                placeBlock(level, BASE_LIGHT, 32, 1, 21, chunkBB);
                for (i = 0; i < 7; ++i) {
                    placeBlock(level, BASE_BLACK, 28 - i, 6 + i, 21, chunkBB);
                    placeBlock(level, BASE_BLACK, 29 + i, 6 + i, 21, chunkBB);
                }
                for (i = 0; i < 4; ++i) {
                    placeBlock(level, BASE_BLACK, 28 - i, 9 + i, 21, chunkBB);
                    placeBlock(level, BASE_BLACK, 29 + i, 9 + i, 21, chunkBB);
                }
                placeBlock(level, BASE_BLACK, 28, 12, 21, chunkBB);
                placeBlock(level, BASE_BLACK, 29, 12, 21, chunkBB);
                for (i = 0; i < 3; ++i) {
                    placeBlock(level, BASE_BLACK, 22 - i * 2, 8, 21, chunkBB);
                    placeBlock(level, BASE_BLACK, 22 - i * 2, 9, 21, chunkBB);
                    placeBlock(level, BASE_BLACK, 35 + i * 2, 8, 21, chunkBB);
                    placeBlock(level, BASE_BLACK, 35 + i * 2, 9, 21, chunkBB);
                }
                generateWaterBox(level, chunkBB, 15, 13, 21, 42, 15, 21);
                generateWaterBox(level, chunkBB, 15, 1, 21, 15, 6, 21);
                generateWaterBox(level, chunkBB, 16, 1, 21, 16, 5, 21);
                generateWaterBox(level, chunkBB, 17, 1, 21, 20, 4, 21);
                generateWaterBox(level, chunkBB, 21, 1, 21, 21, 3, 21);
                generateWaterBox(level, chunkBB, 22, 1, 21, 22, 2, 21);
                generateWaterBox(level, chunkBB, 23, 1, 21, 24, 1, 21);
                generateWaterBox(level, chunkBB, 42, 1, 21, 42, 6, 21);
                generateWaterBox(level, chunkBB, 41, 1, 21, 41, 5, 21);
                generateWaterBox(level, chunkBB, 37, 1, 21, 40, 4, 21);
                generateWaterBox(level, chunkBB, 36, 1, 21, 36, 3, 21);
                generateWaterBox(level, chunkBB, 33, 1, 21, 34, 1, 21);
                generateWaterBox(level, chunkBB, 35, 1, 21, 35, 2, 21);
            }
    }
    void generateRoofPiece(JLevel& level, JRandom random, const BoundingBox& chunkBB) {

            if (chunkIntersects(chunkBB, 21, 21, 36, 36)) {
                generateBox(level, chunkBB, 21, 0, 22, 36, 0, 36, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 21, 1, 22, 36, 23, 36);
                for (int i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, 21 + i, 13 + i, 21 + i, 36 - i, 13 + i, 21 + i, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 21 + i, 13 + i, 36 - i, 36 - i, 13 + i, 36 - i, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 21 + i, 13 + i, 22 + i, 21 + i, 13 + i, 35 - i, BASE_LIGHT, BASE_LIGHT, false);
                    generateBox(level, chunkBB, 36 - i, 13 + i, 22 + i, 36 - i, 13 + i, 35 - i, BASE_LIGHT, BASE_LIGHT, false);
                }
                generateBox(level, chunkBB, 25, 16, 25, 32, 16, 32, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 25, 17, 25, 25, 19, 25, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 32, 17, 25, 32, 19, 25, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 25, 17, 32, 25, 19, 32, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 32, 17, 32, 32, 19, 32, BASE_LIGHT, BASE_LIGHT, false);
                placeBlock(level, BASE_LIGHT, 26, 20, 26, chunkBB);
                placeBlock(level, BASE_LIGHT, 27, 21, 27, chunkBB);
                placeBlock(level, LAMP_BLOCK, 27, 20, 27, chunkBB);
                placeBlock(level, BASE_LIGHT, 26, 20, 31, chunkBB);
                placeBlock(level, BASE_LIGHT, 27, 21, 30, chunkBB);
                placeBlock(level, LAMP_BLOCK, 27, 20, 30, chunkBB);
                placeBlock(level, BASE_LIGHT, 31, 20, 31, chunkBB);
                placeBlock(level, BASE_LIGHT, 30, 21, 30, chunkBB);
                placeBlock(level, LAMP_BLOCK, 30, 20, 30, chunkBB);
                placeBlock(level, BASE_LIGHT, 31, 20, 26, chunkBB);
                placeBlock(level, BASE_LIGHT, 30, 21, 27, chunkBB);
                placeBlock(level, LAMP_BLOCK, 30, 20, 27, chunkBB);
                generateBox(level, chunkBB, 28, 21, 27, 29, 21, 27, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 27, 21, 28, 27, 21, 29, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 28, 21, 30, 29, 21, 30, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 30, 21, 28, 30, 21, 29, BASE_GRAY, BASE_GRAY, false);
            }
    }
    void generateLowerWall(JLevel& level, JRandom random, const BoundingBox& chunkBB) {

            int z;
            int i;
            if (chunkIntersects(chunkBB, 0, 21, 6, 58)) {
                generateBox(level, chunkBB, 0, 0, 21, 6, 0, 57, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 0, 1, 21, 6, 7, 57);
                generateBox(level, chunkBB, 4, 4, 21, 6, 4, 53, BASE_GRAY, BASE_GRAY, false);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, i, i + 1, 21, i, i + 1, 57 - i, BASE_LIGHT, BASE_LIGHT, false);
                }
                for (z = 23; z < 53; z += 3) {
                    placeBlock(level, DOT_DECO_DATA, 5, 5, z, chunkBB);
                }
                placeBlock(level, DOT_DECO_DATA, 5, 5, 52, chunkBB);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, i, i + 1, 21, i, i + 1, 57 - i, BASE_LIGHT, BASE_LIGHT, false);
                }
                generateBox(level, chunkBB, 4, 1, 52, 6, 3, 52, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 5, 1, 51, 5, 3, 53, BASE_GRAY, BASE_GRAY, false);
            }
            if (chunkIntersects(chunkBB, 51, 21, 58, 58)) {
                generateBox(level, chunkBB, 51, 0, 21, 57, 0, 57, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 51, 1, 21, 57, 7, 57);
                generateBox(level, chunkBB, 51, 4, 21, 53, 4, 53, BASE_GRAY, BASE_GRAY, false);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, 57 - i, i + 1, 21, 57 - i, i + 1, 57 - i, BASE_LIGHT, BASE_LIGHT, false);
                }
                for (z = 23; z < 53; z += 3) {
                    placeBlock(level, DOT_DECO_DATA, 52, 5, z, chunkBB);
                }
                placeBlock(level, DOT_DECO_DATA, 52, 5, 52, chunkBB);
                generateBox(level, chunkBB, 51, 1, 52, 53, 3, 52, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 52, 1, 51, 52, 3, 53, BASE_GRAY, BASE_GRAY, false);
            }
            if (chunkIntersects(chunkBB, 0, 51, 57, 57)) {
                generateBox(level, chunkBB, 7, 0, 51, 50, 0, 57, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 7, 1, 51, 50, 10, 57);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, i + 1, i + 1, 57 - i, 56 - i, i + 1, 57 - i, BASE_LIGHT, BASE_LIGHT, false);
                }
            }
    }
    void generateMiddleWall(JLevel& level, JRandom random, const BoundingBox& chunkBB) {

            int z;
            int i;
            if (chunkIntersects(chunkBB, 7, 21, 13, 50)) {
                generateBox(level, chunkBB, 7, 0, 21, 13, 0, 50, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 7, 1, 21, 13, 10, 50);
                generateBox(level, chunkBB, 11, 8, 21, 13, 8, 53, BASE_GRAY, BASE_GRAY, false);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, i + 7, i + 5, 21, i + 7, i + 5, 54, BASE_LIGHT, BASE_LIGHT, false);
                }
                for (z = 21; z <= 45; z += 3) {
                    placeBlock(level, DOT_DECO_DATA, 12, 9, z, chunkBB);
                }
            }
            if (chunkIntersects(chunkBB, 44, 21, 50, 54)) {
                generateBox(level, chunkBB, 44, 0, 21, 50, 0, 50, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 44, 1, 21, 50, 10, 50);
                generateBox(level, chunkBB, 44, 8, 21, 46, 8, 53, BASE_GRAY, BASE_GRAY, false);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, 50 - i, i + 5, 21, 50 - i, i + 5, 54, BASE_LIGHT, BASE_LIGHT, false);
                }
                for (z = 21; z <= 45; z += 3) {
                    placeBlock(level, DOT_DECO_DATA, 45, 9, z, chunkBB);
                }
            }
            if (chunkIntersects(chunkBB, 8, 44, 49, 54)) {
                generateBox(level, chunkBB, 14, 0, 44, 43, 0, 50, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 14, 1, 44, 43, 10, 50);
                for (int x = 12; x <= 45; x += 3) {
                    placeBlock(level, DOT_DECO_DATA, x, 9, 45, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 9, 52, chunkBB);
                    if (x != 12 && x != 18 && x != 24 && x != 33 && x != 39 && x != 45) continue;
                    placeBlock(level, DOT_DECO_DATA, x, 9, 47, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 9, 50, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 10, 45, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 10, 46, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 10, 51, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 10, 52, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 11, 47, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 11, 50, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 12, 48, chunkBB);
                    placeBlock(level, DOT_DECO_DATA, x, 12, 49, chunkBB);
                }
                for (i = 0; i < 3; ++i) {
                    generateBox(level, chunkBB, 8 + i, 5 + i, 54, 49 - i, 5 + i, 54, BASE_GRAY, BASE_GRAY, false);
                }
                generateBox(level, chunkBB, 11, 8, 54, 46, 8, 54, BASE_LIGHT, BASE_LIGHT, false);
                generateBox(level, chunkBB, 14, 8, 44, 43, 8, 53, BASE_GRAY, BASE_GRAY, false);
            }
    }
    void generateUpperWall(JLevel& level, JRandom random, const BoundingBox& chunkBB) {

            int z;
            int i;
            if (chunkIntersects(chunkBB, 14, 21, 20, 43)) {
                generateBox(level, chunkBB, 14, 0, 21, 20, 0, 43, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 14, 1, 22, 20, 14, 43);
                generateBox(level, chunkBB, 18, 12, 22, 20, 12, 39, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 18, 12, 21, 20, 12, 21, BASE_LIGHT, BASE_LIGHT, false);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, i + 14, i + 9, 21, i + 14, i + 9, 43 - i, BASE_LIGHT, BASE_LIGHT, false);
                }
                for (z = 23; z <= 39; z += 3) {
                    placeBlock(level, DOT_DECO_DATA, 19, 13, z, chunkBB);
                }
            }
            if (chunkIntersects(chunkBB, 37, 21, 43, 43)) {
                generateBox(level, chunkBB, 37, 0, 21, 43, 0, 43, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 37, 1, 22, 43, 14, 43);
                generateBox(level, chunkBB, 37, 12, 22, 39, 12, 39, BASE_GRAY, BASE_GRAY, false);
                generateBox(level, chunkBB, 37, 12, 21, 39, 12, 21, BASE_LIGHT, BASE_LIGHT, false);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, 43 - i, i + 9, 21, 43 - i, i + 9, 43 - i, BASE_LIGHT, BASE_LIGHT, false);
                }
                for (z = 23; z <= 39; z += 3) {
                    placeBlock(level, DOT_DECO_DATA, 38, 13, z, chunkBB);
                }
            }
            if (chunkIntersects(chunkBB, 15, 37, 42, 43)) {
                generateBox(level, chunkBB, 21, 0, 37, 36, 0, 43, BASE_GRAY, BASE_GRAY, false);
                generateWaterBox(level, chunkBB, 21, 1, 37, 36, 14, 43);
                generateBox(level, chunkBB, 21, 12, 37, 36, 12, 39, BASE_GRAY, BASE_GRAY, false);
                for (i = 0; i < 4; ++i) {
                    generateBox(level, chunkBB, 15 + i, i + 9, 43 - i, 42 - i, i + 9, 43 - i, BASE_LIGHT, BASE_LIGHT, false);
                }
                for (int x = 21; x <= 36; x += 3) {
                    placeBlock(level, DOT_DECO_DATA, x, 13, 38, chunkBB);
                }
            }
    }
};

class OceanMonumentStructure : public Structure {
public:
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        int offsetX = ctx.chunk.x * 16 + 9, offsetZ = ctx.chunk.z * 16 + 9;
        // BiomeSource.getBiomesWithin(x, seaLevel, z, 29): every biome in the box must be one that may surround a monument
        {
            int minX = (offsetX - 29) >> 2, maxX = (offsetX + 29) >> 2, minZ = (offsetZ - 29) >> 2, maxZ = (offsetZ + 29) >> 2;
            int minY = (ctx.env.sea_level - 29) >> 2, maxY = (ctx.env.sea_level + 29) >> 2;
            const Json& tag = ctx.env.reg->tag("biome/required_ocean_monument_surrounding");
            std::set<std::string> allowed;
            std::function<void(const Json&, int)> collect = [&](const Json& t, int depth) {
                if (depth > 8) return;
                if (const Json* v = t.find("values"))
                    for (auto& e : v->arr) {
                        const std::string& s = e.is_str() ? e.str : e.get_string("id");
                        if (!s.empty() && s[0] == '#') collect(ctx.env.reg->tag("biome/" + Registry::strip(s.substr(1))), depth + 1);
                        else allowed.insert(s.rfind("minecraft:", 0) == 0 ? s : "minecraft:" + s);
                    }
            };
            collect(tag, 0);
            std::set<int> seen;
            for (int xx = minX; xx <= maxX; ++xx)
                for (int yy = minY; yy <= maxY; ++yy)
                    for (int zz = minZ; zz <= maxZ; ++zz) seen.insert(ctx.env.biome_at(xx, yy, zz));
            for (int b : seen) if (!allowed.count((*ctx.env.biome_names)[size_t(b)])) return std::nullopt;
        }
        return on_top_of_chunk_center(ctx, OCEAN_FLOOR_WG, [&ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
            int west = ctx.chunk.x * 16 - 29, north = ctx.chunk.z * 16 - 29;
            Direction orientation = Direction::randomHorizontal(ctx.random);
            out.push_back(std::make_shared<MonumentBuilding>(JRandom(ctx.random), west, north, orientation));
        });
    }
};

struct RegisterMonument {
    RegisterMonument() {
        structure_factories()["minecraft:ocean_monument"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<OceanMonumentStructure>(); };
    }
} register_monument;

}  // namespace

}  // namespace pn::vg
