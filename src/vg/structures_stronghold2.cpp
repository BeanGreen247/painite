// Strongholds (StrongholdStructure and StrongholdPieces): corridors, rooms, libraries and the portal room, grown from a stairway by weighted random pieces.
#include "legacy_piece.hpp"
#include <algorithm>

namespace pn::vg {

using namespace jc;

namespace {

BS CAVE_AIR_STATE() { return blk("minecraft:cave_air"); }
#define CAVE_AIR CAVE_AIR_STATE()

enum PieceKind { K_STRAIGHT, K_PRISON_HALL, K_LEFT_TURN, K_RIGHT_TURN, K_ROOM_CROSSING, K_STRAIGHT_STAIRS_DOWN, K_STAIRS_DOWN, K_FIVE_CROSSING, K_CHEST_CORRIDOR, K_LIBRARY, K_PORTAL_ROOM, K_NONE };

struct PieceWeight {
    PieceKind kind;
    int weight;
    int placeCount;
    int maxPlaceCount;
    int minDepthExclusive;   // Library: depth > 4, PortalRoom: depth > 5 (0: no extra condition)
    bool doPlace(int depth) const { return (maxPlaceCount == 0 || placeCount < maxPlaceCount) && (minDepthExclusive == 0 || depth > minDepthExclusive); }
    bool isValid() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
};

// StrongholdPieces' static state: one stronghold is made at a time per thread
struct StrongholdState {
    std::vector<PieceWeight> currentPieces;
    PieceKind imposedPiece = K_NONE;
    int totalWeight = 0;
    int previousPieceKind = -1;   // identity of StartPiece.previousPiece (index into the initial table)
};
thread_local StrongholdState g_state;

const PieceWeight kInitialWeights[] = {{K_STRAIGHT, 40, 0, 0, 0}, {K_PRISON_HALL, 5, 0, 5, 0}, {K_LEFT_TURN, 20, 0, 0, 0}, {K_RIGHT_TURN, 20, 0, 0, 0}, {K_ROOM_CROSSING, 10, 0, 6, 0},
                                       {K_STRAIGHT_STAIRS_DOWN, 5, 0, 5, 0}, {K_STAIRS_DOWN, 5, 0, 5, 0}, {K_FIVE_CROSSING, 5, 0, 4, 0}, {K_CHEST_CORRIDOR, 5, 0, 4, 0},
                                       {K_LIBRARY, 10, 0, 2, 4}, {K_PORTAL_ROOM, 20, 0, 1, 5}};

void resetPieces() {
    g_state.currentPieces.assign(std::begin(kInitialWeights), std::end(kInitialWeights));
    g_state.imposedPiece = K_NONE;
}

bool updatePieceWeight() {
    bool hasAnyPieces = false;
    g_state.totalWeight = 0;
    for (auto& piece : g_state.currentPieces) {
        if (piece.maxPlaceCount > 0 && piece.placeCount < piece.maxPlaceCount) hasAnyPieces = true;
        g_state.totalWeight += piece.weight;
    }
    return hasAnyPieces;
}

class StartPiece;
class StrongholdPiece;
StrongholdPiece* findAndCreatePieceFactory(PieceKind kind, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth);
StrongholdPiece* generatePieceFromSmallDoor(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth);
StrongholdPiece* generateAndAddPiece(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth);

struct SmoothStoneSelector : BlockSelector {
    void next(JRandom& random, int, int, int, bool isEdge) override {
        if (isEdge) {
            float selection = random.nextFloat();
            next_ = selection < 0.2f ? blk("minecraft:cracked_stone_bricks") : selection < 0.5f ? blk("minecraft:mossy_stone_bricks") : selection < 0.55f ? blk("minecraft:infested_stone_bricks") : blk("minecraft:stone_bricks");
        } else next_ = blk("minecraft:cave_air");
    }
};
SmoothStoneSelector SMOOTH_STONE_SELECTOR;

enum SmallDoorType { OPENING = 0, WOOD_DOOR = 1, GRATES = 2, IRON_DOOR = 3 };

class StrongholdPiece : public LegacyPiece {
public:
    int entryDoor = OPENING;
protected:
    StrongholdPiece(const char* id, int genDepth, const BoundingBox& box) { type = id; gen_depth = genDepth; bb = box; }
    void generateSmallDoor(JLevel& level, JRandom random, const BoundingBox& chunkBB, int doorType, int footX, int footY, int footZ) {

            switch (doorType) {
                case 0: {
                    generateBox(level, chunkBB, footX, footY, footZ, footX + 3 - 1, footY + 3 - 1, footZ, CAVE_AIR, CAVE_AIR, false);
                    break;
                }
                case 1: {
                    placeBlock(level, blk("minecraft:stone_bricks"), footX, footY, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX, footY + 1, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX, footY + 2, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX + 1, footY + 2, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX + 2, footY + 2, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX + 2, footY + 1, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX + 2, footY, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:oak_door"), footX + 1, footY, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:oak_door").setValue("half", "upper"), footX + 1, footY + 1, footZ, chunkBB);
                    break;
                }
                case 2: {
                    placeBlock(level, blk("minecraft:cave_air"), footX + 1, footY, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:cave_air"), footX + 1, footY + 1, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:iron_bars").setValue("west", true), footX, footY, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:iron_bars").setValue("west", true), footX, footY + 1, footZ, chunkBB);
                    placeBlock(level, (blk("minecraft:iron_bars").setValue("east", true)).setValue("west", true), footX, footY + 2, footZ, chunkBB);
                    placeBlock(level, (blk("minecraft:iron_bars").setValue("east", true)).setValue("west", true), footX + 1, footY + 2, footZ, chunkBB);
                    placeBlock(level, (blk("minecraft:iron_bars").setValue("east", true)).setValue("west", true), footX + 2, footY + 2, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:iron_bars").setValue("east", true), footX + 2, footY + 1, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:iron_bars").setValue("east", true), footX + 2, footY, footZ, chunkBB);
                    break;
                }
                case 3: {
                    placeBlock(level, blk("minecraft:stone_bricks"), footX, footY, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX, footY + 1, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX, footY + 2, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX + 1, footY + 2, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX + 2, footY + 2, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX + 2, footY + 1, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), footX + 2, footY, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:iron_door"), footX + 1, footY, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:iron_door").setValue("half", "upper"), footX + 1, footY + 1, footZ, chunkBB);
                    placeBlock(level, blk("minecraft:stone_button").setValue("facing", Direction::NORTH), footX + 2, footY + 1, footZ + 1, chunkBB);
                    placeBlock(level, blk("minecraft:stone_button").setValue("facing", Direction::SOUTH), footX + 2, footY + 1, footZ - 1, chunkBB);
                }
            }
    }
    int randomSmallDoor(JRandom random) {
        int selection = random.nextInt(5);
        switch (selection) {
            default: return OPENING;
            case 2: return WOOD_DOOR;
            case 3: return GRATES;
            case 4: return IRON_DOOR;
        }
    }
    StrongholdPiece* generateSmallDoorChildForward(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int xOff, int yOff);
    StrongholdPiece* generateSmallDoorChildLeft(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int yOff, int zOff);
    StrongholdPiece* generateSmallDoorChildRight(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int yOff, int zOff);
    static bool isOkBox(const BoundingBox& box) { return box.y0 > 10; }
};

class StartPiece;

class Straight : public StrongholdPiece {
public:
    bool leftChild, rightChild;
    Straight(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shs", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
        leftChild = random.nextInt(2) == 0;
        rightChild = random.nextInt(2) == 0;
    }
    static Straight* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 7, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new Straight(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 4, 4, 6, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
            generateSmallDoor(level, random, chunkBB, OPENING, 1, 1, 6);
            BS eastTorch = blk("minecraft:wall_torch").setValue("facing", Direction::EAST);
            BS westTorch = blk("minecraft:wall_torch").setValue("facing", Direction::WEST);
            maybeGenerateBlock(level, chunkBB, random, 0.1f, 1, 2, 1, eastTorch);
            maybeGenerateBlock(level, chunkBB, random, 0.1f, 3, 2, 1, westTorch);
            maybeGenerateBlock(level, chunkBB, random, 0.1f, 1, 2, 5, eastTorch);
            maybeGenerateBlock(level, chunkBB, random, 0.1f, 3, 2, 5, westTorch);
            if (leftChild) {
                generateBox(level, chunkBB, 0, 1, 2, 0, 3, 4, CAVE_AIR, CAVE_AIR, false);
            }
            if (rightChild) {
                generateBox(level, chunkBB, 4, 1, 2, 4, 3, 4, CAVE_AIR, CAVE_AIR, false);
            }
    }
};

class PrisonHall : public StrongholdPiece {
public:
    PrisonHall(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shph", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
    }
    static PrisonHall* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 9, 5, 11, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new PrisonHall(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 8, 4, 10, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
            generateBox(level, chunkBB, 1, 1, 10, 3, 3, 10, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, 4, 1, 1, 4, 3, 1, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 4, 1, 3, 4, 3, 3, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 4, 1, 7, 4, 3, 7, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 4, 1, 9, 4, 3, 9, false, random, SMOOTH_STONE_SELECTOR);
            for (int y = 1; y <= 3; ++y) {
                placeBlock(level, (blk("minecraft:iron_bars").setValue("north", true)).setValue("south", true), 4, y, 4, chunkBB);
                placeBlock(level, ((blk("minecraft:iron_bars").setValue("north", true)).setValue("south", true)).setValue("east", true), 4, y, 5, chunkBB);
                placeBlock(level, (blk("minecraft:iron_bars").setValue("north", true)).setValue("south", true), 4, y, 6, chunkBB);
                placeBlock(level, (blk("minecraft:iron_bars").setValue("west", true)).setValue("east", true), 5, y, 5, chunkBB);
                placeBlock(level, (blk("minecraft:iron_bars").setValue("west", true)).setValue("east", true), 6, y, 5, chunkBB);
                placeBlock(level, (blk("minecraft:iron_bars").setValue("west", true)).setValue("east", true), 7, y, 5, chunkBB);
            }
            placeBlock(level, (blk("minecraft:iron_bars").setValue("north", true)).setValue("south", true), 4, 3, 2, chunkBB);
            placeBlock(level, (blk("minecraft:iron_bars").setValue("north", true)).setValue("south", true), 4, 3, 8, chunkBB);
            BS doorBottom = blk("minecraft:iron_door").setValue("facing", Direction::WEST);
            BS doorTop = (blk("minecraft:iron_door").setValue("facing", Direction::WEST)).setValue("half", "upper");
            placeBlock(level, doorBottom, 4, 1, 2, chunkBB);
            placeBlock(level, doorTop, 4, 2, 2, chunkBB);
            placeBlock(level, doorBottom, 4, 1, 8, chunkBB);
            placeBlock(level, doorTop, 4, 2, 8, chunkBB);
    }
};

class Turn : public StrongholdPiece {
protected:
    Turn(const char* id, int genDepth, const BoundingBox& box) : StrongholdPiece(id, genDepth, box) {}
};

class LeftTurn : public Turn {
public:
    LeftTurn(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : Turn("minecraft:shlt", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
    }
    static LeftTurn* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 5, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new LeftTurn(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 4, 4, 4, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
            Direction orientation = *getOrientation();
            if (orientation == Direction::NORTH || orientation == Direction::EAST) {
                generateBox(level, chunkBB, 0, 1, 1, 0, 3, 3, CAVE_AIR, CAVE_AIR, false);
            } else {
                generateBox(level, chunkBB, 4, 1, 1, 4, 3, 3, CAVE_AIR, CAVE_AIR, false);
            }
    }
};

class RightTurn : public Turn {
public:
    RightTurn(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : Turn("minecraft:shrt", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
    }
    static RightTurn* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 5, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new RightTurn(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 4, 4, 4, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
            Direction orientation = *getOrientation();
            if (orientation == Direction::NORTH || orientation == Direction::EAST) {
                generateBox(level, chunkBB, 4, 1, 1, 4, 3, 3, CAVE_AIR, CAVE_AIR, false);
            } else {
                generateBox(level, chunkBB, 0, 1, 1, 0, 3, 3, CAVE_AIR, CAVE_AIR, false);
            }
    }
};

class RoomCrossing : public StrongholdPiece {
public:
    int roomType;
    RoomCrossing(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shrc", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
        roomType = random.nextInt(5);
    }
    static RoomCrossing* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -4, -1, 0, 11, 7, 11, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new RoomCrossing(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 10, 6, 10, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 4, 1, 0);
            generateBox(level, chunkBB, 4, 1, 10, 6, 3, 10, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, 0, 1, 4, 0, 3, 6, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, 10, 1, 4, 10, 3, 6, CAVE_AIR, CAVE_AIR, false);
            switch (roomType) {
                default: {
                    break;
                }
                case 0: {
                    placeBlock(level, blk("minecraft:stone_bricks"), 5, 1, 5, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), 5, 2, 5, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), 5, 3, 5, chunkBB);
                    placeBlock(level, blk("minecraft:wall_torch").setValue("facing", Direction::WEST), 4, 3, 5, chunkBB);
                    placeBlock(level, blk("minecraft:wall_torch").setValue("facing", Direction::EAST), 6, 3, 5, chunkBB);
                    placeBlock(level, blk("minecraft:wall_torch").setValue("facing", Direction::SOUTH), 5, 3, 4, chunkBB);
                    placeBlock(level, blk("minecraft:wall_torch").setValue("facing", Direction::NORTH), 5, 3, 6, chunkBB);
                    placeBlock(level, blk("minecraft:smooth_stone_slab"), 4, 1, 4, chunkBB);
                    placeBlock(level, blk("minecraft:smooth_stone_slab"), 4, 1, 5, chunkBB);
                    placeBlock(level, blk("minecraft:smooth_stone_slab"), 4, 1, 6, chunkBB);
                    placeBlock(level, blk("minecraft:smooth_stone_slab"), 6, 1, 4, chunkBB);
                    placeBlock(level, blk("minecraft:smooth_stone_slab"), 6, 1, 5, chunkBB);
                    placeBlock(level, blk("minecraft:smooth_stone_slab"), 6, 1, 6, chunkBB);
                    placeBlock(level, blk("minecraft:smooth_stone_slab"), 5, 1, 4, chunkBB);
                    placeBlock(level, blk("minecraft:smooth_stone_slab"), 5, 1, 6, chunkBB);
                    break;
                }
                case 1: {
                    for (int i = 0; i < 5; ++i) {
                        placeBlock(level, blk("minecraft:stone_bricks"), 3, 1, 3 + i, chunkBB);
                        placeBlock(level, blk("minecraft:stone_bricks"), 7, 1, 3 + i, chunkBB);
                        placeBlock(level, blk("minecraft:stone_bricks"), 3 + i, 1, 3, chunkBB);
                        placeBlock(level, blk("minecraft:stone_bricks"), 3 + i, 1, 7, chunkBB);
                    }
                    placeBlock(level, blk("minecraft:stone_bricks"), 5, 1, 5, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), 5, 2, 5, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), 5, 3, 5, chunkBB);
                    placeBlock(level, blk("minecraft:water"), 5, 4, 5, chunkBB);
                    break;
                }
                case 2: {
                    int z;
                    for (z = 1; z <= 9; ++z) {
                        placeBlock(level, blk("minecraft:cobblestone"), 1, 3, z, chunkBB);
                        placeBlock(level, blk("minecraft:cobblestone"), 9, 3, z, chunkBB);
                    }
                    for (int x = 1; x <= 9; ++x) {
                        placeBlock(level, blk("minecraft:cobblestone"), x, 3, 1, chunkBB);
                        placeBlock(level, blk("minecraft:cobblestone"), x, 3, 9, chunkBB);
                    }
                    placeBlock(level, blk("minecraft:cobblestone"), 5, 1, 4, chunkBB);
                    placeBlock(level, blk("minecraft:cobblestone"), 5, 1, 6, chunkBB);
                    placeBlock(level, blk("minecraft:cobblestone"), 5, 3, 4, chunkBB);
                    placeBlock(level, blk("minecraft:cobblestone"), 5, 3, 6, chunkBB);
                    placeBlock(level, blk("minecraft:cobblestone"), 4, 1, 5, chunkBB);
                    placeBlock(level, blk("minecraft:cobblestone"), 6, 1, 5, chunkBB);
                    placeBlock(level, blk("minecraft:cobblestone"), 4, 3, 5, chunkBB);
                    placeBlock(level, blk("minecraft:cobblestone"), 6, 3, 5, chunkBB);
                    for (int y = 1; y <= 3; ++y) {
                        placeBlock(level, blk("minecraft:cobblestone"), 4, y, 4, chunkBB);
                        placeBlock(level, blk("minecraft:cobblestone"), 6, y, 4, chunkBB);
                        placeBlock(level, blk("minecraft:cobblestone"), 4, y, 6, chunkBB);
                        placeBlock(level, blk("minecraft:cobblestone"), 6, y, 6, chunkBB);
                    }
                    placeBlock(level, blk("minecraft:wall_torch"), 5, 3, 5, chunkBB);
                    for (z = 2; z <= 8; ++z) {
                        placeBlock(level, blk("minecraft:oak_planks"), 2, 3, z, chunkBB);
                        placeBlock(level, blk("minecraft:oak_planks"), 3, 3, z, chunkBB);
                        if (z <= 3 || z >= 7) {
                            placeBlock(level, blk("minecraft:oak_planks"), 4, 3, z, chunkBB);
                            placeBlock(level, blk("minecraft:oak_planks"), 5, 3, z, chunkBB);
                            placeBlock(level, blk("minecraft:oak_planks"), 6, 3, z, chunkBB);
                        }
                        placeBlock(level, blk("minecraft:oak_planks"), 7, 3, z, chunkBB);
                        placeBlock(level, blk("minecraft:oak_planks"), 8, 3, z, chunkBB);
                    }
                    BS ladder = blk("minecraft:ladder").setValue("facing", Direction::WEST);
                    placeBlock(level, ladder, 9, 1, 3, chunkBB);
                    placeBlock(level, ladder, 9, 2, 3, chunkBB);
                    placeBlock(level, ladder, 9, 3, 3, chunkBB);
                    createChest(level, chunkBB, random, 3, 4, 8, "minecraft:chests/stronghold_crossing");
                }
            }
    }
};

class StraightStairsDown : public StrongholdPiece {
public:
    StraightStairsDown(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shssd", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
    }
    static StraightStairsDown* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -1, -7, 0, 5, 11, 8, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new StraightStairsDown(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 4, 10, 7, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 1, 7, 0);
            generateSmallDoor(level, random, chunkBB, OPENING, 1, 1, 7);
            BS stairs = blk("minecraft:cobblestone_stairs").setValue("facing", Direction::SOUTH);
            for (int i = 0; i < 6; ++i) {
                placeBlock(level, stairs, 1, 6 - i, 1 + i, chunkBB);
                placeBlock(level, stairs, 2, 6 - i, 1 + i, chunkBB);
                placeBlock(level, stairs, 3, 6 - i, 1 + i, chunkBB);
                if (i >= 5) continue;
                placeBlock(level, blk("minecraft:stone_bricks"), 1, 5 - i, 1 + i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 2, 5 - i, 1 + i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 3, 5 - i, 1 + i, chunkBB);
            }
    }
};

class StairsDown : public StrongholdPiece {
public:
    bool isSource;
    StairsDown(const char* id, int genDepth, int west, int north, const Direction& direction) : StrongholdPiece(id, genDepth, makeBoundingBox(west, 64, north, direction, 5, 11, 5)) {
        isSource = true;
        setOrientation(direction);
        entryDoor = OPENING;
    }
    StairsDown(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shsd", genDepth, box) {
        isSource = false;
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
    }
    static StairsDown* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -1, -7, 0, 5, 11, 5, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new StairsDown(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 4, 10, 4, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 1, 7, 0);
            generateSmallDoor(level, random, chunkBB, OPENING, 1, 1, 4);
            placeBlock(level, blk("minecraft:stone_bricks"), 2, 6, 1, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 1, 5, 1, chunkBB);
            placeBlock(level, blk("minecraft:smooth_stone_slab"), 1, 6, 1, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 1, 5, 2, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 1, 4, 3, chunkBB);
            placeBlock(level, blk("minecraft:smooth_stone_slab"), 1, 5, 3, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 2, 4, 3, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 3, 3, 3, chunkBB);
            placeBlock(level, blk("minecraft:smooth_stone_slab"), 3, 4, 3, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 3, 3, 2, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 3, 2, 1, chunkBB);
            placeBlock(level, blk("minecraft:smooth_stone_slab"), 3, 3, 1, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 2, 2, 1, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 1, 1, 1, chunkBB);
            placeBlock(level, blk("minecraft:smooth_stone_slab"), 1, 2, 1, chunkBB);
            placeBlock(level, blk("minecraft:stone_bricks"), 1, 1, 2, chunkBB);
            placeBlock(level, blk("minecraft:smooth_stone_slab"), 1, 1, 3, chunkBB);
    }
};

class PortalRoom;

class StartPiece : public StairsDown {
public:
    int previousPiece = -1;   // the PieceWeight picked last (by kind), -1 none
    PortalRoom* portalRoomPiece = nullptr;
    std::vector<StrongholdPiece*> pendingChildren;
    StartPiece(JRandom random, int west, int north) : StairsDown("minecraft:shstart", 0, west, north, Direction::randomHorizontal(random.r())) {}
};

class FiveCrossing : public StrongholdPiece {
public:
    bool leftLow, leftHigh, rightLow, rightHigh;
    FiveCrossing(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:sh5c", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
        leftLow = random.nextBoolean();
        leftHigh = random.nextBoolean();
        rightLow = random.nextBoolean();
        rightHigh = random.nextInt(3) > 0;
    }
    static FiveCrossing* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -4, -3, 0, 10, 9, 11, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new FiveCrossing(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 9, 8, 10, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 4, 3, 0);
            if (leftLow) {
                generateBox(level, chunkBB, 0, 3, 1, 0, 5, 3, CAVE_AIR, CAVE_AIR, false);
            }
            if (rightLow) {
                generateBox(level, chunkBB, 9, 3, 1, 9, 5, 3, CAVE_AIR, CAVE_AIR, false);
            }
            if (leftHigh) {
                generateBox(level, chunkBB, 0, 5, 7, 0, 7, 9, CAVE_AIR, CAVE_AIR, false);
            }
            if (rightHigh) {
                generateBox(level, chunkBB, 9, 5, 7, 9, 7, 9, CAVE_AIR, CAVE_AIR, false);
            }
            generateBox(level, chunkBB, 5, 1, 10, 7, 3, 10, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, 1, 2, 1, 8, 2, 6, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 4, 1, 5, 4, 4, 9, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 8, 1, 5, 8, 4, 9, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 1, 4, 7, 3, 4, 9, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 1, 3, 5, 3, 3, 6, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 1, 3, 4, 3, 3, 4, blk("minecraft:smooth_stone_slab"), blk("minecraft:smooth_stone_slab"), false);
            generateBox(level, chunkBB, 1, 4, 6, 3, 4, 6, blk("minecraft:smooth_stone_slab"), blk("minecraft:smooth_stone_slab"), false);
            generateBox(level, chunkBB, 5, 1, 7, 7, 1, 8, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 5, 1, 9, 7, 1, 9, blk("minecraft:smooth_stone_slab"), blk("minecraft:smooth_stone_slab"), false);
            generateBox(level, chunkBB, 5, 2, 7, 7, 2, 7, blk("minecraft:smooth_stone_slab"), blk("minecraft:smooth_stone_slab"), false);
            generateBox(level, chunkBB, 4, 5, 7, 4, 5, 9, blk("minecraft:smooth_stone_slab"), blk("minecraft:smooth_stone_slab"), false);
            generateBox(level, chunkBB, 8, 5, 7, 8, 5, 9, blk("minecraft:smooth_stone_slab"), blk("minecraft:smooth_stone_slab"), false);
            generateBox(level, chunkBB, 5, 5, 7, 7, 5, 9, blk("minecraft:smooth_stone_slab").setValue("type", "double"), blk("minecraft:smooth_stone_slab").setValue("type", "double"), false);
            placeBlock(level, blk("minecraft:wall_torch").setValue("facing", Direction::SOUTH), 6, 5, 6, chunkBB);
    }
};

class ChestCorridor : public StrongholdPiece {
public:
    bool hasPlacedChest = false;
    ChestCorridor(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shcc", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
    }
    static ChestCorridor* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 7, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new ChestCorridor(genDepth, random, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override;
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            generateBox(level, chunkBB, 0, 0, 0, 4, 4, 6, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 1, 1, 0);
            generateSmallDoor(level, random, chunkBB, OPENING, 1, 1, 6);
            generateBox(level, chunkBB, 3, 1, 2, 3, 1, 4, blk("minecraft:stone_bricks"), blk("minecraft:stone_bricks"), false);
            placeBlock(level, blk("minecraft:stone_brick_slab"), 3, 1, 1, chunkBB);
            placeBlock(level, blk("minecraft:stone_brick_slab"), 3, 1, 5, chunkBB);
            placeBlock(level, blk("minecraft:stone_brick_slab"), 3, 2, 2, chunkBB);
            placeBlock(level, blk("minecraft:stone_brick_slab"), 3, 2, 4, chunkBB);
            for (int z = 2; z <= 4; ++z) {
                placeBlock(level, blk("minecraft:stone_brick_slab"), 2, 1, z, chunkBB);
            }
            if (!hasPlacedChest && chunkBB.inside(getWorldPos(3, 2, 3))) {
                hasPlacedChest = true;
                createChest(level, chunkBB, random, 3, 2, 3, "minecraft:chests/stronghold_corridor");
            }
    }
};

class Library : public StrongholdPiece {
public:
    bool isTall;
    Library(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shli", genDepth, box) {
        setOrientation(direction);
        entryDoor = randomSmallDoor(random);
        isTall = box.y_span() > 6;
    }
    static Library* createPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -4, -1, 0, 14, 11, 15, direction);
        if (!(isOkBox(box) && !accessor.findCollisionPiece(box))) {
            box = orientBox(footX, footY, footZ, -4, -1, 0, 14, 6, 15, direction);
            if (!(isOkBox(box) && !accessor.findCollisionPiece(box))) return nullptr;
        }
        return new Library(genDepth, random, box, direction);
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            int d;
            int currentHeight = 11;
            if (!isTall) {
                currentHeight = 6;
            }
            generateBox(level, chunkBB, 0, 0, 0, 13, currentHeight - 1, 14, true, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, entryDoor, 4, 1, 0);
            generateMaybeBox(level, chunkBB, random, 0.07f, 2, 1, 1, 11, 4, 13, blk("minecraft:cobweb"), blk("minecraft:cobweb"), false, false);
            bool bookLeft = true;
            int bookRight = 12;
            for (d = 1; d <= 13; ++d) {
                if ((d - 1) % 4 == 0) {
                    generateBox(level, chunkBB, 1, 1, d, 1, 4, d, blk("minecraft:oak_planks"), blk("minecraft:oak_planks"), false);
                    generateBox(level, chunkBB, 12, 1, d, 12, 4, d, blk("minecraft:oak_planks"), blk("minecraft:oak_planks"), false);
                    placeBlock(level, blk("minecraft:wall_torch").setValue("facing", Direction::EAST), 2, 3, d, chunkBB);
                    placeBlock(level, blk("minecraft:wall_torch").setValue("facing", Direction::WEST), 11, 3, d, chunkBB);
                    if (!isTall) continue;
                    generateBox(level, chunkBB, 1, 6, d, 1, 9, d, blk("minecraft:oak_planks"), blk("minecraft:oak_planks"), false);
                    generateBox(level, chunkBB, 12, 6, d, 12, 9, d, blk("minecraft:oak_planks"), blk("minecraft:oak_planks"), false);
                    continue;
                }
                generateBox(level, chunkBB, 1, 1, d, 1, 4, d, blk("minecraft:bookshelf"), blk("minecraft:bookshelf"), false);
                generateBox(level, chunkBB, 12, 1, d, 12, 4, d, blk("minecraft:bookshelf"), blk("minecraft:bookshelf"), false);
                if (!isTall) continue;
                generateBox(level, chunkBB, 1, 6, d, 1, 9, d, blk("minecraft:bookshelf"), blk("minecraft:bookshelf"), false);
                generateBox(level, chunkBB, 12, 6, d, 12, 9, d, blk("minecraft:bookshelf"), blk("minecraft:bookshelf"), false);
            }
            for (d = 3; d < 12; d += 2) {
                generateBox(level, chunkBB, 3, 1, d, 4, 3, d, blk("minecraft:bookshelf"), blk("minecraft:bookshelf"), false);
                generateBox(level, chunkBB, 6, 1, d, 7, 3, d, blk("minecraft:bookshelf"), blk("minecraft:bookshelf"), false);
                generateBox(level, chunkBB, 9, 1, d, 10, 3, d, blk("minecraft:bookshelf"), blk("minecraft:bookshelf"), false);
            }
            if (isTall) {
                generateBox(level, chunkBB, 1, 5, 1, 3, 5, 13, blk("minecraft:oak_planks"), blk("minecraft:oak_planks"), false);
                generateBox(level, chunkBB, 10, 5, 1, 12, 5, 13, blk("minecraft:oak_planks"), blk("minecraft:oak_planks"), false);
                generateBox(level, chunkBB, 4, 5, 1, 9, 5, 2, blk("minecraft:oak_planks"), blk("minecraft:oak_planks"), false);
                generateBox(level, chunkBB, 4, 5, 12, 9, 5, 13, blk("minecraft:oak_planks"), blk("minecraft:oak_planks"), false);
                placeBlock(level, blk("minecraft:oak_planks"), 9, 5, 11, chunkBB);
                placeBlock(level, blk("minecraft:oak_planks"), 8, 5, 11, chunkBB);
                placeBlock(level, blk("minecraft:oak_planks"), 9, 5, 10, chunkBB);
                BS weFence = (blk("minecraft:oak_fence").setValue("west", true)).setValue("east", true);
                BS nsFence = (blk("minecraft:oak_fence").setValue("north", true)).setValue("south", true);
                generateBox(level, chunkBB, 3, 6, 3, 3, 6, 11, nsFence, nsFence, false);
                generateBox(level, chunkBB, 10, 6, 3, 10, 6, 9, nsFence, nsFence, false);
                generateBox(level, chunkBB, 4, 6, 2, 9, 6, 2, weFence, weFence, false);
                generateBox(level, chunkBB, 4, 6, 12, 7, 6, 12, weFence, weFence, false);
                placeBlock(level, (blk("minecraft:oak_fence").setValue("north", true)).setValue("east", true), 3, 6, 2, chunkBB);
                placeBlock(level, (blk("minecraft:oak_fence").setValue("south", true)).setValue("east", true), 3, 6, 12, chunkBB);
                placeBlock(level, (blk("minecraft:oak_fence").setValue("north", true)).setValue("west", true), 10, 6, 2, chunkBB);
                for (int i = 0; i <= 2; ++i) {
                    placeBlock(level, (blk("minecraft:oak_fence").setValue("south", true)).setValue("west", true), 8 + i, 6, 12 - i, chunkBB);
                    if (i == 2) continue;
                    placeBlock(level, (blk("minecraft:oak_fence").setValue("north", true)).setValue("east", true), 8 + i, 6, 11 - i, chunkBB);
                }
                BS ladder = blk("minecraft:ladder").setValue("facing", Direction::SOUTH);
                placeBlock(level, ladder, 10, 1, 13, chunkBB);
                placeBlock(level, ladder, 10, 2, 13, chunkBB);
                placeBlock(level, ladder, 10, 3, 13, chunkBB);
                placeBlock(level, ladder, 10, 4, 13, chunkBB);
                placeBlock(level, ladder, 10, 5, 13, chunkBB);
                placeBlock(level, ladder, 10, 6, 13, chunkBB);
                placeBlock(level, ladder, 10, 7, 13, chunkBB);
                int x = 7;
                int z = 7;
                BS eFence = blk("minecraft:oak_fence").setValue("east", true);
                placeBlock(level, eFence, 6, 9, 7, chunkBB);
                BS wFence = blk("minecraft:oak_fence").setValue("west", true);
                placeBlock(level, wFence, 7, 9, 7, chunkBB);
                placeBlock(level, eFence, 6, 8, 7, chunkBB);
                placeBlock(level, wFence, 7, 8, 7, chunkBB);
                BS nsweFence = (nsFence.setValue("west", true)).setValue("east", true);
                placeBlock(level, nsweFence, 6, 7, 7, chunkBB);
                placeBlock(level, nsweFence, 7, 7, 7, chunkBB);
                placeBlock(level, eFence, 5, 7, 7, chunkBB);
                placeBlock(level, wFence, 8, 7, 7, chunkBB);
                placeBlock(level, eFence.setValue("north", true), 6, 7, 6, chunkBB);
                placeBlock(level, eFence.setValue("south", true), 6, 7, 8, chunkBB);
                placeBlock(level, wFence.setValue("north", true), 7, 7, 6, chunkBB);
                placeBlock(level, wFence.setValue("south", true), 7, 7, 8, chunkBB);
                BS torch = blk("minecraft:torch");
                placeBlock(level, torch, 5, 8, 7, chunkBB);
                placeBlock(level, torch, 8, 8, 7, chunkBB);
                placeBlock(level, torch, 6, 8, 6, chunkBB);
                placeBlock(level, torch, 6, 8, 8, chunkBB);
                placeBlock(level, torch, 7, 8, 6, chunkBB);
                placeBlock(level, torch, 7, 8, 8, chunkBB);
            }
            createChest(level, chunkBB, random, 3, 3, 5, "minecraft:chests/stronghold_library");
            if (isTall) {
                placeBlock(level, CAVE_AIR, 12, 9, 1, chunkBB);
                createChest(level, chunkBB, random, 12, 8, 1, "minecraft:chests/stronghold_library");
            }
    }
};

class PortalRoom : public StrongholdPiece {
public:
    bool hasPlacedSpawner = false;
    PortalRoom(int genDepth, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shpr", genDepth, box) { setOrientation(direction); }
    static PortalRoom* createPiece(PieceAccessor& accessor, int footX, int footY, int footZ, const Direction& direction, int genDepth) {
        BoundingBox box = orientBox(footX, footY, footZ, -4, -1, 0, 11, 8, 16, direction);
        if (!isOkBox(box) || accessor.findCollisionPiece(box)) return nullptr;
        return new PortalRoom(genDepth, box, direction);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
        if (startPiece) static_cast<StartPiece*>(startPiece)->portalRoomPiece = this;
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            BlockPos pos;
            generateBox(level, chunkBB, 0, 0, 0, 10, 7, 15, false, random, SMOOTH_STONE_SELECTOR);
            generateSmallDoor(level, random, chunkBB, GRATES, 4, 1, 0);
            int y = 6;
            generateBox(level, chunkBB, 1, 6, 1, 1, 6, 14, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 9, 6, 1, 9, 6, 14, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 2, 6, 1, 8, 6, 2, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 2, 6, 14, 8, 6, 14, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 1, 1, 1, 2, 1, 4, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 8, 1, 1, 9, 1, 4, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 1, 1, 1, 1, 1, 3, blk("minecraft:lava"), blk("minecraft:lava"), false);
            generateBox(level, chunkBB, 9, 1, 1, 9, 1, 3, blk("minecraft:lava"), blk("minecraft:lava"), false);
            generateBox(level, chunkBB, 3, 1, 8, 7, 1, 12, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 4, 1, 9, 6, 1, 11, blk("minecraft:lava"), blk("minecraft:lava"), false);
            BS nsBars = (blk("minecraft:iron_bars").setValue("north", true)).setValue("south", true);
            BS weBars = (blk("minecraft:iron_bars").setValue("west", true)).setValue("east", true);
            for (int z = 3; z < 14; z += 2) {
                generateBox(level, chunkBB, 0, 3, z, 0, 4, z, nsBars, nsBars, false);
                generateBox(level, chunkBB, 10, 3, z, 10, 4, z, nsBars, nsBars, false);
            }
            for (int x = 2; x < 9; x += 2) {
                generateBox(level, chunkBB, x, 3, 15, x, 4, 15, weBars, weBars, false);
            }
            BS blockState = blk("minecraft:stone_brick_stairs").setValue("facing", Direction::NORTH);
            generateBox(level, chunkBB, 4, 1, 5, 6, 1, 7, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 4, 2, 6, 6, 2, 7, false, random, SMOOTH_STONE_SELECTOR);
            generateBox(level, chunkBB, 4, 3, 7, 6, 3, 7, false, random, SMOOTH_STONE_SELECTOR);
            for (int x = 4; x <= 6; ++x) {
                placeBlock(level, blockState, x, 1, 4, chunkBB);
                placeBlock(level, blockState, x, 2, 5, chunkBB);
                placeBlock(level, blockState, x, 3, 6, chunkBB);
            }
            BS northFrame = blk("minecraft:end_portal_frame").setValue("facing", Direction::NORTH);
            BS southFrame = blk("minecraft:end_portal_frame").setValue("facing", Direction::SOUTH);
            BS eastFrame = blk("minecraft:end_portal_frame").setValue("facing", Direction::EAST);
            BS westFrame = blk("minecraft:end_portal_frame").setValue("facing", Direction::WEST);
            bool allEyes = true;
            bool eyes[12];
            for (int i = 0; i < 12; ++i) {
                eyes[i] = random.nextFloat() > 0.9f;
                allEyes &= eyes[i];
            }
            placeBlock(level, northFrame.setValue("has_eye", eyes[0]), 4, 3, 8, chunkBB);
            placeBlock(level, northFrame.setValue("has_eye", eyes[1]), 5, 3, 8, chunkBB);
            placeBlock(level, northFrame.setValue("has_eye", eyes[2]), 6, 3, 8, chunkBB);
            placeBlock(level, southFrame.setValue("has_eye", eyes[3]), 4, 3, 12, chunkBB);
            placeBlock(level, southFrame.setValue("has_eye", eyes[4]), 5, 3, 12, chunkBB);
            placeBlock(level, southFrame.setValue("has_eye", eyes[5]), 6, 3, 12, chunkBB);
            placeBlock(level, eastFrame.setValue("has_eye", eyes[6]), 3, 3, 9, chunkBB);
            placeBlock(level, eastFrame.setValue("has_eye", eyes[7]), 3, 3, 10, chunkBB);
            placeBlock(level, eastFrame.setValue("has_eye", eyes[8]), 3, 3, 11, chunkBB);
            placeBlock(level, westFrame.setValue("has_eye", eyes[9]), 7, 3, 9, chunkBB);
            placeBlock(level, westFrame.setValue("has_eye", eyes[10]), 7, 3, 10, chunkBB);
            placeBlock(level, westFrame.setValue("has_eye", eyes[11]), 7, 3, 11, chunkBB);
            if (allEyes) {
                BS portal = blk("minecraft:end_portal");
                placeBlock(level, portal, 4, 3, 9, chunkBB);
                placeBlock(level, portal, 5, 3, 9, chunkBB);
                placeBlock(level, portal, 6, 3, 9, chunkBB);
                placeBlock(level, portal, 4, 3, 10, chunkBB);
                placeBlock(level, portal, 5, 3, 10, chunkBB);
                placeBlock(level, portal, 6, 3, 10, chunkBB);
                placeBlock(level, portal, 4, 3, 11, chunkBB);
                placeBlock(level, portal, 5, 3, 11, chunkBB);
                placeBlock(level, portal, 6, 3, 11, chunkBB);
            }
            if (!hasPlacedSpawner && chunkBB.inside(pos = getWorldPos(5, 3, 6))) {
                hasPlacedSpawner = true;
                level.setBlock(pos, blk("minecraft:spawner"), 2);
                setSpawnerEntity(level, pos, "minecraft:silverfish");
            }
    }
};

class FillerCorridor : public StrongholdPiece {
public:
    int steps;
    FillerCorridor(int genDepth, const BoundingBox& box, const Direction& direction) : StrongholdPiece("minecraft:shfc", genDepth, box) {
        setOrientation(direction);
        steps = direction == Direction::NORTH || direction == Direction::SOUTH ? box.z_span() : box.x_span();
    }
    static bool findPieceBox(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, BoundingBox& out) {
        BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 4, direction);
        StructurePiece* collisionPiece = accessor.findCollisionPiece(box);
        if (!collisionPiece) return false;
        if (collisionPiece->bb.y0 == box.y0) {
            for (int depth = 2; depth >= 1; --depth) {
                box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, depth, direction);
                if (collisionPiece->bb.intersects(box)) continue;
                out = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, depth + 1, direction);
                return true;
            }
        }
        return false;
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            for (int i = 0; i < steps; ++i) {
                placeBlock(level, blk("minecraft:stone_bricks"), 0, 0, i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 1, 0, i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 2, 0, i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 3, 0, i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 4, 0, i, chunkBB);
                for (int y = 1; y <= 3; ++y) {
                    placeBlock(level, blk("minecraft:stone_bricks"), 0, y, i, chunkBB);
                    placeBlock(level, blk("minecraft:cave_air"), 1, y, i, chunkBB);
                    placeBlock(level, blk("minecraft:cave_air"), 2, y, i, chunkBB);
                    placeBlock(level, blk("minecraft:cave_air"), 3, y, i, chunkBB);
                    placeBlock(level, blk("minecraft:stone_bricks"), 4, y, i, chunkBB);
                }
                placeBlock(level, blk("minecraft:stone_bricks"), 0, 4, i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 1, 4, i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 2, 4, i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 3, 4, i, chunkBB);
                placeBlock(level, blk("minecraft:stone_bricks"), 4, 4, i, chunkBB);
            }
    }
};

// ---- the shared child generators and the dispatch

StrongholdPiece* StrongholdPiece::generateSmallDoorChildForward(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int xOff, int yOff) {
    auto orientation = getOrientation();
    if (orientation) {
        switch (orientation->v) {
            case pn::vg::NORTH: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + xOff, bb.y0 + yOff, bb.z0 - 1, *orientation, getGenDepth());
            case pn::vg::SOUTH: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + xOff, bb.y0 + yOff, bb.z1 + 1, *orientation, getGenDepth());
            case pn::vg::WEST: return generateAndAddPiece(startPiece, accessor, random, bb.x0 - 1, bb.y0 + yOff, bb.z0 + xOff, *orientation, getGenDepth());
            case pn::vg::EAST: return generateAndAddPiece(startPiece, accessor, random, bb.x1 + 1, bb.y0 + yOff, bb.z0 + xOff, *orientation, getGenDepth());
        }
    }
    return nullptr;
}

StrongholdPiece* StrongholdPiece::generateSmallDoorChildLeft(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int yOff, int zOff) {
    auto orientation = getOrientation();
    if (orientation) {
        switch (orientation->v) {
            case pn::vg::NORTH: return generateAndAddPiece(startPiece, accessor, random, bb.x0 - 1, bb.y0 + yOff, bb.z0 + zOff, Direction::WEST, getGenDepth());
            case pn::vg::SOUTH: return generateAndAddPiece(startPiece, accessor, random, bb.x0 - 1, bb.y0 + yOff, bb.z0 + zOff, Direction::WEST, getGenDepth());
            case pn::vg::WEST: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + zOff, bb.y0 + yOff, bb.z0 - 1, Direction::NORTH, getGenDepth());
            case pn::vg::EAST: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + zOff, bb.y0 + yOff, bb.z0 - 1, Direction::NORTH, getGenDepth());
        }
    }
    return nullptr;
}

StrongholdPiece* StrongholdPiece::generateSmallDoorChildRight(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int yOff, int zOff) {
    auto orientation = getOrientation();
    if (orientation) {
        switch (orientation->v) {
            case pn::vg::NORTH: return generateAndAddPiece(startPiece, accessor, random, bb.x1 + 1, bb.y0 + yOff, bb.z0 + zOff, Direction::EAST, getGenDepth());
            case pn::vg::SOUTH: return generateAndAddPiece(startPiece, accessor, random, bb.x1 + 1, bb.y0 + yOff, bb.z0 + zOff, Direction::EAST, getGenDepth());
            case pn::vg::WEST: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + zOff, bb.y0 + yOff, bb.z1 + 1, Direction::SOUTH, getGenDepth());
            case pn::vg::EAST: return generateAndAddPiece(startPiece, accessor, random, bb.x0 + zOff, bb.y0 + yOff, bb.z1 + 1, Direction::SOUTH, getGenDepth());
        }
    }
    return nullptr;
}

void Straight::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {

            generateSmallDoorChildForward(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
            if (leftChild) {
                generateSmallDoorChildLeft(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 2);
            }
            if (rightChild) {
                generateSmallDoorChildRight(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 2);
            }
}
void PrisonHall::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {

            generateSmallDoorChildForward(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
}
void LeftTurn::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {

            Direction orientation = *getOrientation();
            if (orientation == Direction::NORTH || orientation == Direction::EAST) {
                generateSmallDoorChildLeft(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
            } else {
                generateSmallDoorChildRight(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
            }
}
void RightTurn::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {

            Direction orientation = *getOrientation();
            if (orientation == Direction::NORTH || orientation == Direction::EAST) {
                generateSmallDoorChildRight(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
            } else {
                generateSmallDoorChildLeft(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
            }
}
void RoomCrossing::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {

            generateSmallDoorChildForward(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 4, 1);
            generateSmallDoorChildLeft(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 4);
            generateSmallDoorChildRight(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 4);
}
void StraightStairsDown::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {

            generateSmallDoorChildForward(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
}
void StairsDown::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {
    if (isSource) g_state.imposedPiece = K_FIVE_CROSSING;
    generateSmallDoorChildForward(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
}
void FiveCrossing::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {

            int zOffA = 3;
            int zOffB = 5;
            Direction orientation = *getOrientation();
            if (orientation == Direction::WEST || orientation == Direction::NORTH) {
                zOffA = 8 - zOffA;
                zOffB = 8 - zOffB;
            }
            generateSmallDoorChildForward(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 5, 1);
            if (leftLow) {
                generateSmallDoorChildLeft(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, zOffA, 1);
            }
            if (leftHigh) {
                generateSmallDoorChildLeft(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, zOffB, 7);
            }
            if (rightLow) {
                generateSmallDoorChildRight(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, zOffA, 1);
            }
            if (rightHigh) {
                generateSmallDoorChildRight(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, zOffB, 7);
            }
}
void ChestCorridor::addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) {

            generateSmallDoorChildForward(static_cast<StartPiece*>(startPiece), structurePieceAccessor, random, 1, 1);
}

StrongholdPiece* findAndCreatePieceFactory(PieceKind kind, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth) {
    switch (kind) {
        case K_STRAIGHT: return Straight::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_PRISON_HALL: return PrisonHall::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_LEFT_TURN: return LeftTurn::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_RIGHT_TURN: return RightTurn::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_ROOM_CROSSING: return RoomCrossing::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_STRAIGHT_STAIRS_DOWN: return StraightStairsDown::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_STAIRS_DOWN: return StairsDown::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_FIVE_CROSSING: return FiveCrossing::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_CHEST_CORRIDOR: return ChestCorridor::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_LIBRARY: return Library::createPiece(accessor, random, footX, footY, footZ, direction, depth);
        case K_PORTAL_ROOM: return PortalRoom::createPiece(accessor, footX, footY, footZ, direction, depth);
        default: return nullptr;
    }
}

StrongholdPiece* generatePieceFromSmallDoor(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth) {
    if (!updatePieceWeight()) return nullptr;
    if (g_state.imposedPiece != K_NONE) {
        StrongholdPiece* p = findAndCreatePieceFactory(g_state.imposedPiece, accessor, random, footX, footY, footZ, direction, depth);
        g_state.imposedPiece = K_NONE;
        if (p) return p;
    }
    int numAttempts = 0;
    while (numAttempts < 5) {
        ++numAttempts;
        int weightSelection = random.nextInt(g_state.totalWeight);
        bool restart = false;
        for (size_t idx = 0; idx < g_state.currentPieces.size(); idx++) {
            PieceWeight& piece = g_state.currentPieces[idx];
            if ((weightSelection -= piece.weight) >= 0) continue;
            if (!piece.doPlace(depth) || int(piece.kind) == startPiece->previousPiece) { restart = true; break; }
            StrongholdPiece* p = findAndCreatePieceFactory(piece.kind, accessor, random, footX, footY, footZ, direction, depth);
            if (!p) continue;
            ++piece.placeCount;
            startPiece->previousPiece = int(piece.kind);
            if (!piece.isValid()) g_state.currentPieces.erase(g_state.currentPieces.begin() + long(idx));
            return p;
        }
        if (restart) continue;
    }
    BoundingBox box;
    if (FillerCorridor::findPieceBox(accessor, random, footX, footY, footZ, direction, box) && box.y0 > 1) return new FillerCorridor(depth, box, direction);
    return nullptr;
}

StrongholdPiece* generateAndAddPiece(StartPiece* startPiece, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth) {
    if (depth > 50) return nullptr;
    if (std::abs(footX - startPiece->bb.x0) > 112 || std::abs(footZ - startPiece->bb.z0) > 112) return nullptr;
    StrongholdPiece* newPiece = generatePieceFromSmallDoor(startPiece, accessor, random, footX, footY, footZ, direction, depth + 1);
    if (newPiece) {
        accessor.addPiece(std::shared_ptr<StructurePiece>(newPiece));
        startPiece->pendingChildren.push_back(newPiece);
    }
    return newPiece;
}

class StrongholdStructure : public Structure {
public:
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        GenerationStub stub;
        stub.position = BlockPos(ctx.chunk.x * 16, 0, ctx.chunk.z * 16);
        stub.generate = [&ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
            std::shared_ptr<StartPiece> startRoom;
            int tries = 0;
            do {
                out.clear();
                PieceAccessor builder(out);
                ctx.random.set_large_feature_seed(ctx.env.seed + int64_t(tries++), ctx.chunk.x, ctx.chunk.z);
                resetPieces();
                JRandom random(ctx.random);
                startRoom = std::make_shared<StartPiece>(random, ctx.chunk.x * 16 + 2, ctx.chunk.z * 16 + 2);
                builder.addPiece(startRoom);
                startRoom->addChildren(startRoom.get(), builder, random);
                std::vector<StrongholdPiece*>& pendingChildren = startRoom->pendingChildren;
                while (!pendingChildren.empty()) {
                    int pos = random.nextInt(int(pendingChildren.size()));
                    StrongholdPiece* piece = pendingChildren[size_t(pos)];
                    pendingChildren.erase(pendingChildren.begin() + pos);
                    piece->addChildren(startRoom.get(), builder, random);
                }
                builder.moveBelowSeaLevel(ctx.env.sea_level, ctx.env.min_y, ctx.random, 10);
            } while (out.empty() || startRoom->portalRoomPiece == nullptr);
        };
        return stub;
    }
};

struct RegisterStronghold {
    RegisterStronghold() {
        structure_factories()["minecraft:stronghold"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<StrongholdStructure>(); };
    }
} register_stronghold;

}  // namespace

}  // namespace pn::vg
