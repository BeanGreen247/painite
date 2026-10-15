// Nether fortresses (NetherFortressStructure and NetherFortressPieces): bridges and castle corridors grown from a crossing by weighted random pieces.
#include "legacy_piece.hpp"
#include <algorithm>

namespace pn::vg {

using namespace jc;

namespace {

BS CAVE_AIR_STATE() { return blk("minecraft:cave_air"); }
#define CAVE_AIR CAVE_AIR_STATE()

enum FortressKind { F_BRIDGE_STRAIGHT, F_BRIDGE_CROSSING, F_ROOM_CROSSING, F_STAIRS_ROOM, F_MONSTER_THRONE, F_CASTLE_ENTRANCE, F_CORRIDOR, F_CORRIDOR_CROSSING, F_RIGHT_TURN, F_LEFT_TURN,
                    F_CORRIDOR_STAIRS, F_CORRIDOR_T_BALCONY, F_STALK_ROOM };

struct PieceWeight {
    FortressKind kind;
    int weight;
    int placeCount;
    int maxPlaceCount;
    bool allowInRow;
    bool doPlace(int) const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
    bool isValid() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
};

const PieceWeight kBridgeWeights[] = {{F_BRIDGE_STRAIGHT, 30, 0, 0, true}, {F_BRIDGE_CROSSING, 10, 0, 4, false}, {F_ROOM_CROSSING, 10, 0, 4, false}, {F_STAIRS_ROOM, 10, 0, 3, false},
                                      {F_MONSTER_THRONE, 5, 0, 2, false}, {F_CASTLE_ENTRANCE, 5, 0, 1, false}};
const PieceWeight kCastleWeights[] = {{F_CORRIDOR, 25, 0, 0, true}, {F_CORRIDOR_CROSSING, 15, 0, 5, false}, {F_RIGHT_TURN, 5, 0, 10, false}, {F_LEFT_TURN, 5, 0, 10, false},
                                      {F_CORRIDOR_STAIRS, 10, 0, 3, true}, {F_CORRIDOR_T_BALCONY, 7, 0, 2, false}, {F_STALK_ROOM, 5, 0, 2, false}};

class StartPiece;
class NetherBridgePiece;
NetherBridgePiece* findAndCreateBridgePieceFactory(FortressKind kind, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth);
class BridgeEndFiller;
NetherBridgePiece* createBridgeEndFiller(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth);

class NetherBridgePiece : public LegacyPiece {
protected:
    NetherBridgePiece(const char* id, int genDepth, const BoundingBox& box) { type = id; gen_depth = genDepth; bb = box; }
    static bool isOkBox(const BoundingBox& box) { return box.y0 > 10; }
    int updatePieceWeight(std::vector<PieceWeight>& currentPieces) {
        bool hasAnyPieces = false;
        int totalWeight = 0;
        for (auto& piece : currentPieces) {
            if (piece.maxPlaceCount > 0 && piece.placeCount < piece.maxPlaceCount) hasAnyPieces = true;
            totalWeight += piece.weight;
        }
        return hasAnyPieces ? totalWeight : -1;
    }
    NetherBridgePiece* generatePiece(StructurePiece* startPiece, std::vector<PieceWeight>& currentPieces, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth);
    StructurePiece* generateAndAddPiece(StructurePiece* startPiece, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth, bool isCastle);
    StructurePiece* generateChildForward(StructurePiece* startPiece, PieceAccessor& accessor, JRandom random, int xOff, int yOff, bool isCastle) {
        auto orientation = getOrientation();
        if (orientation) {
            switch (orientation->v) {
                case pn::vg::NORTH: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + xOff, bb.y0 + yOff, bb.z0 - 1, *orientation, getGenDepth(), isCastle);
                case pn::vg::SOUTH: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + xOff, bb.y0 + yOff, bb.z1 + 1, *orientation, getGenDepth(), isCastle);
                case pn::vg::WEST: return generateAndAddPiece(startPiece, accessor, random, bb.x0 - 1, bb.y0 + yOff, bb.z0 + xOff, *orientation, getGenDepth(), isCastle);
                case pn::vg::EAST: return generateAndAddPiece(startPiece, accessor, random, bb.x1 + 1, bb.y0 + yOff, bb.z0 + xOff, *orientation, getGenDepth(), isCastle);
            }
        }
        return nullptr;
    }
    StructurePiece* generateChildLeft(StructurePiece* startPiece, PieceAccessor& accessor, JRandom random, int yOff, int zOff, bool isCastle) {
        auto orientation = getOrientation();
        if (orientation) {
            switch (orientation->v) {
                case pn::vg::NORTH: case pn::vg::SOUTH: return generateAndAddPiece(startPiece, accessor, random, bb.x0 - 1, bb.y0 + yOff, bb.z0 + zOff, Direction::WEST, getGenDepth(), isCastle);
                case pn::vg::WEST: case pn::vg::EAST: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + zOff, bb.y0 + yOff, bb.z0 - 1, Direction::NORTH, getGenDepth(), isCastle);
            }
        }
        return nullptr;
    }
    StructurePiece* generateChildRight(StructurePiece* startPiece, PieceAccessor& accessor, JRandom random, int yOff, int zOff, bool isCastle) {
        auto orientation = getOrientation();
        if (orientation) {
            switch (orientation->v) {
                case pn::vg::NORTH: case pn::vg::SOUTH: return generateAndAddPiece(startPiece, accessor, random, bb.x1 + 1, bb.y0 + yOff, bb.z0 + zOff, Direction::EAST, getGenDepth(), isCastle);
                case pn::vg::WEST: case pn::vg::EAST: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + zOff, bb.y0 + yOff, bb.z1 + 1, Direction::SOUTH, getGenDepth(), isCastle);
            }
        }
        return nullptr;
    }
};

class BridgeStraight : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 5;
    static constexpr int HEIGHT = 10;
    static constexpr int DEPTH = 19;
    BridgeStraight(int genDepth, JRandom random, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nebs", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildForward(startPiece, structurePieceAccessor, random, 1, 3, false);
    }
    static BridgeStraight* createPiece(PieceAccessor& structurePieceAccessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -3, 0, 5, 10, 19, direction);
            if (!BridgeStraight::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new BridgeStraight(genDepth, random, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 0, 3, 0, 4, 4, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 5, 0, 3, 7, 18, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 0, 5, 0, 0, 5, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 5, 0, 4, 5, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 4, 2, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 13, 4, 2, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 0, 0, 4, 1, 3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 0, 15, 4, 1, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (int x = 0; x <= 4; ++x) {
                for (int z = 0; z <= 2; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, 18 - z, chunkBB);
                }
            }
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            BS nseFence = nsFence.setValue("east", true);
            BS nswFence = nsFence.setValue("west", true);
            generateBox(level, chunkBB, 0, 1, 1, 0, 4, 1, nseFence, nseFence, false);
            generateBox(level, chunkBB, 0, 3, 4, 0, 4, 4, nseFence, nseFence, false);
            generateBox(level, chunkBB, 0, 3, 14, 0, 4, 14, nseFence, nseFence, false);
            generateBox(level, chunkBB, 0, 1, 17, 0, 4, 17, nseFence, nseFence, false);
            generateBox(level, chunkBB, 4, 1, 1, 4, 4, 1, nswFence, nswFence, false);
            generateBox(level, chunkBB, 4, 3, 4, 4, 4, 4, nswFence, nswFence, false);
            generateBox(level, chunkBB, 4, 3, 14, 4, 4, 14, nswFence, nswFence, false);
            generateBox(level, chunkBB, 4, 1, 17, 4, 4, 17, nswFence, nswFence, false);
    }
};

class BridgeCrossing : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 19;
    static constexpr int HEIGHT = 10;
    static constexpr int DEPTH = 19;
    BridgeCrossing(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nebcr", genDepth, bb) {setOrientation(direction);}
    BridgeCrossing(int west, int north, const Direction& direction) : NetherBridgePiece("minecraft:nebcr", 0, makeBoundingBox(west, 64, north, direction, 19, 10, 19)) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildForward(startPiece, structurePieceAccessor, random, 8, 3, false);
            generateChildLeft(startPiece, structurePieceAccessor, random, 3, 8, false);
            generateChildRight(startPiece, structurePieceAccessor, random, 3, 8, false);
    }
    static BridgeCrossing* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -8, -3, 0, 19, 10, 19, direction);
            if (!BridgeCrossing::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new BridgeCrossing(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            int z;
            int x;
            generateBox(level, chunkBB, 7, 3, 0, 11, 4, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 3, 7, 18, 4, 11, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 8, 5, 0, 10, 7, 18, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 0, 5, 8, 18, 7, 10, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 7, 5, 0, 7, 5, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 7, 5, 11, 7, 5, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 11, 5, 0, 11, 5, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 11, 5, 11, 11, 5, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 5, 7, 7, 5, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 11, 5, 7, 18, 5, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 5, 11, 7, 5, 11, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 11, 5, 11, 18, 5, 11, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 7, 2, 0, 11, 2, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 7, 2, 13, 11, 2, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 7, 0, 0, 11, 1, 3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 7, 0, 15, 11, 1, 18, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (x = 7; x <= 11; ++x) {
                for (z = 0; z <= 2; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, 18 - z, chunkBB);
                }
            }
            generateBox(level, chunkBB, 0, 2, 7, 5, 2, 11, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 13, 2, 7, 18, 2, 11, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 0, 7, 3, 1, 11, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 15, 0, 7, 18, 1, 11, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (x = 0; x <= 2; ++x) {
                for (z = 7; z <= 11; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                    fillColumnDown(level, blk("minecraft:nether_bricks"), 18 - x, -1, z, chunkBB);
                }
            }
    }
};

class RoomCrossing : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 7;
    static constexpr int HEIGHT = 9;
    static constexpr int DEPTH = 7;
    RoomCrossing(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nerc", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildForward(startPiece, structurePieceAccessor, random, 2, 0, false);
            generateChildLeft(startPiece, structurePieceAccessor, random, 0, 2, false);
            generateChildRight(startPiece, structurePieceAccessor, random, 0, 2, false);
    }
    static RoomCrossing* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -2, 0, 0, 7, 9, 7, direction);
            if (!RoomCrossing::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new RoomCrossing(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 0, 0, 0, 6, 1, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 6, 7, 6, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 0, 2, 0, 1, 6, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 6, 1, 6, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 2, 0, 6, 6, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 2, 6, 6, 6, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 0, 6, 1, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 5, 0, 6, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 2, 0, 6, 6, 1, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 2, 5, 6, 6, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            BS weFence = (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("east", true);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            generateBox(level, chunkBB, 2, 6, 0, 4, 6, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 0, 4, 5, 0, weFence, weFence, false);
            generateBox(level, chunkBB, 2, 6, 6, 4, 6, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 6, 4, 5, 6, weFence, weFence, false);
            generateBox(level, chunkBB, 0, 6, 2, 0, 6, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 5, 2, 0, 5, 4, nsFence, nsFence, false);
            generateBox(level, chunkBB, 6, 6, 2, 6, 6, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 5, 2, 6, 5, 4, nsFence, nsFence, false);
            for (int x = 0; x <= 6; ++x) {
                for (int z = 0; z <= 6; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class StairsRoom : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 7;
    static constexpr int HEIGHT = 11;
    static constexpr int DEPTH = 7;
    StairsRoom(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nesr", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildRight(startPiece, structurePieceAccessor, random, 6, 2, false);
    }
    static StairsRoom* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, int genDepth, const Direction& direction) {
            BoundingBox box = orientBox(footX, footY, footZ, -2, 0, 0, 7, 11, 7, direction);
            if (!StairsRoom::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new StairsRoom(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 0, 0, 0, 6, 1, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 6, 10, 6, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 0, 2, 0, 1, 8, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 2, 0, 6, 8, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 1, 0, 8, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 2, 1, 6, 8, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 2, 6, 5, 8, 6, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            BS weFence = (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("east", true);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            generateBox(level, chunkBB, 0, 3, 2, 0, 5, 4, nsFence, nsFence, false);
            generateBox(level, chunkBB, 6, 3, 2, 6, 5, 2, nsFence, nsFence, false);
            generateBox(level, chunkBB, 6, 3, 4, 6, 5, 4, nsFence, nsFence, false);
            placeBlock(level, blk("minecraft:nether_bricks"), 5, 2, 5, chunkBB);
            generateBox(level, chunkBB, 4, 2, 5, 4, 3, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 3, 2, 5, 3, 4, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 2, 5, 2, 5, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 2, 5, 1, 6, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 7, 1, 5, 7, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 8, 2, 6, 8, 4, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 2, 6, 0, 4, 8, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 0, 4, 5, 0, weFence, weFence, false);
            for (int x = 0; x <= 6; ++x) {
                for (int z = 0; z <= 6; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class MonsterThrone : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 7;
    static constexpr int HEIGHT = 8;
    static constexpr int DEPTH = 9;
    bool hasPlacedSpawner;
    MonsterThrone(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nemt", genDepth, bb) {setOrientation(direction);}
    static MonsterThrone* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, int genDepth, const Direction& direction) {
            BoundingBox box = orientBox(footX, footY, footZ, -2, 0, 0, 7, 8, 9, direction);
            if (!MonsterThrone::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new MonsterThrone(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            BlockPos pos;
            generateBox(level, chunkBB, 0, 2, 0, 6, 7, 7, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 1, 0, 0, 5, 1, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 2, 1, 5, 2, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 3, 2, 5, 3, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 4, 3, 5, 4, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 2, 0, 1, 4, 2, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 2, 0, 5, 4, 2, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 5, 2, 1, 5, 3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 5, 2, 5, 5, 3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 5, 3, 0, 5, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 5, 3, 6, 5, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 5, 8, 5, 5, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            BS weFence = (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("east", true);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            placeBlock(level, blk("minecraft:nether_brick_fence").setValue("west", true), 1, 6, 3, chunkBB);
            placeBlock(level, blk("minecraft:nether_brick_fence").setValue("east", true), 5, 6, 3, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("east", true)).setValue("north", true), 0, 6, 3, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("north", true), 6, 6, 3, chunkBB);
            generateBox(level, chunkBB, 0, 6, 4, 0, 6, 7, nsFence, nsFence, false);
            generateBox(level, chunkBB, 6, 6, 4, 6, 6, 7, nsFence, nsFence, false);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("east", true)).setValue("south", true), 0, 6, 8, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("south", true), 6, 6, 8, chunkBB);
            generateBox(level, chunkBB, 1, 6, 8, 5, 6, 8, weFence, weFence, false);
            placeBlock(level, blk("minecraft:nether_brick_fence").setValue("east", true), 1, 7, 8, chunkBB);
            generateBox(level, chunkBB, 2, 7, 8, 4, 7, 8, weFence, weFence, false);
            placeBlock(level, blk("minecraft:nether_brick_fence").setValue("west", true), 5, 7, 8, chunkBB);
            placeBlock(level, blk("minecraft:nether_brick_fence").setValue("east", true), 2, 8, 8, chunkBB);
            placeBlock(level, weFence, 3, 8, 8, chunkBB);
            placeBlock(level, blk("minecraft:nether_brick_fence").setValue("west", true), 4, 8, 8, chunkBB);
            if (!hasPlacedSpawner && chunkBB.inside(pos = getWorldPos(3, 5, 5))) {
                hasPlacedSpawner = true;
                level.setBlock(pos, blk("minecraft:spawner"), 2);
                setSpawnerEntity(level, pos, "minecraft:blaze");
            }
            for (int x = 0; x <= 6; ++x) {
                for (int z = 0; z <= 6; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class CastleEntrance : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 13;
    static constexpr int HEIGHT = 14;
    static constexpr int DEPTH = 13;
    CastleEntrance(int genDepth, JRandom random, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nece", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildForward(startPiece, structurePieceAccessor, random, 5, 3, true);
    }
    static CastleEntrance* createPiece(PieceAccessor& structurePieceAccessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -5, -3, 0, 13, 14, 13, direction);
            if (!CastleEntrance::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new CastleEntrance(genDepth, random, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            int z;
            int x;
            generateBox(level, chunkBB, 0, 3, 0, 12, 4, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 5, 0, 12, 13, 12, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 0, 5, 0, 1, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 11, 5, 0, 12, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 11, 4, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 8, 5, 11, 10, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 9, 11, 7, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 0, 4, 12, 1, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 8, 5, 0, 10, 12, 1, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 9, 0, 7, 12, 1, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 11, 2, 10, 12, 10, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 8, 0, 7, 8, 0, blk("minecraft:nether_brick_fence"), blk("minecraft:nether_brick_fence"), false);
            BS weFence = (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("east", true);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            for (int i = 1; i <= 11; i += 2) {
                generateBox(level, chunkBB, i, 10, 0, i, 11, 0, weFence, weFence, false);
                generateBox(level, chunkBB, i, 10, 12, i, 11, 12, weFence, weFence, false);
                generateBox(level, chunkBB, 0, 10, i, 0, 11, i, nsFence, nsFence, false);
                generateBox(level, chunkBB, 12, 10, i, 12, 11, i, nsFence, nsFence, false);
                placeBlock(level, blk("minecraft:nether_bricks"), i, 13, 0, chunkBB);
                placeBlock(level, blk("minecraft:nether_bricks"), i, 13, 12, chunkBB);
                placeBlock(level, blk("minecraft:nether_bricks"), 0, 13, i, chunkBB);
                placeBlock(level, blk("minecraft:nether_bricks"), 12, 13, i, chunkBB);
                if (i == 11) continue;
                placeBlock(level, weFence, i + 1, 13, 0, chunkBB);
                placeBlock(level, weFence, i + 1, 13, 12, chunkBB);
                placeBlock(level, nsFence, 0, 13, i + 1, chunkBB);
                placeBlock(level, nsFence, 12, 13, i + 1, chunkBB);
            }
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("east", true), 0, 13, 0, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("south", true)).setValue("east", true), 0, 13, 12, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("south", true)).setValue("west", true), 12, 13, 12, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("west", true), 12, 13, 0, chunkBB);
            for (int z2 = 3; z2 <= 9; z2 += 2) {
                generateBox(level, chunkBB, 1, 7, z2, 1, 8, z2, nsFence.setValue("west", true), nsFence.setValue("west", true), false);
                generateBox(level, chunkBB, 11, 7, z2, 11, 8, z2, nsFence.setValue("east", true), nsFence.setValue("east", true), false);
            }
            generateBox(level, chunkBB, 4, 2, 0, 8, 2, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 4, 12, 2, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 0, 0, 8, 1, 3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 0, 9, 8, 1, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 0, 4, 3, 1, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 9, 0, 4, 12, 1, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (x = 4; x <= 8; ++x) {
                for (z = 0; z <= 2; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, 12 - z, chunkBB);
                }
            }
            for (x = 0; x <= 2; ++x) {
                for (z = 4; z <= 8; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                    fillColumnDown(level, blk("minecraft:nether_bricks"), 12 - x, -1, z, chunkBB);
                }
            }
            generateBox(level, chunkBB, 5, 5, 5, 7, 5, 7, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 1, 6, 6, 4, 6, blk("minecraft:air"), blk("minecraft:air"), false);
            placeBlock(level, blk("minecraft:nether_bricks"), 6, 0, 6, chunkBB);
            placeBlock(level, blk("minecraft:lava"), 6, 5, 6, chunkBB);
            BlockPos pos = getWorldPos(6, 5, 6);
    }
};

class CastleSmallCorridorPiece : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 5;
    static constexpr int HEIGHT = 7;
    static constexpr int DEPTH = 5;
    CastleSmallCorridorPiece(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nesc", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildForward(startPiece, structurePieceAccessor, random, 1, 0, true);
    }
    static CastleSmallCorridorPiece* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -1, 0, 0, 5, 7, 5, direction);
            if (!CastleSmallCorridorPiece::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new CastleSmallCorridorPiece(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 0, 0, 0, 4, 1, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 4, 5, 4, blk("minecraft:air"), blk("minecraft:air"), false);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            generateBox(level, chunkBB, 0, 2, 0, 0, 5, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 2, 0, 4, 5, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 3, 1, 0, 4, 1, nsFence, nsFence, false);
            generateBox(level, chunkBB, 0, 3, 3, 0, 4, 3, nsFence, nsFence, false);
            generateBox(level, chunkBB, 4, 3, 1, 4, 4, 1, nsFence, nsFence, false);
            generateBox(level, chunkBB, 4, 3, 3, 4, 4, 3, nsFence, nsFence, false);
            generateBox(level, chunkBB, 0, 6, 0, 4, 6, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (int x = 0; x <= 4; ++x) {
                for (int z = 0; z <= 4; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class CastleSmallCorridorRightTurnPiece : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 5;
    static constexpr int HEIGHT = 7;
    static constexpr int DEPTH = 5;
    bool isNeedingChest;
    CastleSmallCorridorRightTurnPiece(int genDepth, JRandom random, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nescrt", genDepth, bb) {setOrientation(direction);
            isNeedingChest = random.nextInt(3) == 0;}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildRight(startPiece, structurePieceAccessor, random, 0, 1, true);
    }
    static CastleSmallCorridorRightTurnPiece* createPiece(PieceAccessor& structurePieceAccessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -1, 0, 0, 5, 7, 5, direction);
            if (!CastleSmallCorridorRightTurnPiece::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new CastleSmallCorridorRightTurnPiece(genDepth, random, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 0, 0, 0, 4, 1, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 4, 5, 4, blk("minecraft:air"), blk("minecraft:air"), false);
            BS weFence = (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("east", true);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            generateBox(level, chunkBB, 0, 2, 0, 0, 5, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 3, 1, 0, 4, 1, nsFence, nsFence, false);
            generateBox(level, chunkBB, 0, 3, 3, 0, 4, 3, nsFence, nsFence, false);
            generateBox(level, chunkBB, 4, 2, 0, 4, 5, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 2, 4, 4, 5, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 3, 4, 1, 4, 4, weFence, weFence, false);
            generateBox(level, chunkBB, 3, 3, 4, 3, 4, 4, weFence, weFence, false);
            if (isNeedingChest && chunkBB.inside(getWorldPos(1, 2, 3))) {
                isNeedingChest = false;
                createChest(level, chunkBB, random, 1, 2, 3, "minecraft:chests/nether_bridge");
            }
            generateBox(level, chunkBB, 0, 6, 0, 4, 6, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (int x = 0; x <= 4; ++x) {
                for (int z = 0; z <= 4; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class CastleSmallCorridorLeftTurnPiece : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 5;
    static constexpr int HEIGHT = 7;
    static constexpr int DEPTH = 5;
    bool isNeedingChest;
    CastleSmallCorridorLeftTurnPiece(int genDepth, JRandom random, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nesclt", genDepth, bb) {setOrientation(direction);
            isNeedingChest = random.nextInt(3) == 0;}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildLeft(startPiece, structurePieceAccessor, random, 0, 1, true);
    }
    static CastleSmallCorridorLeftTurnPiece* createPiece(PieceAccessor& structurePieceAccessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -1, 0, 0, 5, 7, 5, direction);
            if (!CastleSmallCorridorLeftTurnPiece::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new CastleSmallCorridorLeftTurnPiece(genDepth, random, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 0, 0, 0, 4, 1, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 4, 5, 4, blk("minecraft:air"), blk("minecraft:air"), false);
            BS weFence = (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("east", true);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            generateBox(level, chunkBB, 4, 2, 0, 4, 5, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 3, 1, 4, 4, 1, nsFence, nsFence, false);
            generateBox(level, chunkBB, 4, 3, 3, 4, 4, 3, nsFence, nsFence, false);
            generateBox(level, chunkBB, 0, 2, 0, 0, 5, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 4, 3, 5, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 3, 4, 1, 4, 4, weFence, weFence, false);
            generateBox(level, chunkBB, 3, 3, 4, 3, 4, 4, weFence, weFence, false);
            if (isNeedingChest && chunkBB.inside(getWorldPos(3, 2, 3))) {
                isNeedingChest = false;
                createChest(level, chunkBB, random, 3, 2, 3, "minecraft:chests/nether_bridge");
            }
            generateBox(level, chunkBB, 0, 6, 0, 4, 6, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (int x = 0; x <= 4; ++x) {
                for (int z = 0; z <= 4; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class CastleCorridorStairsPiece : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 5;
    static constexpr int HEIGHT = 14;
    static constexpr int DEPTH = 10;
    CastleCorridorStairsPiece(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:neccs", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildForward(startPiece, structurePieceAccessor, random, 1, 0, true);
    }
    static CastleCorridorStairsPiece* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -7, 0, 5, 14, 10, direction);
            if (!CastleCorridorStairsPiece::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new CastleCorridorStairsPiece(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            BS stairs = blk("minecraft:nether_brick_stairs").setValue("facing", Direction::SOUTH);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            for (int step = 0; step <= 9; ++step) {
                int floor = std::max(1, 7 - step);
                int roof = std::min(std::max(floor + 5, 14 - step), 13);
                int z = step;
                generateBox(level, chunkBB, 0, 0, z, 4, floor, z, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
                generateBox(level, chunkBB, 1, floor + 1, z, 3, roof - 1, z, blk("minecraft:air"), blk("minecraft:air"), false);
                if (step <= 6) {
                    placeBlock(level, stairs, 1, floor + 1, z, chunkBB);
                    placeBlock(level, stairs, 2, floor + 1, z, chunkBB);
                    placeBlock(level, stairs, 3, floor + 1, z, chunkBB);
                }
                generateBox(level, chunkBB, 0, roof, z, 4, roof, z, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
                generateBox(level, chunkBB, 0, floor + 1, z, 0, roof - 1, z, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
                generateBox(level, chunkBB, 4, floor + 1, z, 4, roof - 1, z, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
                if ((step & 1) == 0) {
                    generateBox(level, chunkBB, 0, floor + 2, z, 0, floor + 3, z, nsFence, nsFence, false);
                    generateBox(level, chunkBB, 4, floor + 2, z, 4, floor + 3, z, nsFence, nsFence, false);
                }
                for (int x = 0; x <= 4; ++x) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class CastleCorridorTBalconyPiece : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 9;
    static constexpr int HEIGHT = 7;
    static constexpr int DEPTH = 9;
    CastleCorridorTBalconyPiece(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nectb", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            int zOff = 1;
            auto orientation = getOrientation();
            if (orientation == Direction::WEST || orientation == Direction::NORTH) {
                zOff = 5;
            }
            generateChildLeft(startPiece, structurePieceAccessor, random, 0, zOff, random.nextInt(8) > 0);
            generateChildRight(startPiece, structurePieceAccessor, random, 0, zOff, random.nextInt(8) > 0);
    }
    static CastleCorridorTBalconyPiece* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -3, 0, 0, 9, 7, 9, direction);
            if (!CastleCorridorTBalconyPiece::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new CastleCorridorTBalconyPiece(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            BS weFence = (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("east", true);
            generateBox(level, chunkBB, 0, 0, 0, 8, 1, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 8, 5, 8, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 0, 6, 0, 8, 6, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 2, 5, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 2, 0, 8, 5, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 3, 0, 1, 4, 0, weFence, weFence, false);
            generateBox(level, chunkBB, 7, 3, 0, 7, 4, 0, weFence, weFence, false);
            generateBox(level, chunkBB, 0, 2, 4, 8, 2, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 1, 4, 2, 2, 4, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 6, 1, 4, 7, 2, 4, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 1, 3, 8, 7, 3, 8, weFence, weFence, false);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("east", true)).setValue("south", true), 0, 3, 8, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("south", true), 8, 3, 8, chunkBB);
            generateBox(level, chunkBB, 0, 3, 6, 0, 3, 7, nsFence, nsFence, false);
            generateBox(level, chunkBB, 8, 3, 6, 8, 3, 7, nsFence, nsFence, false);
            generateBox(level, chunkBB, 0, 3, 4, 0, 5, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 8, 3, 4, 8, 5, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 3, 5, 2, 5, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 6, 3, 5, 7, 5, 5, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 1, 4, 5, 1, 5, 5, weFence, weFence, false);
            generateBox(level, chunkBB, 7, 4, 5, 7, 5, 5, weFence, weFence, false);
            for (int z = 0; z <= 5; ++z) {
                for (int x = 0; x <= 8; ++x) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class CastleSmallCorridorCrossingPiece : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 5;
    static constexpr int HEIGHT = 7;
    static constexpr int DEPTH = 5;
    CastleSmallCorridorCrossingPiece(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nescsc", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildForward(startPiece, structurePieceAccessor, random, 1, 0, true);
            generateChildLeft(startPiece, structurePieceAccessor, random, 0, 1, true);
            generateChildRight(startPiece, structurePieceAccessor, random, 0, 1, true);
    }
    static CastleSmallCorridorCrossingPiece* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -1, 0, 0, 5, 7, 5, direction);
            if (!CastleSmallCorridorCrossingPiece::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new CastleSmallCorridorCrossingPiece(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            generateBox(level, chunkBB, 0, 0, 0, 4, 1, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 0, 4, 5, 4, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 0, 2, 0, 0, 5, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 2, 0, 4, 5, 0, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 4, 0, 5, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 2, 4, 4, 5, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 6, 0, 4, 6, 4, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (int x = 0; x <= 4; ++x) {
                for (int z = 0; z <= 4; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                }
            }
    }
};

class CastleStalkRoom : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 13;
    static constexpr int HEIGHT = 14;
    static constexpr int DEPTH = 13;
    CastleStalkRoom(int genDepth, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:necsr", genDepth, bb) {setOrientation(direction);}
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
            generateChildForward(startPiece, structurePieceAccessor, random, 5, 3, true);
            generateChildForward(startPiece, structurePieceAccessor, random, 5, 11, true);
    }
    static CastleStalkRoom* createPiece(PieceAccessor& structurePieceAccessor, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -5, -3, 0, 13, 14, 13, direction);
            if (!CastleStalkRoom::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new CastleStalkRoom(genDepth, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            int z;
            int x;
            generateBox(level, chunkBB, 0, 3, 0, 12, 4, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 5, 0, 12, 13, 12, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 0, 5, 0, 1, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 11, 5, 0, 12, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 11, 4, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 8, 5, 11, 10, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 9, 11, 7, 12, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 0, 4, 12, 1, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 8, 5, 0, 10, 12, 1, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 5, 9, 0, 7, 12, 1, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 11, 2, 10, 12, 10, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            BS weFence = (blk("minecraft:nether_brick_fence").setValue("west", true)).setValue("east", true);
            BS nsFence = (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("south", true);
            BS nswFence = nsFence.setValue("west", true);
            BS nseFence = nsFence.setValue("east", true);
            for (int i = 1; i <= 11; i += 2) {
                generateBox(level, chunkBB, i, 10, 0, i, 11, 0, weFence, weFence, false);
                generateBox(level, chunkBB, i, 10, 12, i, 11, 12, weFence, weFence, false);
                generateBox(level, chunkBB, 0, 10, i, 0, 11, i, nsFence, nsFence, false);
                generateBox(level, chunkBB, 12, 10, i, 12, 11, i, nsFence, nsFence, false);
                placeBlock(level, blk("minecraft:nether_bricks"), i, 13, 0, chunkBB);
                placeBlock(level, blk("minecraft:nether_bricks"), i, 13, 12, chunkBB);
                placeBlock(level, blk("minecraft:nether_bricks"), 0, 13, i, chunkBB);
                placeBlock(level, blk("minecraft:nether_bricks"), 12, 13, i, chunkBB);
                if (i == 11) continue;
                placeBlock(level, weFence, i + 1, 13, 0, chunkBB);
                placeBlock(level, weFence, i + 1, 13, 12, chunkBB);
                placeBlock(level, nsFence, 0, 13, i + 1, chunkBB);
                placeBlock(level, nsFence, 12, 13, i + 1, chunkBB);
            }
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("east", true), 0, 13, 0, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("south", true)).setValue("east", true), 0, 13, 12, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("south", true)).setValue("west", true), 12, 13, 12, chunkBB);
            placeBlock(level, (blk("minecraft:nether_brick_fence").setValue("north", true)).setValue("west", true), 12, 13, 0, chunkBB);
            for (int z2 = 3; z2 <= 9; z2 += 2) {
                generateBox(level, chunkBB, 1, 7, z2, 1, 8, z2, nswFence, nswFence, false);
                generateBox(level, chunkBB, 11, 7, z2, 11, 8, z2, nseFence, nseFence, false);
            }
            BS stairs = blk("minecraft:nether_brick_stairs").setValue("facing", Direction::NORTH);
            for (int i = 0; i <= 6; ++i) {
                int z3 = i + 4;
                for (x = 5; x <= 7; ++x) {
                    placeBlock(level, stairs, x, 5 + i, z3, chunkBB);
                }
                if (z3 >= 5 && z3 <= 8) {
                    generateBox(level, chunkBB, 5, 5, z3, 7, i + 4, z3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
                } else if (z3 >= 9 && z3 <= 10) {
                    generateBox(level, chunkBB, 5, 8, z3, 7, i + 4, z3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
                }
                if (i < 1) continue;
                generateBox(level, chunkBB, 5, 6 + i, z3, 7, 9 + i, z3, blk("minecraft:air"), blk("minecraft:air"), false);
            }
            for (int x2 = 5; x2 <= 7; ++x2) {
                placeBlock(level, stairs, x2, 12, 11, chunkBB);
            }
            generateBox(level, chunkBB, 5, 6, 7, 5, 7, 7, nseFence, nseFence, false);
            generateBox(level, chunkBB, 7, 6, 7, 7, 7, 7, nswFence, nswFence, false);
            generateBox(level, chunkBB, 5, 13, 12, 7, 13, 12, blk("minecraft:air"), blk("minecraft:air"), false);
            generateBox(level, chunkBB, 2, 5, 2, 3, 5, 3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 9, 3, 5, 10, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 2, 5, 4, 2, 5, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 9, 5, 2, 10, 5, 3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 9, 5, 9, 10, 5, 10, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 10, 5, 4, 10, 5, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            BS eastStairs = stairs.setValue("facing", Direction::EAST);
            BS westStairs = stairs.setValue("facing", Direction::WEST);
            placeBlock(level, westStairs, 4, 5, 2, chunkBB);
            placeBlock(level, westStairs, 4, 5, 3, chunkBB);
            placeBlock(level, westStairs, 4, 5, 9, chunkBB);
            placeBlock(level, westStairs, 4, 5, 10, chunkBB);
            placeBlock(level, eastStairs, 8, 5, 2, chunkBB);
            placeBlock(level, eastStairs, 8, 5, 3, chunkBB);
            placeBlock(level, eastStairs, 8, 5, 9, chunkBB);
            placeBlock(level, eastStairs, 8, 5, 10, chunkBB);
            generateBox(level, chunkBB, 3, 4, 4, 4, 4, 8, blk("minecraft:soul_sand"), blk("minecraft:soul_sand"), false);
            generateBox(level, chunkBB, 8, 4, 4, 9, 4, 8, blk("minecraft:soul_sand"), blk("minecraft:soul_sand"), false);
            generateBox(level, chunkBB, 3, 5, 4, 4, 5, 8, blk("minecraft:nether_wart"), blk("minecraft:nether_wart"), false);
            generateBox(level, chunkBB, 8, 5, 4, 9, 5, 8, blk("minecraft:nether_wart"), blk("minecraft:nether_wart"), false);
            generateBox(level, chunkBB, 4, 2, 0, 8, 2, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 2, 4, 12, 2, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 0, 0, 8, 1, 3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 4, 0, 9, 8, 1, 12, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 0, 0, 4, 3, 1, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            generateBox(level, chunkBB, 9, 0, 4, 12, 1, 8, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (x = 4; x <= 8; ++x) {
                for (z = 0; z <= 2; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, 12 - z, chunkBB);
                }
            }
            for (x = 0; x <= 2; ++x) {
                for (z = 4; z <= 8; ++z) {
                    fillColumnDown(level, blk("minecraft:nether_bricks"), x, -1, z, chunkBB);
                    fillColumnDown(level, blk("minecraft:nether_bricks"), 12 - x, -1, z, chunkBB);
                }
            }
    }
};

class BridgeEndFiller : public NetherBridgePiece {
public:
    static constexpr int WIDTH = 5;
    static constexpr int HEIGHT = 10;
    static constexpr int DEPTH = 8;
    int selfSeed;
    BridgeEndFiller(int genDepth, JRandom random, const BoundingBox& bb, const Direction& direction) : NetherBridgePiece("minecraft:nebef", genDepth, bb) {setOrientation(direction);
            selfSeed = random.nextInt();}
    static BridgeEndFiller* createPiece(PieceAccessor& structurePieceAccessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -3, 0, 5, 10, 8, direction);
            if (!BridgeEndFiller::isOkBox(box) || structurePieceAccessor.findCollisionPiece(box) != nullptr) {
                return nullptr;
            }
            return new BridgeEndFiller(genDepth, random, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
            int z;
            int y;
            int x;
            JRandom selfRandom = JRandom::createThreadLocalInstance(selfSeed);
            for (x = 0; x <= 4; ++x) {
                for (y = 3; y <= 4; ++y) {
                    z = selfRandom.nextInt(8);
                    generateBox(level, chunkBB, x, y, 0, x, y, z, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
                }
            }
            int z2 = selfRandom.nextInt(8);
            generateBox(level, chunkBB, 0, 5, 0, 0, 5, z2, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            z2 = selfRandom.nextInt(8);
            generateBox(level, chunkBB, 4, 5, 0, 4, 5, z2, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            for (x = 0; x <= 4; ++x) {
                int z3 = selfRandom.nextInt(5);
                generateBox(level, chunkBB, x, 2, 0, x, 2, z3, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
            }
            for (x = 0; x <= 4; ++x) {
                for (y = 0; y <= 1; ++y) {
                    z = selfRandom.nextInt(3);
                    generateBox(level, chunkBB, x, y, 0, x, y, z, blk("minecraft:nether_bricks"), blk("minecraft:nether_bricks"), false);
                }
            }
    }
};



class StartPiece : public BridgeCrossing {
public:
    int previousPiece = -1;   // kind of the PieceWeight picked last
    std::vector<PieceWeight> availableBridgePieces, availableCastlePieces;
    std::vector<StructurePiece*> pendingChildren;
    StartPiece(JRandom random, int west, int north) : BridgeCrossing(west, north, Direction::randomHorizontal(random.r())) {
        for (auto w : kBridgeWeights) { w.placeCount = 0; availableBridgePieces.push_back(w); }
        for (auto w : kCastleWeights) { w.placeCount = 0; availableCastlePieces.push_back(w); }
    }
};

NetherBridgePiece* NetherBridgePiece::generatePiece(StructurePiece* startPiece_, std::vector<PieceWeight>& currentPieces, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth) {
    StartPiece* startPiece = static_cast<StartPiece*>(startPiece_);
    int totalWeight = updatePieceWeight(currentPieces);
    bool doStuff = totalWeight > 0 && depth <= 30;
    int numAttempts = 0;
    while (numAttempts < 5 && doStuff) {
        ++numAttempts;
        int weightSelection = random.nextInt(totalWeight);
        for (size_t idx = 0; idx < currentPieces.size(); idx++) {
            PieceWeight& piece = currentPieces[idx];
            if ((weightSelection -= piece.weight) >= 0) continue;
            if (!piece.doPlace(depth) || (int(piece.kind) == startPiece->previousPiece && !piece.allowInRow)) break;   // continue block0
            NetherBridgePiece* structurePiece = findAndCreateBridgePieceFactory(piece.kind, accessor, random, footX, footY, footZ, direction, depth);
            if (!structurePiece) continue;
            ++piece.placeCount;
            startPiece->previousPiece = int(piece.kind);
            if (!piece.isValid()) currentPieces.erase(currentPieces.begin() + long(idx));
            return structurePiece;
        }
    }
    return createBridgeEndFiller(accessor, random, footX, footY, footZ, direction, depth);
}

StructurePiece* NetherBridgePiece::generateAndAddPiece(StructurePiece* startPiece_, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth, bool isCastle) {
    StartPiece* startPiece = static_cast<StartPiece*>(startPiece_);
    if (std::abs(footX - startPiece->bb.x0) > 112 || std::abs(footZ - startPiece->bb.z0) > 112) return createBridgeEndFiller(accessor, random, footX, footY, footZ, direction, depth);
    std::vector<PieceWeight>& availablePieces = isCastle ? startPiece->availableCastlePieces : startPiece->availableBridgePieces;
    NetherBridgePiece* newPiece = generatePiece(startPiece, availablePieces, accessor, random, footX, footY, footZ, direction, depth + 1);
    if (newPiece) {
        accessor.addPiece(std::shared_ptr<StructurePiece>(newPiece));
        startPiece->pendingChildren.push_back(newPiece);
    }
    return newPiece;
}

NetherBridgePiece* createBridgeEndFiller(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth) { return BridgeEndFiller::createPiece(accessor, random, footX, footY, footZ, direction, depth); }

NetherBridgePiece* findAndCreateBridgePieceFactory(FortressKind kind, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth) {
    switch (kind) {
        case F_BRIDGE_STRAIGHT: return BridgeStraight::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case F_BRIDGE_CROSSING: return BridgeCrossing::createPiece(accessor, footX, footY, footZ, direction, depth);
        case F_ROOM_CROSSING: return RoomCrossing::createPiece(accessor, footX, footY, footZ, direction, depth);
        case F_STAIRS_ROOM: return StairsRoom::createPiece(accessor, footX, footY, footZ, depth, direction);
        case F_MONSTER_THRONE: return MonsterThrone::createPiece(accessor, footX, footY, footZ, depth, direction);
        case F_CASTLE_ENTRANCE: return CastleEntrance::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case F_CORRIDOR: return CastleSmallCorridorPiece::createPiece(accessor, footX, footY, footZ, direction, depth);
        case F_RIGHT_TURN: return CastleSmallCorridorRightTurnPiece::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case F_LEFT_TURN: return CastleSmallCorridorLeftTurnPiece::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case F_CORRIDOR_STAIRS: return CastleCorridorStairsPiece::createPiece(accessor, footX, footY, footZ, direction, depth);
        case F_CORRIDOR_T_BALCONY: return CastleCorridorTBalconyPiece::createPiece(accessor, footX, footY, footZ, direction, depth);
        case F_CORRIDOR_CROSSING: return CastleSmallCorridorCrossingPiece::createPiece(accessor, footX, footY, footZ, direction, depth);
        case F_STALK_ROOM: return CastleStalkRoom::createPiece(accessor, footX, footY, footZ, direction, depth);
    }
    return nullptr;
}

class FortressStructure : public Structure {
public:
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        GenerationStub stub;
        stub.position = BlockPos(ctx.chunk.x * 16, 64, ctx.chunk.z * 16);
        stub.generate = [&ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
            PieceAccessor builder(out);
            JRandom random(ctx.random);
            auto start = std::make_shared<StartPiece>(random, ctx.chunk.x * 16 + 2, ctx.chunk.z * 16 + 2);
            builder.addPiece(start);
            start->addChildren(start.get(), builder, random);
            std::vector<StructurePiece*>& pendingChildren = start->pendingChildren;
            while (!pendingChildren.empty()) {
                int pos = random.nextInt(int(pendingChildren.size()));
                StructurePiece* piece = pendingChildren[size_t(pos)];
                pendingChildren.erase(pendingChildren.begin() + pos);
                static_cast<LegacyPiece*>(piece)->addChildren(start.get(), builder, random);
            }
            builder.moveInsideHeights(ctx.random, 48, 70);
        };
        return stub;
    }
};

struct RegisterFortress {
    RegisterFortress() {
        structure_factories()["minecraft:fortress"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<FortressStructure>(); };
    }
} register_fortress;

}  // namespace

}  // namespace pn::vg
