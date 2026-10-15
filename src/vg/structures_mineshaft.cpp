// Mineshafts (MineshaftStructure and MineshaftPieces): rooms, corridors, crossings and stairs, grown recursively from a room, with rails, supports,
// cobwebs, spider spawners and chest minecarts.
#include "legacy_piece.hpp"
#include <algorithm>

namespace pn::vg {

using namespace jc;

namespace {

struct MineshaftType {
    const char* wood;
    const char* planks;
    const char* fence;
    int ordinal;
    BS getWoodState() const { return blk(wood); }
    BS getPlanksState() const { return blk(planks); }
    BS getFenceState() const { return blk(fence); }
};
const MineshaftType kNormal{"minecraft:oak_log", "minecraft:oak_planks", "minecraft:oak_fence", 0};
const MineshaftType kMesa{"minecraft:dark_oak_log", "minecraft:dark_oak_planks", "minecraft:dark_oak_fence", 1};

BS CAVE_AIR() { return blk("minecraft:cave_air"); }
#define CAVE_AIR CAVE_AIR()

class MineShaftPiece : public LegacyPiece {
public:
    const MineshaftType* mtype = &kNormal;
protected:
    MineShaftPiece(const char* id, int genDepth, const MineshaftType* t, const BoundingBox& box) {
        type = id;
        gen_depth = genDepth;
        bb = box;
        mtype = t;
    }
    bool canBeReplaced(JLevel& level, int x, int y, int z, const BoundingBox& chunkBB) override {

            BS state = getBlock(level, x, y, z, chunkBB);
            return !state.is(mtype->getPlanksState().getBlock()) && !state.is(mtype->getWoodState().getBlock()) && !state.is(mtype->getFenceState().getBlock()) && !state.is(BlockRef("minecraft:iron_chain"));
    }
    bool isSupportingBox(JLevel& level, const BoundingBox& chunkBB, int x0, int x1, int y1, int z0) {

            for (int x = x0; x <= x1; ++x) {
                if (!getBlock(level, x, y1 + 1, z0, chunkBB).isAir()) continue;
                return false;
            }
            return true;
    }
    bool isInInvalidLocation(JLevel& level, const BoundingBox& chunkBB) {

            int y;
            int x;
            int z1;
            int y1;
            int x0 = std::max(bb.x0 - 1, chunkBB.x0);
            int y0 = std::max(bb.y0 - 1, chunkBB.y0);
            int z0 = std::max(bb.z0 - 1, chunkBB.z0);
            int x1 = std::min(bb.x1 + 1, chunkBB.x1);
            BlockPos blockPos = BlockPos((x0 + x1) / 2, (y0 + (y1 = std::min(bb.y1 + 1, chunkBB.y1))) / 2, (z0 + (z1 = std::min(bb.z1 + 1, chunkBB.z1))) / 2);
            if (level.biomeIs(blockPos,"#minecraft:mineshaft_blocking")) {
                return true;
            }
            for (x = x0; x <= x1; ++x) {
                for (int z = z0; z <= z1; ++z) {
                    if (level.getBlockState((blockPos=BlockPos(x, y0, z))).liquid()) {
                        return true;
                    }
                    if (!level.getBlockState((blockPos=BlockPos(x, y1, z))).liquid()) continue;
                    return true;
                }
            }
            for (x = x0; x <= x1; ++x) {
                for (y = y0; y <= y1; ++y) {
                    if (level.getBlockState((blockPos=BlockPos(x, y, z0))).liquid()) {
                        return true;
                    }
                    if (!level.getBlockState((blockPos=BlockPos(x, y, z1))).liquid()) continue;
                    return true;
                }
            }
            for (int z = z0; z <= z1; ++z) {
                for (y = y0; y <= y1; ++y) {
                    if (level.getBlockState((blockPos=BlockPos(x0, y, z))).liquid()) {
                        return true;
                    }
                    if (!level.getBlockState((blockPos=BlockPos(x1, y, z))).liquid()) continue;
                    return true;
                }
            }
            return false;
    }
    void setPlanksBlock(JLevel& level, const BoundingBox& chunkBB, const BS& planksBlock, int x, int y, int z) {

            if (!isInterior(level, x, y, z, chunkBB)) {
                return;
            }
            BlockPos pos = getWorldPos(x, y, z);
            BS existingState = level.getBlockState(pos);
            if (!existingState.isFaceSturdy(level, pos, Direction::UP)) {
                level.setBlock(pos, planksBlock, 2);
            }
    }
};

MineShaftPiece* generateAndAddPiece(StructurePiece* startPiece, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth);

class MineShaftRoom : public MineShaftPiece {
public:
    std::vector<BoundingBox> childEntranceBoxes;
    MineShaftRoom(int genDepth, JRandom random, int west, int north, const MineshaftType* t)
        : MineShaftPiece("minecraft:msroom", genDepth, t, mk(random, west, north)) {}
    static BoundingBox mk(JRandom random, int west, int north) {
        int a = random.nextInt(6), b = random.nextInt(6), c = random.nextInt(6);
        // new BoundingBox(west, 50, north, west + 7 + nextInt(6), 54 + nextInt(6), north + 7 + nextInt(6)): arguments are evaluated in this order
        return BoundingBox(west, 50, north, west + 7 + a, 54 + b, north + 7 + c);
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {
        MineShaftPiece* child;
        BoundingBox childBox;
        int pos;

                                                int depth = getGenDepth();
            int heightSpace = bb.y_span() - 3 - 1;
            if (heightSpace <= 0) {
                heightSpace = 1;
            }
            for (pos = 0; pos < bb.x_span() && (pos += random.nextInt(bb.x_span())) + 3 <= bb.x_span(); pos += 4) {
                child = generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + pos, bb.y0 + random.nextInt(heightSpace) + 1, bb.z0 - 1, Direction::NORTH, depth);
                if (!child) continue;
                childBox = child->bb;
                childEntranceBoxes.push_back(BoundingBox(childBox.x0, childBox.y0, bb.z0, childBox.x1, childBox.y1, bb.z0 + 1));
            }
            for (pos = 0; pos < bb.x_span() && (pos += random.nextInt(bb.x_span())) + 3 <= bb.x_span(); pos += 4) {
                child = generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + pos, bb.y0 + random.nextInt(heightSpace) + 1, bb.z1 + 1, Direction::SOUTH, depth);
                if (!child) continue;
                childBox = child->bb;
                childEntranceBoxes.push_back(BoundingBox(childBox.x0, childBox.y0, bb.z1 - 1, childBox.x1, childBox.y1, bb.z1));
            }
            for (pos = 0; pos < bb.z_span() && (pos += random.nextInt(bb.z_span())) + 3 <= bb.z_span(); pos += 4) {
                child = generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0 + random.nextInt(heightSpace) + 1, bb.z0 + pos, Direction::WEST, depth);
                if (!child) continue;
                childBox = child->bb;
                childEntranceBoxes.push_back(BoundingBox(bb.x0, childBox.y0, childBox.z0, bb.x0 + 1, childBox.y1, childBox.z1));
            }
            for (pos = 0; pos < bb.z_span() && (pos += random.nextInt(bb.z_span())) + 3 <= bb.z_span(); pos += 4) {
                child = generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0 + random.nextInt(heightSpace) + 1, bb.z0 + pos, Direction::EAST, depth);
                if (!child) continue;
                childBox = child->bb;
                childEntranceBoxes.push_back(BoundingBox(bb.x1 - 1, childBox.y0, childBox.z0, bb.x1, childBox.y1, childBox.z1));
            }
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            if (isInInvalidLocation(level, chunkBB)) {
                return;
            }
            generateBox(level, chunkBB, bb.x0, bb.y0 + 1, bb.z0, bb.x1, std::min(bb.y0 + 3, bb.y1), bb.z1, CAVE_AIR, CAVE_AIR, false);
            for (BoundingBox entranceBox : childEntranceBoxes) {
                generateBox(level, chunkBB, entranceBox.x0, entranceBox.y1 - 2, entranceBox.z0, entranceBox.x1, entranceBox.y1, entranceBox.z1, CAVE_AIR, CAVE_AIR, false);
            }
            generateUpperHalfSphere(level, chunkBB, bb.x0, bb.y0 + 4, bb.z0, bb.x1, bb.y1, bb.z1, CAVE_AIR, false);
    }
    void move(int dx, int dy, int dz) override {
        MineShaftPiece::move(dx, dy, dz);
        for (BoundingBox& b : childEntranceBoxes) b.move(dx, dy, dz);
    }
};

class MineShaftCorridor : public MineShaftPiece {
public:
    bool hasRails, spiderCorridor, hasPlacedSpider = false;
    int numSections;
    MineShaftCorridor(int genDepth, JRandom random, const BoundingBox& box, const Direction& direction, const MineshaftType* t) : MineShaftPiece("minecraft:mscorridor", genDepth, t, box) {
        setOrientation(direction);
        hasRails = random.nextInt(3) == 0;
        spiderCorridor = !hasRails && random.nextInt(23) == 0;
        numSections = direction.axis() == 'z' ? box.z_span() / 5 : box.x_span() / 5;
    }
    static BoundingBox* findCorridorSize(PieceAccessor& structurePieceAccessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, BoundingBox& out) {
        int corridorLength = random.nextInt(3) + 2;
        while (corridorLength > 0) {
            int blockLength = corridorLength * 5;
            BoundingBox box;
            switch (direction.v) {
                case pn::vg::SOUTH: box = BoundingBox(0, 0, 0, 2, 2, blockLength - 1); break;
                case pn::vg::WEST: box = BoundingBox(-(blockLength - 1), 0, 0, 0, 2, 2); break;
                case pn::vg::EAST: box = BoundingBox(0, 0, 0, blockLength - 1, 2, 2); break;
                default: box = BoundingBox(0, 0, -(blockLength - 1), 2, 2, 0); break;
            }
            box.move(footX, footY, footZ);
            if (!structurePieceAccessor.findCollisionPiece(box)) { out = box; return &out; }
            --corridorLength;
        }
        return nullptr;
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {

             {
                int depth = getGenDepth();
                int endSelection = random.nextInt(4);
                Direction orientation = *getOrientation();
                {
                    switch (orientation.v) {
                        default: {
                            if (endSelection <= 1) {
                                generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0, bb.y0 - 1 + random.nextInt(3), bb.z0 - 1, orientation, depth);
                                break;
                            }
                            if (endSelection == 2) {
                                generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0 - 1 + random.nextInt(3), bb.z0, Direction::WEST, depth);
                                break;
                            }
                            generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0 - 1 + random.nextInt(3), bb.z0, Direction::EAST, depth);
                            break;
                        }
                        case pn::vg::SOUTH: {
                            if (endSelection <= 1) {
                                generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0, bb.y0 - 1 + random.nextInt(3), bb.z1 + 1, orientation, depth);
                                break;
                            }
                            if (endSelection == 2) {
                                generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0 - 1 + random.nextInt(3), bb.z1 - 3, Direction::WEST, depth);
                                break;
                            }
                            generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0 - 1 + random.nextInt(3), bb.z1 - 3, Direction::EAST, depth);
                            break;
                        }
                        case pn::vg::WEST: {
                            if (endSelection <= 1) {
                                generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0 - 1 + random.nextInt(3), bb.z0, orientation, depth);
                                break;
                            }
                            if (endSelection == 2) {
                                generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0, bb.y0 - 1 + random.nextInt(3), bb.z0 - 1, Direction::NORTH, depth);
                                break;
                            }
                            generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0, bb.y0 - 1 + random.nextInt(3), bb.z1 + 1, Direction::SOUTH, depth);
                            break;
                        }
                        case pn::vg::EAST: {
                            if (endSelection <= 1) {
                                generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0 - 1 + random.nextInt(3), bb.z0, orientation, depth);
                                break;
                            }
                            if (endSelection == 2) {
                                generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 - 3, bb.y0 - 1 + random.nextInt(3), bb.z0 - 1, Direction::NORTH, depth);
                                break;
                            }
                            generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 - 3, bb.y0 - 1 + random.nextInt(3), bb.z1 + 1, Direction::SOUTH, depth);
                        }
                    }
                }
                if (depth >= 8) return;
                if ((orientation == Direction::NORTH || orientation == Direction::SOUTH)) {
                    int z = bb.z0 + 3;
                    while (z + 3 <= bb.z1) {
                        int selection = random.nextInt(5);
                        if (selection == 0) {
                            generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0, z, Direction::WEST, depth + 1);
                        } else if (selection == 1) {
                            generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0, z, Direction::EAST, depth + 1);
                        }
                        z += 5;
                    }
                } else {
                    int x = bb.x0 + 3;
                    while (x + 3 <= bb.x1) {
                        int selection = random.nextInt(5);
                        if (selection == 0) {
                            generateAndAddPiece(startPiece, structurePieceAccessor, random, x, bb.y0, bb.z0 - 1, Direction::NORTH, depth + 1);
                        } else if (selection == 1) {
                            generateAndAddPiece(startPiece, structurePieceAccessor, random, x, bb.y0, bb.z1 + 1, Direction::SOUTH, depth + 1);
                        }
                        x += 5;
                    }
                }
            }
    }
    bool createChest(JLevel& level, const BoundingBox& chunkBB, JRandom random, int x, int y, int z, const std::string& lootTable) override {

            BlockPos pos = getWorldPos(x, y, z);
            if (chunkBB.inside(pos) && level.getBlockState(pos).isAir() && !level.getBlockState(((BlockPos)pos).below()).isAir()) {
                BS state = blk("minecraft:rail").setValue("shape", random.nextBoolean() ? "north_south" : "east_west");
                placeBlock(level, state, x, y, z, chunkBB);
                {   // a chest minecart on the rail
                    pn::Nbt e = pn::Nbt::compound();
                    e.put("id", pn::Nbt::str("minecraft:chest_minecart"));
                    pn::Nbt p = pn::Nbt::list_of(pn::Nbt::Double);
                    for (double v : {double(pos.x) + 0.5, double(pos.y) + 0.5, double(pos.z) + 0.5}) { pn::Nbt d; d.type = pn::Nbt::Double; d.f = v; p.list.push_back(d); }
                    e.put("Pos", p);
                    e.put("LootTable", pn::Nbt::str(lootTable));
                    e.put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, random.nextLong()));
                    level.L.add_entity(double(pos.x) + 0.5, double(pos.z) + 0.5, std::move(e));
                }
                return true;
            }
            return false;
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            int z;
            if (isInInvalidLocation(level, chunkBB)) {
                return;
            }
            bool x0 = false;
            int x1 = 2;
            bool y0 = false;
            int y1 = 2;
            int length = numSections * 5 - 1;
            BS planks = mtype->getPlanksState();
            generateBox(level, chunkBB, 0, 0, 0, 2, 1, length, CAVE_AIR, CAVE_AIR, false);
            generateMaybeBox(level, chunkBB, random, 0.8f, 0, 2, 0, 2, 2, length, CAVE_AIR, CAVE_AIR, false, false);
            if (spiderCorridor) {
                generateMaybeBox(level, chunkBB, random, 0.6f, 0, 0, 0, 2, 1, length, blk("minecraft:cobweb"), CAVE_AIR, false, true);
            }
            for (int section = 0; section < numSections; ++section) {
                z = 2 + section * 5;
                placeSupport(level, chunkBB, 0, 0, z, 2, 2, random);
                maybePlaceCobWeb(level, chunkBB, random, 0.1f, 0, 2, z - 1);
                maybePlaceCobWeb(level, chunkBB, random, 0.1f, 2, 2, z - 1);
                maybePlaceCobWeb(level, chunkBB, random, 0.1f, 0, 2, z + 1);
                maybePlaceCobWeb(level, chunkBB, random, 0.1f, 2, 2, z + 1);
                maybePlaceCobWeb(level, chunkBB, random, 0.05f, 0, 2, z - 2);
                maybePlaceCobWeb(level, chunkBB, random, 0.05f, 2, 2, z - 2);
                maybePlaceCobWeb(level, chunkBB, random, 0.05f, 0, 2, z + 2);
                maybePlaceCobWeb(level, chunkBB, random, 0.05f, 2, 2, z + 2);
                if (random.nextInt(100) == 0) {
                    createChest(level, chunkBB, random, 2, 0, z - 1, "minecraft:chests/abandoned_mineshaft");
                }
                if (random.nextInt(100) == 0) {
                    createChest(level, chunkBB, random, 0, 0, z + 1, "minecraft:chests/abandoned_mineshaft");
                }
                if (!spiderCorridor || hasPlacedSpider) continue;
                bool newX = true;
                int newZ = z - 1 + random.nextInt(3);
                BlockPos pos = getWorldPos(1, 0, newZ);
                if (!chunkBB.inside(pos) || !isInterior(level, 1, 0, newZ, chunkBB)) continue;
                hasPlacedSpider = true;
                level.setBlock(pos, blk("minecraft:spawner"), 2);
                {   // SpawnerBlockEntity.setEntityId(CAVE_SPIDER)
                    pn::Nbt be = pn::Nbt::compound();
                    be.put("id", pn::Nbt::str("minecraft:mob_spawner"));
                    be.put("x", pn::Nbt::make(pn::Nbt::Int, pos.x));
                    be.put("y", pn::Nbt::make(pn::Nbt::Int, pos.y));
                    be.put("z", pn::Nbt::make(pn::Nbt::Int, pos.z));
                    pn::Nbt entity = pn::Nbt::compound();
                    entity.put("id", pn::Nbt::str("minecraft:cave_spider"));
                    pn::Nbt sd = pn::Nbt::compound();
                    sd.put("entity", entity);
                    be.put("SpawnData", sd);
                    level.L.set_block_entity(pos, std::move(be));
                }
            }
            for (int x = 0; x <= 2; ++x) {
                for (z = 0; z <= length; ++z) {
                    setPlanksBlock(level, chunkBB, planks, x, -1, z);
                }
            }
            int supportPillarIndent = 2;
            placeDoubleLowerOrUpperSupport(level, chunkBB, 0, -1, 2);
            if (numSections > 1) {
                int lastSupportPillar = length - 2;
                placeDoubleLowerOrUpperSupport(level, chunkBB, 0, -1, lastSupportPillar);
            }
            if (hasRails) {
                BS state = blk("minecraft:rail").setValue("shape", "north_south");
                for (int z2 = 0; z2 <= length; ++z2) {
                    BS floor = getBlock(level, 1, -1, z2, chunkBB);
                    if (floor.isAir() || !floor.isSolidRender()) continue;
                    float probability = isInterior(level, 1, 0, z2, chunkBB) ? 0.7f : 0.9f;
                    maybeGenerateBlock(level, chunkBB, random, probability, 1, 0, z2, state);
                }
            }
    }
    void placeDoubleLowerOrUpperSupport(JLevel& level, const BoundingBox& chunkBB, int x, int y, int z) {

            BS woodBlock = mtype->getWoodState();
            BS plankBlock = mtype->getPlanksState();
            if (getBlock(level, x, y, z, chunkBB).is(plankBlock.getBlock())) {
                fillPillarDownOrChainUp(level, woodBlock, x, y, z, chunkBB);
            }
            if (getBlock(level, x + 2, y, z, chunkBB).is(plankBlock.getBlock())) {
                fillPillarDownOrChainUp(level, woodBlock, x + 2, y, z, chunkBB);
            }
    }
    void fillColumnDown(JLevel& level, const BS& columnState, int x, int startY, int z, const BoundingBox& chunkBB) override {

            BlockPos pos = getWorldPos(x, startY, z);
            if (!chunkBB.inside(pos)) {
                return;
            }
            int worldY = pos.y;
            while (isReplaceableByStructures(level.getBlockState(pos)) && pos.y > level.getMinY() + 1) {
                pos = pos.below();
            }
            if (!canPlaceColumnOnTopOf(level, pos, level.getBlockState(pos))) {
                return;
            }
            while (pos.y < worldY) {
                pos = pos.above();
                level.setBlock(pos, columnState, 2);
            }
    }
    void fillPillarDownOrChainUp(JLevel& level, const BS& pillarState, int x, int y, int z, const BoundingBox& chunkBB) {

            BlockPos pos = getWorldPos(x, y, z);
            if (!chunkBB.inside(pos)) {
                return;
            }
            int worldY = pos.y;
            int distanceFromWorldY = 1;
            bool checkBelow = true;
            bool checkAbove = true;
            while (checkBelow || checkAbove) {
                if (checkBelow) {
                    bool emptyBelow;
                    pos = BlockPos(pos.x, worldY - distanceFromWorldY, pos.z);
                    BS belowState = level.getBlockState(pos);
                    bool bl = emptyBelow = isReplaceableByStructures(belowState) && !belowState.is(BlockRef("minecraft:lava"));
                    if (!emptyBelow && canPlaceColumnOnTopOf(level, pos, belowState)) {
                        fillColumnBetween(level, pillarState, pos, worldY - distanceFromWorldY + 1, worldY);
                        return;
                    }
                    bool bl2 = checkBelow = distanceFromWorldY <= 20 && emptyBelow && pos.y > level.getMinY() + 1;
                }
                if (checkAbove) {
                    pos = BlockPos(pos.x, worldY + distanceFromWorldY, pos.z);
                    BS aboveState = level.getBlockState(pos);
                    bool emptyAbove = isReplaceableByStructures(aboveState);
                    if (!emptyAbove && canHangChainBelow(level, pos, aboveState)) {
                        level.setBlock(BlockPos(pos.x, worldY + 1, pos.z), mtype->getFenceState(), 2);
                        fillColumnBetween(level, blk("minecraft:iron_chain"), pos, worldY + 2, worldY + distanceFromWorldY);
                        return;
                    }
                    checkAbove = distanceFromWorldY <= 50 && emptyAbove && pos.y < level.getMaxY();
                }
                ++distanceFromWorldY;
            }
    }
    static void fillColumnBetween(JLevel& level, const BS& pillarState, BlockPos pos, int bottomInclusive, int topExclusive) {
        for (int pillarY = bottomInclusive; pillarY < topExclusive; ++pillarY) level.setBlock(BlockPos(pos.x, pillarY, pos.z), pillarState, 2);
    }
    bool canPlaceColumnOnTopOf(JLevel& level, const BlockPos& posBelow, const BS& stateBelow) { return stateBelow.isFaceSturdy(Direction::UP); }
    bool canHangChainBelow(JLevel& level, const BlockPos& posAbove, const BS& stateAbove) {
        // Block.canSupportCenter(level, pos, DOWN) && !(state.getBlock() instanceof FallingBlock)
        const auto& cl = jc::bd().classes(stateAbove.id);
        bool falling = std::find(cl.begin(), cl.end(), "FallingBlock") != cl.end();
        return stateAbove.isFaceSturdy(Direction::DOWN) && !falling;
    }
    void placeSupport(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z, int y1, int x1, JRandom random) {

            if (!isSupportingBox(level, chunkBB, x0, x1, y1, z)) {
                return;
            }
            BS planksBlock = mtype->getPlanksState();
            BS fenceBlock = mtype->getFenceState();
            generateBox(level, chunkBB, x0, y0, z, x0, y1 - 1, z, fenceBlock.setValue("west", true), CAVE_AIR, false);
            generateBox(level, chunkBB, x1, y0, z, x1, y1 - 1, z, fenceBlock.setValue("east", true), CAVE_AIR, false);
            if (random.nextInt(4) == 0) {
                generateBox(level, chunkBB, x0, y1, z, x0, y1, z, planksBlock, CAVE_AIR, false);
                generateBox(level, chunkBB, x1, y1, z, x1, y1, z, planksBlock, CAVE_AIR, false);
            } else {
                generateBox(level, chunkBB, x0, y1, z, x1, y1, z, planksBlock, CAVE_AIR, false);
                maybeGenerateBlock(level, chunkBB, random, 0.05f, x0 + 1, y1, z - 1, blk("minecraft:wall_torch").setValue("facing", Direction::SOUTH));
                maybeGenerateBlock(level, chunkBB, random, 0.05f, x0 + 1, y1, z + 1, blk("minecraft:wall_torch").setValue("facing", Direction::NORTH));
            }
    }
    void maybePlaceCobWeb(JLevel& level, const BoundingBox& chunkBB, JRandom random, float probability, int x, int y, int z) {

            if (isInterior(level, x, y, z, chunkBB) && random.nextFloat() < probability && hasSturdyNeighbours(level, chunkBB, x, y, z, 2)) {
                placeBlock(level, blk("minecraft:cobweb"), x, y, z, chunkBB);
            }
    }
    bool hasSturdyNeighbours(JLevel& level, const BoundingBox& chunkBB, int x, int y, int z, int count) {
        BlockPos worldPos = getWorldPos(x, y, z);
        int sturdyNeighbours = 0;
        for (int d = 0; d < 6; d++) {
            Direction direction(d);
            worldPos = BlockPos(worldPos.x + direction.getStepX(), worldPos.y + direction.getStepY(), worldPos.z + direction.getStepZ());
            if (chunkBB.inside(worldPos) && level.getBlockState(worldPos).isFaceSturdy(direction.getOpposite()) && ++sturdyNeighbours >= count) return true;
            Direction opp = direction.getOpposite();
            worldPos = BlockPos(worldPos.x + opp.getStepX(), worldPos.y + opp.getStepY(), worldPos.z + opp.getStepZ());
        }
        return false;
    }
};

class MineShaftCrossing : public MineShaftPiece {
public:
    Direction direction;
    bool isTwoFloored;
    MineShaftCrossing(int genDepth, const BoundingBox& box, const Direction* dir, const MineshaftType* t) : MineShaftPiece("minecraft:mscrossing", genDepth, t, box) {
        direction = dir ? *dir : Direction::SOUTH;
        isTwoFloored = box.y_span() > 3;
    }
    static bool findCrossing(PieceAccessor& structurePieceAccessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, BoundingBox& out) {
        int y1 = random.nextInt(4) == 0 ? 6 : 2;
        BoundingBox box;
        switch (direction.v) {
            case pn::vg::SOUTH: box = BoundingBox(-1, 0, 0, 3, y1, 4); break;
            case pn::vg::WEST: box = BoundingBox(-4, 0, -1, 0, y1, 3); break;
            case pn::vg::EAST: box = BoundingBox(0, 0, -1, 4, y1, 3); break;
            default: box = BoundingBox(-1, 0, -4, 3, y1, 0); break;
        }
        box.move(footX, footY, footZ);
        if (structurePieceAccessor.findCollisionPiece(box)) return false;
        out = box;
        return true;
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {

            int depth = getGenDepth();
            switch (direction.v) {
                default: {
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + 1, bb.y0, bb.z0 - 1, Direction::NORTH, depth);
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0, bb.z0 + 1, Direction::WEST, depth);
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0, bb.z0 + 1, Direction::EAST, depth);
                    break;
                }
                case pn::vg::SOUTH: {
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + 1, bb.y0, bb.z1 + 1, Direction::SOUTH, depth);
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0, bb.z0 + 1, Direction::WEST, depth);
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0, bb.z0 + 1, Direction::EAST, depth);
                    break;
                }
                case pn::vg::WEST: {
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + 1, bb.y0, bb.z0 - 1, Direction::NORTH, depth);
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + 1, bb.y0, bb.z1 + 1, Direction::SOUTH, depth);
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0, bb.z0 + 1, Direction::WEST, depth);
                    break;
                }
                case pn::vg::EAST: {
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + 1, bb.y0, bb.z0 - 1, Direction::NORTH, depth);
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + 1, bb.y0, bb.z1 + 1, Direction::SOUTH, depth);
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0, bb.z0 + 1, Direction::EAST, depth);
                }
            }
            if (isTwoFloored) {
                if (random.nextBoolean()) {
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + 1, bb.y0 + 3 + 1, bb.z0 - 1, Direction::NORTH, depth);
                }
                if (random.nextBoolean()) {
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0 + 3 + 1, bb.z0 + 1, Direction::WEST, depth);
                }
                if (random.nextBoolean()) {
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0 + 3 + 1, bb.z0 + 1, Direction::EAST, depth);
                }
                if (random.nextBoolean()) {
                    generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 + 1, bb.y0 + 3 + 1, bb.z1 + 1, Direction::SOUTH, depth);
                }
            }
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            if (isInInvalidLocation(level, chunkBB)) {
                return;
            }
            BS planks = mtype->getPlanksState();
            if (isTwoFloored) {
                generateBox(level, chunkBB, bb.x0 + 1, bb.y0, bb.z0, bb.x1 - 1, bb.y0 + 3 - 1, bb.z1, CAVE_AIR, CAVE_AIR, false);
                generateBox(level, chunkBB, bb.x0, bb.y0, bb.z0 + 1, bb.x1, bb.y0 + 3 - 1, bb.z1 - 1, CAVE_AIR, CAVE_AIR, false);
                generateBox(level, chunkBB, bb.x0 + 1, bb.y1 - 2, bb.z0, bb.x1 - 1, bb.y1, bb.z1, CAVE_AIR, CAVE_AIR, false);
                generateBox(level, chunkBB, bb.x0, bb.y1 - 2, bb.z0 + 1, bb.x1, bb.y1, bb.z1 - 1, CAVE_AIR, CAVE_AIR, false);
                generateBox(level, chunkBB, bb.x0 + 1, bb.y0 + 3, bb.z0 + 1, bb.x1 - 1, bb.y0 + 3, bb.z1 - 1, CAVE_AIR, CAVE_AIR, false);
            } else {
                generateBox(level, chunkBB, bb.x0 + 1, bb.y0, bb.z0, bb.x1 - 1, bb.y1, bb.z1, CAVE_AIR, CAVE_AIR, false);
                generateBox(level, chunkBB, bb.x0, bb.y0, bb.z0 + 1, bb.x1, bb.y1, bb.z1 - 1, CAVE_AIR, CAVE_AIR, false);
            }
            placeSupportPillar(level, chunkBB, bb.x0 + 1, bb.y0, bb.z0 + 1, bb.y1);
            placeSupportPillar(level, chunkBB, bb.x0 + 1, bb.y0, bb.z1 - 1, bb.y1);
            placeSupportPillar(level, chunkBB, bb.x1 - 1, bb.y0, bb.z0 + 1, bb.y1);
            placeSupportPillar(level, chunkBB, bb.x1 - 1, bb.y0, bb.z1 - 1, bb.y1);
            int y = bb.y0 - 1;
            for (int x = bb.x0; x <= bb.x1; ++x) {
                for (int z = bb.z0; z <= bb.z1; ++z) {
                    setPlanksBlock(level, chunkBB, planks, x, y, z);
                }
            }
    }
    void placeSupportPillar(JLevel& level, const BoundingBox& chunkBB, int x, int y0, int z, int y1) {

            if (!getBlock(level, x, y1 + 1, z, chunkBB).isAir()) {
                generateBox(level, chunkBB, x, y0, z, x, y1, z, mtype->getPlanksState(), CAVE_AIR, false);
            }
    }
};

class MineShaftStairs : public MineShaftPiece {
public:
    MineShaftStairs(int genDepth, const BoundingBox& box, const Direction& direction, const MineshaftType* t) : MineShaftPiece("minecraft:msstairs", genDepth, t, box) { setOrientation(direction); }
    static bool findStairs(PieceAccessor& structurePieceAccessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, BoundingBox& out) {
        BoundingBox box;
        switch (direction.v) {
            case pn::vg::SOUTH: box = BoundingBox(0, -5, 0, 2, 2, 8); break;
            case pn::vg::WEST: box = BoundingBox(-8, -5, 0, 0, 2, 2); break;
            case pn::vg::EAST: box = BoundingBox(0, -5, 0, 8, 2, 2); break;
            default: box = BoundingBox(0, -5, -8, 2, 2, 0); break;
        }
        box.move(footX, footY, footZ);
        if (structurePieceAccessor.findCollisionPiece(box)) return false;
        out = box;
        return true;
    }
    void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {

            int depth = getGenDepth();
            auto orientation_o = getOrientation();
            if (orientation_o) { Direction orientation = *orientation_o;
                switch (orientation.v) {
                    default: {
                        generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0, bb.y0, bb.z0 - 1, Direction::NORTH, depth);
                        break;
                    }
                    case pn::vg::SOUTH: {
                        generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0, bb.y0, bb.z1 + 1, Direction::SOUTH, depth);
                        break;
                    }
                    case pn::vg::WEST: {
                        generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x0 - 1, bb.y0, bb.z0, Direction::WEST, depth);
                        break;
                    }
                    case pn::vg::EAST: {
                        generateAndAddPiece(startPiece, structurePieceAccessor, random, bb.x1 + 1, bb.y0, bb.z0, Direction::EAST, depth);
                    }
                }
            }
    }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

            if (isInInvalidLocation(level, chunkBB)) {
                return;
            }
            generateBox(level, chunkBB, 0, 5, 0, 2, 7, 1, CAVE_AIR, CAVE_AIR, false);
            generateBox(level, chunkBB, 0, 0, 7, 2, 2, 8, CAVE_AIR, CAVE_AIR, false);
            for (int i = 0; i < 5; ++i) {
                generateBox(level, chunkBB, 0, 5 - i - (i < 4 ? 1 : 0), 2 + i, 2, 7 - i, 2 + i, CAVE_AIR, CAVE_AIR, false);
            }
    }
};

MineShaftPiece* createRandomShaftPiece(PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int genDepth, const MineshaftType* type) {
    int randomSelection = random.nextInt(100);
    BoundingBox box;
    if (randomSelection >= 80) {
        if (MineShaftCrossing::findCrossing(accessor, random, footX, footY, footZ, direction, box)) return new MineShaftCrossing(genDepth, box, &direction, type);
    } else if (randomSelection >= 70) {
        if (MineShaftStairs::findStairs(accessor, random, footX, footY, footZ, direction, box)) return new MineShaftStairs(genDepth, box, direction, type);
    } else {
        BoundingBox out;
        if (MineShaftCorridor::findCorridorSize(accessor, random, footX, footY, footZ, direction, out)) return new MineShaftCorridor(genDepth, random, out, direction, type);
    }
    return nullptr;
}

MineShaftPiece* generateAndAddPiece(StructurePiece* startPiece, PieceAccessor& accessor, JRandom random, int footX, int footY, int footZ, const Direction& direction, int depth) {
    if (depth > 8) return nullptr;
    if (std::abs(footX - startPiece->bb.x0) > 80 || std::abs(footZ - startPiece->bb.z0) > 80) return nullptr;
    const MineshaftType* type = static_cast<MineShaftPiece*>(startPiece)->mtype;
    MineShaftPiece* newPiece = createRandomShaftPiece(accessor, random, footX, footY, footZ, direction, depth + 1, type);
    if (newPiece) {
        accessor.addPiece(std::shared_ptr<StructurePiece>(newPiece));
        newPiece->addChildren(startPiece, accessor, random);
    }
    return newPiece;
}

class MineshaftStructure : public Structure {
public:
    const MineshaftType* mtype = &kNormal;
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        ctx.random.next_double();
        BlockPos startPos(ctx.chunk.x * 16 + 8, 50, ctx.chunk.z * 16);
        std::vector<std::shared_ptr<StructurePiece>> pieces;
        PieceAccessor builder(pieces);
        JRandom random(ctx.random);
        auto room = std::make_shared<MineShaftRoom>(0, random, ctx.chunk.x * 16 + 2, ctx.chunk.z * 16 + 2, mtype);
        builder.addPiece(room);
        room->addChildren(room.get(), builder, random);
        int seaLevel = ctx.env.sea_level;
        int yOffset;
        if (mtype == &kMesa) {
            BlockPos center = builder.boundingBox().center();
            int surfaceHeight = ctx.env.base_height(center.x, center.z, WORLD_SURFACE_WG);
            int targetY = surfaceHeight <= seaLevel ? seaLevel : random.nextIntBetweenInclusive(seaLevel, surfaceHeight);
            int dy = targetY - center.y;
            builder.offsetPiecesVertically(dy);
            yOffset = dy;
        } else {
            yOffset = builder.moveBelowSeaLevel(seaLevel, ctx.env.min_y, ctx.random, 10);
        }
        GenerationStub stub;
        stub.position = startPos.offset(0, yOffset, 0);
        stub.generate = [pieces](std::vector<std::shared_ptr<StructurePiece>>& out) { out = pieces; };
        return stub;
    }
};

struct RegisterMineshaft {
    RegisterMineshaft() {
        structure_factories()["minecraft:mineshaft"] = [](const Json& j, const Registry&) -> std::unique_ptr<Structure> {
            auto s = std::make_unique<MineshaftStructure>();
            s->mtype = j.get_string("mineshaft_type") == "mesa" ? &kMesa : &kNormal;
            return s;
        };
    }
} register_mineshaft;

}  // namespace

}  // namespace pn::vg
