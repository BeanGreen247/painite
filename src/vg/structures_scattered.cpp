// Scattered structures built from code or small templates: desert pyramids, jungle temples, swamp huts, igloos, buried treasure and nether fossils.
// Ported from Mojang's DesertPyramidPiece, JungleTemplePiece, SwampHutPiece, IglooPieces, BuriedTreasurePieces and NetherFossilPieces; the box and block
// calls were translated with tools/j2cpp.py.
#include "legacy_piece.hpp"
#include "template_piece.hpp"
#include <algorithm>
#include <set>

namespace pn::vg {

using namespace jc;

namespace {

BlockPos pos_of(const BlockPos& p) { return p; }

// ScatteredFeaturePiece
class ScatteredPiece : public LegacyPiece {
protected:
    int width = 0, height = 0, depth = 0, heightPosition = -1;
    ScatteredPiece(const char* id, int west, int floor, int north, int w, int h, int d, const Direction& direction) {
        type = id;
        gen_depth = 0;
        bb = makeBoundingBox(west, floor, north, direction, w, h, d);
        width = w; height = h; depth = d;
        setOrientation(direction);
    }
    bool updateAverageGroundHeight(JLevel& level, const BoundingBox& chunkBB, int offset) {
        if (heightPosition >= 0) return true;
        int total = 0, count = 0;
        for (int z = bb.z0; z <= bb.z1; ++z)
            for (int x = bb.x0; x <= bb.x1; ++x) {
                BlockPos pos(x, 64, z);
                if (!chunkBB.inside(pos)) continue;
                total += level.getHeightmapPos(MOTION_BLOCKING_NO_LEAVES, pos).y;
                ++count;
            }
        if (count == 0) return false;
        heightPosition = total / count;
        bb.move(0, heightPosition - bb.y0 + offset, 0);
        return true;
    }
    bool updateHeightPositionToLowestGroundHeight(JLevel& level, int offset) {
        if (heightPosition >= 0) return true;
        int lowest = level.getMaxY() + 1;
        bool found = false;
        for (int z = bb.z0; z <= bb.z1; ++z)
            for (int x = bb.x0; x <= bb.x1; ++x) {
                lowest = std::min(lowest, level.getHeightmapPos(MOTION_BLOCKING_NO_LEAVES, BlockPos(x, 0, z)).y);
                found = true;
            }
        if (!found) return false;
        heightPosition = lowest;
        bb.move(0, heightPosition - bb.y0 + offset, 0);
        return true;
    }
};

Direction random_horizontal(JRandom random) { return Direction::randomHorizontal(random.r()); }

// SinglePieceStructure
template <class PieceT>
class SinglePieceStructure : public Structure {
public:
    int width, depth;
    SinglePieceStructure(int w, int d) : width(w), depth(d) {}
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        if (structure_lowest_y(ctx, width, depth) < ctx.env.sea_level) return std::nullopt;
        return on_top_of_chunk_center(ctx, WORLD_SURFACE_WG, [&ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
            out.push_back(std::make_shared<PieceT>(JRandom(ctx.random), ctx.chunk.x * 16, ctx.chunk.z * 16));
        });
    }
};

// ---------------------------------------------------------------- desert pyramid

class DesertPyramidPiece : public ScatteredPiece {
public:
    bool hasPlacedChest[4] = {false, false, false, false};
    std::vector<BlockPos> potentialSuspiciousSandWorldPositions;
    BlockPos randomCollapsedRoofPos;
    DesertPyramidPiece(JRandom random, int west, int north) : ScatteredPiece("minecraft:tedp", west, 64, north, 21, 15, 21, random_horizontal(random)) {}

    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

        int x;
        if (!updateHeightPositionToLowestGroundHeight(level, -random.nextInt(3))) {
            return;
        }
        generateBox(level, chunkBB, 0, -4, 0, width - 1, 0, depth - 1, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        for (int pos = 1; pos <= 9; ++pos) {
            generateBox(level, chunkBB, pos, pos, pos, width - 1 - pos, pos, depth - 1 - pos, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
            generateBox(level, chunkBB, pos + 1, pos, pos + 1, width - 2 - pos, pos, depth - 2 - pos, blk("minecraft:air"), blk("minecraft:air"), false);
        }
        for (int x2 = 0; x2 < width; ++x2) {
            for (int z = 0; z < depth; ++z) {
                int startY = -5;
                fillColumnDown(level, blk("minecraft:sandstone"), x2, -5, z, chunkBB);
            }
        }
        BS northStairs = blk("minecraft:sandstone_stairs").setValue("facing", Direction::NORTH);
        BS southStairs = blk("minecraft:sandstone_stairs").setValue("facing", Direction::SOUTH);
        BS eastStairs = blk("minecraft:sandstone_stairs").setValue("facing", Direction::EAST);
        BS westStairs = blk("minecraft:sandstone_stairs").setValue("facing", Direction::WEST);
        generateBox(level, chunkBB, 0, 0, 0, 4, 9, 4, blk("minecraft:sandstone"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, 1, 10, 1, 3, 10, 3, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        placeBlock(level, northStairs, 2, 10, 0, chunkBB);
        placeBlock(level, southStairs, 2, 10, 4, chunkBB);
        placeBlock(level, eastStairs, 0, 10, 2, chunkBB);
        placeBlock(level, westStairs, 4, 10, 2, chunkBB);
        generateBox(level, chunkBB, width - 5, 0, 0, width - 1, 9, 4, blk("minecraft:sandstone"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, width - 4, 10, 1, width - 2, 10, 3, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        placeBlock(level, northStairs, width - 3, 10, 0, chunkBB);
        placeBlock(level, southStairs, width - 3, 10, 4, chunkBB);
        placeBlock(level, eastStairs, width - 5, 10, 2, chunkBB);
        placeBlock(level, westStairs, width - 1, 10, 2, chunkBB);
        generateBox(level, chunkBB, 8, 0, 0, 12, 4, 4, blk("minecraft:sandstone"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, 9, 1, 0, 11, 3, 4, blk("minecraft:air"), blk("minecraft:air"), false);
        placeBlock(level, blk("minecraft:cut_sandstone"), 9, 1, 1, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 9, 2, 1, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 9, 3, 1, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 10, 3, 1, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 11, 3, 1, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 11, 2, 1, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 11, 1, 1, chunkBB);
        generateBox(level, chunkBB, 4, 1, 1, 8, 3, 3, blk("minecraft:sandstone"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, 4, 1, 2, 8, 2, 2, blk("minecraft:air"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, 12, 1, 1, 16, 3, 3, blk("minecraft:sandstone"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, 12, 1, 2, 16, 2, 2, blk("minecraft:air"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, 5, 4, 5, width - 6, 4, depth - 6, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, 9, 4, 9, 11, 4, 11, blk("minecraft:air"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, 8, 1, 8, 8, 3, 8, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 12, 1, 8, 12, 3, 8, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 8, 1, 12, 8, 3, 12, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 12, 1, 12, 12, 3, 12, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 1, 1, 5, 4, 4, 11, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, width - 5, 1, 5, width - 2, 4, 11, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, 6, 7, 9, 6, 7, 11, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, width - 7, 7, 9, width - 7, 7, 11, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, 5, 5, 9, 5, 7, 11, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, width - 6, 5, 9, width - 6, 7, 11, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        placeBlock(level, blk("minecraft:air"), 5, 5, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), 5, 6, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), 6, 6, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), width - 6, 5, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), width - 6, 6, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), width - 7, 6, 10, chunkBB);
        generateBox(level, chunkBB, 2, 4, 4, 2, 6, 4, blk("minecraft:air"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, width - 3, 4, 4, width - 3, 6, 4, blk("minecraft:air"), blk("minecraft:air"), false);
        placeBlock(level, northStairs, 2, 4, 5, chunkBB);
        placeBlock(level, northStairs, 2, 3, 4, chunkBB);
        placeBlock(level, northStairs, width - 3, 4, 5, chunkBB);
        placeBlock(level, northStairs, width - 3, 3, 4, chunkBB);
        generateBox(level, chunkBB, 1, 1, 3, 2, 2, 3, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, width - 3, 1, 3, width - 2, 2, 3, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        placeBlock(level, blk("minecraft:sandstone"), 1, 1, 2, chunkBB);
        placeBlock(level, blk("minecraft:sandstone"), width - 2, 1, 2, chunkBB);
        placeBlock(level, blk("minecraft:sandstone_slab"), 1, 2, 2, chunkBB);
        placeBlock(level, blk("minecraft:sandstone_slab"), width - 2, 2, 2, chunkBB);
        placeBlock(level, westStairs, 2, 1, 2, chunkBB);
        placeBlock(level, eastStairs, width - 3, 1, 2, chunkBB);
        generateBox(level, chunkBB, 4, 3, 5, 4, 3, 17, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, width - 5, 3, 5, width - 5, 3, 17, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, 3, 1, 5, 4, 2, 16, blk("minecraft:air"), blk("minecraft:air"), false);
        generateBox(level, chunkBB, width - 6, 1, 5, width - 5, 2, 16, blk("minecraft:air"), blk("minecraft:air"), false);
        for (int z = 5; z <= 17; z += 2) {
            placeBlock(level, blk("minecraft:cut_sandstone"), 4, 1, z, chunkBB);
            placeBlock(level, blk("minecraft:chiseled_sandstone"), 4, 2, z, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), width - 5, 1, z, chunkBB);
            placeBlock(level, blk("minecraft:chiseled_sandstone"), width - 5, 2, z, chunkBB);
        }
        placeBlock(level, blk("minecraft:orange_terracotta"), 10, 0, 7, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 10, 0, 8, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 9, 0, 9, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 11, 0, 9, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 8, 0, 10, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 12, 0, 10, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 7, 0, 10, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 13, 0, 10, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 9, 0, 11, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 11, 0, 11, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 10, 0, 12, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 10, 0, 13, chunkBB);
        placeBlock(level, blk("minecraft:blue_terracotta"), 10, 0, 10, chunkBB);
        for (x = 0; x <= width - 1; x += width - 1) {
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 2, 1, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 2, 2, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 2, 3, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 3, 1, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 3, 2, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 3, 3, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 4, 1, chunkBB);
            placeBlock(level, blk("minecraft:chiseled_sandstone"), x, 4, 2, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 4, 3, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 5, 1, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 5, 2, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 5, 3, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 6, 1, chunkBB);
            placeBlock(level, blk("minecraft:chiseled_sandstone"), x, 6, 2, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 6, 3, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 7, 1, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 7, 2, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 7, 3, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 8, 1, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 8, 2, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 8, 3, chunkBB);
        }
        for (x = 2; x <= width - 3; x += width - 3 - 2) {
            placeBlock(level, blk("minecraft:cut_sandstone"), x - 1, 2, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 2, 0, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x + 1, 2, 0, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x - 1, 3, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 3, 0, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x + 1, 3, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x - 1, 4, 0, chunkBB);
            placeBlock(level, blk("minecraft:chiseled_sandstone"), x, 4, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x + 1, 4, 0, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x - 1, 5, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 5, 0, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x + 1, 5, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x - 1, 6, 0, chunkBB);
            placeBlock(level, blk("minecraft:chiseled_sandstone"), x, 6, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x + 1, 6, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x - 1, 7, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x, 7, 0, chunkBB);
            placeBlock(level, blk("minecraft:orange_terracotta"), x + 1, 7, 0, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x - 1, 8, 0, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x, 8, 0, chunkBB);
            placeBlock(level, blk("minecraft:cut_sandstone"), x + 1, 8, 0, chunkBB);
        }
        generateBox(level, chunkBB, 8, 4, 0, 12, 6, 0, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        placeBlock(level, blk("minecraft:air"), 8, 6, 0, chunkBB);
        placeBlock(level, blk("minecraft:air"), 12, 6, 0, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 9, 5, 0, chunkBB);
        placeBlock(level, blk("minecraft:chiseled_sandstone"), 10, 5, 0, chunkBB);
        placeBlock(level, blk("minecraft:orange_terracotta"), 11, 5, 0, chunkBB);
        generateBox(level, chunkBB, 8, -14, 8, 12, -11, 12, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 8, -10, 8, 12, -10, 12, blk("minecraft:chiseled_sandstone"), blk("minecraft:chiseled_sandstone"), false);
        generateBox(level, chunkBB, 8, -9, 8, 12, -9, 12, blk("minecraft:cut_sandstone"), blk("minecraft:cut_sandstone"), false);
        generateBox(level, chunkBB, 8, -8, 8, 12, -1, 12, blk("minecraft:sandstone"), blk("minecraft:sandstone"), false);
        generateBox(level, chunkBB, 9, -11, 9, 11, -1, 11, blk("minecraft:air"), blk("minecraft:air"), false);
        placeBlock(level, blk("minecraft:stone_pressure_plate"), 10, -11, 10, chunkBB);
        generateBox(level, chunkBB, 9, -13, 9, 11, -13, 11, blk("minecraft:tnt"), blk("minecraft:air"), false);
        placeBlock(level, blk("minecraft:air"), 8, -11, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), 8, -10, 10, chunkBB);
        placeBlock(level, blk("minecraft:chiseled_sandstone"), 7, -10, 10, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 7, -11, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), 12, -11, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), 12, -10, 10, chunkBB);
        placeBlock(level, blk("minecraft:chiseled_sandstone"), 13, -10, 10, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 13, -11, 10, chunkBB);
        placeBlock(level, blk("minecraft:air"), 10, -11, 8, chunkBB);
        placeBlock(level, blk("minecraft:air"), 10, -10, 8, chunkBB);
        placeBlock(level, blk("minecraft:chiseled_sandstone"), 10, -10, 7, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 10, -11, 7, chunkBB);
        placeBlock(level, blk("minecraft:air"), 10, -11, 12, chunkBB);
        placeBlock(level, blk("minecraft:air"), 10, -10, 12, chunkBB);
        placeBlock(level, blk("minecraft:chiseled_sandstone"), 10, -10, 13, chunkBB);
        placeBlock(level, blk("minecraft:cut_sandstone"), 10, -11, 13, chunkBB);
        for (Direction direction : Direction::HORIZONTAL) {
            if (hasPlacedChest[direction.get2DDataValue()]) continue;
            int xo = direction.getStepX() * 2;
            int zo = direction.getStepZ() * 2;
            hasPlacedChest[direction.get2DDataValue()] = createChest(level, chunkBB, random, 10 + xo, -11, 10 + zo, "minecraft:chests/desert_pyramid");
        }
        addCellar(level, chunkBB);
    }

private:
    void addCellar(JLevel& level, const BoundingBox& chunkBB) {

        BlockPos roomCenter = BlockPos(16, -4, 13);
        addCellarStairs(roomCenter, level, chunkBB);
        addCellarRoom(roomCenter, level, chunkBB);
    }
    void addCellarStairs(const BlockPos& roomCenter, JLevel& level, const BoundingBox& chunkBB) {

        int x = roomCenter.x;
        int y = roomCenter.y;
        int z = roomCenter.z;
        BS sandStoneStairs = blk("minecraft:sandstone_stairs");
        placeBlock(level, sandStoneStairs.rotate(Rotation::COUNTERCLOCKWISE_90), 13, -1, 17, chunkBB);
        placeBlock(level, sandStoneStairs.rotate(Rotation::COUNTERCLOCKWISE_90), 14, -2, 17, chunkBB);
        placeBlock(level, sandStoneStairs.rotate(Rotation::COUNTERCLOCKWISE_90), 15, -3, 17, chunkBB);
        BS sand = blk("minecraft:sand");
        BS sandStone = blk("minecraft:sandstone");
        bool variant = levelRandom(level).nextBoolean();
        placeBlock(level, sand, x - 4, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x - 3, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x - 2, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x - 1, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x, y + 4, z + 4, chunkBB);
        placeBlock(level, sand, x - 2, y + 3, z + 4, chunkBB);
        placeBlock(level, variant ? sand : sandStone, x - 1, y + 3, z + 4, chunkBB);
        placeBlock(level, !variant ? sand : sandStone, x, y + 3, z + 4, chunkBB);
        placeBlock(level, sand, x - 1, y + 2, z + 4, chunkBB);
        placeBlock(level, sandStone, x, y + 2, z + 4, chunkBB);
        placeBlock(level, sand, x, y + 1, z + 4, chunkBB);
    }
    void addCellarRoom(const BlockPos& roomCenter, JLevel& level, const BoundingBox& chunkBB) {

        int x = roomCenter.x;
        int y = roomCenter.y;
        int z = roomCenter.z;
        BS cutSandStone = blk("minecraft:cut_sandstone");
        BS hieroglyphsSandStone = blk("minecraft:chiseled_sandstone");
        generateBox(level, chunkBB, x - 3, y + 1, z - 3, x - 3, y + 1, z + 2, cutSandStone, cutSandStone, true);
        generateBox(level, chunkBB, x + 3, y + 1, z - 3, x + 3, y + 1, z + 2, cutSandStone, cutSandStone, true);
        generateBox(level, chunkBB, x - 3, y + 1, z - 3, x + 3, y + 1, z - 2, cutSandStone, cutSandStone, true);
        generateBox(level, chunkBB, x - 3, y + 1, z + 3, x + 3, y + 1, z + 3, cutSandStone, cutSandStone, true);
        generateBox(level, chunkBB, x - 3, y + 2, z - 3, x - 3, y + 2, z + 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
        generateBox(level, chunkBB, x + 3, y + 2, z - 3, x + 3, y + 2, z + 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
        generateBox(level, chunkBB, x - 3, y + 2, z - 3, x + 3, y + 2, z - 2, hieroglyphsSandStone, hieroglyphsSandStone, true);
        generateBox(level, chunkBB, x - 3, y + 2, z + 3, x + 3, y + 2, z + 3, hieroglyphsSandStone, hieroglyphsSandStone, true);
        generateBox(level, chunkBB, x - 3, -1, z - 3, x - 3, -1, z + 2, cutSandStone, cutSandStone, true);
        generateBox(level, chunkBB, x + 3, -1, z - 3, x + 3, -1, z + 2, cutSandStone, cutSandStone, true);
        generateBox(level, chunkBB, x - 3, -1, z - 3, x + 3, -1, z - 2, cutSandStone, cutSandStone, true);
        generateBox(level, chunkBB, x - 3, -1, z + 3, x + 3, -1, z + 3, cutSandStone, cutSandStone, true);
        placeSandBox(x - 2, y + 1, z - 2, x + 2, y + 3, z + 2);
        placeCollapsedRoof(level, chunkBB, x - 2, y + 4, z - 2, x + 2, z + 2);
        BS orangeTeracotta = blk("minecraft:orange_terracotta");
        BS blueTeracotta = blk("minecraft:blue_terracotta");
        placeBlock(level, blueTeracotta, x, y, z, chunkBB);
        placeBlock(level, orangeTeracotta, x + 1, y, z - 1, chunkBB);
        placeBlock(level, orangeTeracotta, x + 1, y, z + 1, chunkBB);
        placeBlock(level, orangeTeracotta, x - 1, y, z - 1, chunkBB);
        placeBlock(level, orangeTeracotta, x - 1, y, z + 1, chunkBB);
        placeBlock(level, orangeTeracotta, x + 2, y, z, chunkBB);
        placeBlock(level, orangeTeracotta, x - 2, y, z, chunkBB);
        placeBlock(level, orangeTeracotta, x, y, z + 2, chunkBB);
        placeBlock(level, orangeTeracotta, x, y, z - 2, chunkBB);
        placeBlock(level, orangeTeracotta, x + 3, y, z, chunkBB);
        placeSand(x + 3, y + 1, z);
        placeSand(x + 3, y + 2, z);
        placeBlock(level, cutSandStone, x + 4, y + 1, z, chunkBB);
        placeBlock(level, hieroglyphsSandStone, x + 4, y + 2, z, chunkBB);
        placeBlock(level, orangeTeracotta, x - 3, y, z, chunkBB);
        placeSand(x - 3, y + 1, z);
        placeSand(x - 3, y + 2, z);
        placeBlock(level, cutSandStone, x - 4, y + 1, z, chunkBB);
        placeBlock(level, hieroglyphsSandStone, x - 4, y + 2, z, chunkBB);
        placeBlock(level, orangeTeracotta, x, y, z + 3, chunkBB);
        placeSand(x, y + 1, z + 3);
        placeSand(x, y + 2, z + 3);
        placeBlock(level, orangeTeracotta, x, y, z - 3, chunkBB);
        placeSand(x, y + 1, z - 3);
        placeSand(x, y + 2, z - 3);
        placeBlock(level, cutSandStone, x, y + 1, z - 4, chunkBB);
        placeBlock(level, hieroglyphsSandStone, x, -2, z - 4, chunkBB);
    }
    void placeSand(int x, int y, int z) { potentialSuspiciousSandWorldPositions.push_back(getWorldPos(x, y, z)); }
    void placeSandBox(int x0, int y0, int z0, int x1, int y1, int z1) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z) placeSand(x, y, z);
    }
    void placeCollapsedRoofPiece(JLevel& level, int x, int y, int z, const BoundingBox& chunkBB) {

        if (levelRandom(level).nextFloat() < 0.33f) {
            BS blockState = blk("minecraft:sandstone");
            placeBlock(level, blockState, x, y, z, chunkBB);
        } else {
            BS blockState = blk("minecraft:sand");
            placeBlock(level, blockState, x, y, z, chunkBB);
        }
    }
    void placeCollapsedRoof(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int z1) {

        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                placeCollapsedRoofPiece(level, x, y0, z, chunkBB);
            }
        }
        JRandom random = positionalRandom(level.getSeed(), getWorldPos(x0, y0, z0));
        int roofPosX = random.nextIntBetweenInclusive(x0, x1);
        int roofPosZ = random.nextIntBetweenInclusive(z0, z1);
        randomCollapsedRoofPos = BlockPos(getWorldX(roofPosX, roofPosZ), getWorldY(y0), getWorldZ(roofPosX, roofPosZ));
    }
};

class DesertPyramidStructure : public SinglePieceStructure<DesertPyramidPiece> {
public:
    DesertPyramidStructure() : SinglePieceStructure(21, 21) {}
    static void placeSuspiciousSand(PieceCtx& ctx, const BlockPos& pos) {
        if (!ctx.chunk_bb.inside(pos)) return;
        ctx.level.set(pos, blk("minecraft:suspicious_sand").id);
        pn::Nbt be = pn::Nbt::compound();
        be.put("id", pn::Nbt::str("minecraft:brushable_block"));
        be.put("x", pn::Nbt::make(pn::Nbt::Int, pos.x));
        be.put("y", pn::Nbt::make(pn::Nbt::Int, pos.y));
        be.put("z", pn::Nbt::make(pn::Nbt::Int, pos.z));
        be.put("LootTable", pn::Nbt::str("minecraft:archaeology/desert_pyramid"));
        // BlockPos.asLong
        int64_t packed = (int64_t(pos.x & 0x3FFFFFF) << 38) | (int64_t(pos.z & 0x3FFFFFF) << 12) | int64_t(pos.y & 0xFFF);
        be.put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, packed));
        ctx.level.set_block_entity(pos, std::move(be));
    }
    void after_place(PieceCtx& ctx, const StructureStart& start) const override {
        std::set<BlockPos> unique;
        for (auto& piece : start.pieces) {
            auto* dp = dynamic_cast<DesertPyramidPiece*>(piece.get());
            if (!dp) continue;
            for (auto& p : dp->potentialSuspiciousSandWorldPositions) unique.insert(p);
            placeSuspiciousSand(ctx, dp->randomCollapsedRoofPos);
        }
        std::vector<BlockPos> shuffled(unique.begin(), unique.end());
        BoundingBox center_box = start.piece_bounds();
        JRandom random = positionalRandom(ctx.senv.seed, center_box.center());
        for (int i = int(shuffled.size()); i > 1; --i) std::swap(shuffled[size_t(i - 1)], shuffled[size_t(random.nextInt(i))]);
        int to_place = std::min(int(unique.size()), random.nextInt(5, 8));
        for (auto& pos : shuffled) {
            if (to_place > 0) { --to_place; placeSuspiciousSand(ctx, pos); continue; }
            if (!ctx.chunk_bb.inside(pos)) continue;
            ctx.level.set(pos, blk("minecraft:sand").id);
        }
    }
};

// ---------------------------------------------------------------- jungle temple

struct MossStoneSelector : BlockSelector {
    void next(JRandom& random, int, int, int, bool) override { next_ = random.nextFloat() < 0.4f ? blk("minecraft:cobblestone") : blk("minecraft:mossy_cobblestone"); }
};

class JungleTemplePiece : public ScatteredPiece {
public:
    bool placedMainChest = false, placedHiddenChest = false, placedTrap1 = false, placedTrap2 = false;
    JungleTemplePiece(JRandom random, int west, int north) : ScatteredPiece("minecraft:tejp", west, 64, north, 12, 10, 15, random_horizontal(random)) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {
        static MossStoneSelector STONE_SELECTOR;

        int z;
        if (!updateAverageGroundHeight(level, chunkBB, 0)) {
            return;
        }
        generateBox(level, chunkBB, 0, -4, 0, width - 1, 0, depth - 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 1, 2, 9, 2, 2, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 1, 12, 9, 2, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 1, 3, 2, 2, 11, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 9, 1, 3, 9, 2, 11, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 1, 3, 1, 10, 6, 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 1, 3, 13, 10, 6, 13, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 1, 3, 2, 1, 6, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 10, 3, 2, 10, 6, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 3, 2, 9, 3, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 6, 2, 9, 6, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 3, 7, 3, 8, 7, 11, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 8, 4, 7, 8, 10, false, random, STONE_SELECTOR);
        generateAirBox(level, chunkBB, 3, 1, 3, 8, 2, 11);
        generateAirBox(level, chunkBB, 4, 3, 6, 7, 3, 9);
        generateAirBox(level, chunkBB, 2, 4, 2, 9, 5, 12);
        generateAirBox(level, chunkBB, 4, 6, 5, 7, 6, 9);
        generateAirBox(level, chunkBB, 5, 7, 6, 6, 7, 8);
        generateAirBox(level, chunkBB, 5, 1, 2, 6, 2, 2);
        generateAirBox(level, chunkBB, 5, 2, 12, 6, 2, 12);
        generateAirBox(level, chunkBB, 5, 5, 1, 6, 5, 1);
        generateAirBox(level, chunkBB, 5, 5, 13, 6, 5, 13);
        placeBlock(level, blk("minecraft:air"), 1, 5, 5, chunkBB);
        placeBlock(level, blk("minecraft:air"), 10, 5, 5, chunkBB);
        placeBlock(level, blk("minecraft:air"), 1, 5, 9, chunkBB);
        placeBlock(level, blk("minecraft:air"), 10, 5, 9, chunkBB);
        for (int z2 = 0; z2 <= 14; z2 += 14) {
            generateBox(level, chunkBB, 2, 4, z2, 2, 5, z2, false, random, STONE_SELECTOR);
            generateBox(level, chunkBB, 4, 4, z2, 4, 5, z2, false, random, STONE_SELECTOR);
            generateBox(level, chunkBB, 7, 4, z2, 7, 5, z2, false, random, STONE_SELECTOR);
            generateBox(level, chunkBB, 9, 4, z2, 9, 5, z2, false, random, STONE_SELECTOR);
        }
        generateBox(level, chunkBB, 5, 6, 0, 6, 6, 0, false, random, STONE_SELECTOR);
        for (int x = 0; x <= 11; x += 11) {
            for (int z3 = 2; z3 <= 12; z3 += 2) {
                generateBox(level, chunkBB, x, 4, z3, x, 5, z3, false, random, STONE_SELECTOR);
            }
            generateBox(level, chunkBB, x, 6, 5, x, 6, 5, false, random, STONE_SELECTOR);
            generateBox(level, chunkBB, x, 6, 9, x, 6, 9, false, random, STONE_SELECTOR);
        }
        generateBox(level, chunkBB, 2, 7, 2, 2, 9, 2, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 9, 7, 2, 9, 9, 2, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 2, 7, 12, 2, 9, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 9, 7, 12, 9, 9, 12, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 9, 4, 4, 9, 4, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, 9, 4, 7, 9, 4, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 9, 10, 4, 9, 10, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, 9, 10, 7, 9, 10, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 5, 9, 7, 6, 9, 7, false, random, STONE_SELECTOR);
        BS eastStairs = blk("minecraft:cobblestone_stairs").setValue("facing", Direction::EAST);
        BS westStairs = blk("minecraft:cobblestone_stairs").setValue("facing", Direction::WEST);
        BS southStairs = blk("minecraft:cobblestone_stairs").setValue("facing", Direction::SOUTH);
        BS northStairs = blk("minecraft:cobblestone_stairs").setValue("facing", Direction::NORTH);
        placeBlock(level, northStairs, 5, 9, 6, chunkBB);
        placeBlock(level, northStairs, 6, 9, 6, chunkBB);
        placeBlock(level, southStairs, 5, 9, 8, chunkBB);
        placeBlock(level, southStairs, 6, 9, 8, chunkBB);
        placeBlock(level, northStairs, 4, 0, 0, chunkBB);
        placeBlock(level, northStairs, 5, 0, 0, chunkBB);
        placeBlock(level, northStairs, 6, 0, 0, chunkBB);
        placeBlock(level, northStairs, 7, 0, 0, chunkBB);
        placeBlock(level, northStairs, 4, 1, 8, chunkBB);
        placeBlock(level, northStairs, 4, 2, 9, chunkBB);
        placeBlock(level, northStairs, 4, 3, 10, chunkBB);
        placeBlock(level, northStairs, 7, 1, 8, chunkBB);
        placeBlock(level, northStairs, 7, 2, 9, chunkBB);
        placeBlock(level, northStairs, 7, 3, 10, chunkBB);
        generateBox(level, chunkBB, 4, 1, 9, 4, 1, 9, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, 1, 9, 7, 1, 9, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 4, 1, 10, 7, 2, 10, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 5, 4, 5, 6, 4, 5, false, random, STONE_SELECTOR);
        placeBlock(level, eastStairs, 4, 4, 5, chunkBB);
        placeBlock(level, westStairs, 7, 4, 5, chunkBB);
        for (int i = 0; i < 4; ++i) {
            placeBlock(level, southStairs, 5, 0 - i, 6 + i, chunkBB);
            placeBlock(level, southStairs, 6, 0 - i, 6 + i, chunkBB);
            generateAirBox(level, chunkBB, 5, 0 - i, 7 + i, 6, 0 - i, 9 + i);
        }
        generateAirBox(level, chunkBB, 1, -3, 12, 10, -1, 13);
        generateAirBox(level, chunkBB, 1, -3, 1, 3, -1, 13);
        generateAirBox(level, chunkBB, 1, -3, 1, 9, -1, 5);
        for (z = 1; z <= 13; z += 2) {
            generateBox(level, chunkBB, 1, -3, z, 1, -2, z, false, random, STONE_SELECTOR);
        }
        for (z = 2; z <= 12; z += 2) {
            generateBox(level, chunkBB, 1, -1, z, 3, -1, z, false, random, STONE_SELECTOR);
        }
        generateBox(level, chunkBB, 2, -2, 1, 5, -2, 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 7, -2, 1, 9, -2, 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 6, -3, 1, 6, -3, 1, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 6, -1, 1, 6, -1, 1, false, random, STONE_SELECTOR);
        placeBlock(level, (blk("minecraft:tripwire_hook").setValue("facing", Direction::EAST)).setValue("attached", true), 1, -3, 8, chunkBB);
        placeBlock(level, (blk("minecraft:tripwire_hook").setValue("facing", Direction::WEST)).setValue("attached", true), 4, -3, 8, chunkBB);
        placeBlock(level, ((blk("minecraft:tripwire").setValue("east", true)).setValue("west", true)).setValue("attached", true), 2, -3, 8, chunkBB);
        placeBlock(level, ((blk("minecraft:tripwire").setValue("east", true)).setValue("west", true)).setValue("attached", true), 3, -3, 8, chunkBB);
        BS redstoneWireNS = (blk("minecraft:redstone_wire").setValue("north", "side")).setValue("south", "side");
        placeBlock(level, redstoneWireNS, 5, -3, 7, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 6, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 5, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 4, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 3, chunkBB);
        placeBlock(level, redstoneWireNS, 5, -3, 2, chunkBB);
        placeBlock(level, (blk("minecraft:redstone_wire").setValue("north", "side")).setValue("west", "side"), 5, -3, 1, chunkBB);
        placeBlock(level, (blk("minecraft:redstone_wire").setValue("east", "side")).setValue("west", "side"), 4, -3, 1, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 3, -3, 1, chunkBB);
        if (!placedTrap1) {
            placedTrap1 = createDispenser(level, chunkBB, random, 3, -2, 1, Direction::NORTH, "minecraft:chests/jungle_temple_dispenser");
        }
        placeBlock(level, blk("minecraft:vine").setValue("south", true), 3, -2, 2, chunkBB);
        placeBlock(level, (blk("minecraft:tripwire_hook").setValue("facing", Direction::NORTH)).setValue("attached", true), 7, -3, 1, chunkBB);
        placeBlock(level, (blk("minecraft:tripwire_hook").setValue("facing", Direction::SOUTH)).setValue("attached", true), 7, -3, 5, chunkBB);
        placeBlock(level, ((blk("minecraft:tripwire").setValue("north", true)).setValue("south", true)).setValue("attached", true), 7, -3, 2, chunkBB);
        placeBlock(level, ((blk("minecraft:tripwire").setValue("north", true)).setValue("south", true)).setValue("attached", true), 7, -3, 3, chunkBB);
        placeBlock(level, ((blk("minecraft:tripwire").setValue("north", true)).setValue("south", true)).setValue("attached", true), 7, -3, 4, chunkBB);
        placeBlock(level, (blk("minecraft:redstone_wire").setValue("east", "side")).setValue("west", "side"), 8, -3, 6, chunkBB);
        placeBlock(level, (blk("minecraft:redstone_wire").setValue("west", "side")).setValue("south", "side"), 9, -3, 6, chunkBB);
        placeBlock(level, (blk("minecraft:redstone_wire").setValue("north", "side")).setValue("south", "up"), 9, -3, 5, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 9, -3, 4, chunkBB);
        placeBlock(level, redstoneWireNS, 9, -2, 4, chunkBB);
        if (!placedTrap2) {
            placedTrap2 = createDispenser(level, chunkBB, random, 9, -2, 3, Direction::WEST, "minecraft:chests/jungle_temple_dispenser");
        }
        placeBlock(level, blk("minecraft:vine").setValue("east", true), 8, -1, 3, chunkBB);
        placeBlock(level, blk("minecraft:vine").setValue("east", true), 8, -2, 3, chunkBB);
        if (!placedMainChest) {
            placedMainChest = createChest(level, chunkBB, random, 8, -3, 3, "minecraft:chests/jungle_temple");
        }
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 9, -3, 2, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 8, -3, 1, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 4, -3, 5, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 5, -2, 5, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 5, -1, 5, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 6, -3, 5, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 7, -2, 5, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 7, -1, 5, chunkBB);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 8, -3, 5, chunkBB);
        generateBox(level, chunkBB, 9, -1, 1, 9, -1, 5, false, random, STONE_SELECTOR);
        generateAirBox(level, chunkBB, 8, -3, 8, 10, -1, 10);
        placeBlock(level, blk("minecraft:chiseled_stone_bricks"), 8, -2, 11, chunkBB);
        placeBlock(level, blk("minecraft:chiseled_stone_bricks"), 9, -2, 11, chunkBB);
        placeBlock(level, blk("minecraft:chiseled_stone_bricks"), 10, -2, 11, chunkBB);
        BS lever = (blk("minecraft:lever").setValue("facing", Direction::NORTH)).setValue("face", "wall");
        placeBlock(level, lever, 8, -2, 12, chunkBB);
        placeBlock(level, lever, 9, -2, 12, chunkBB);
        placeBlock(level, lever, 10, -2, 12, chunkBB);
        generateBox(level, chunkBB, 8, -3, 8, 8, -3, 10, false, random, STONE_SELECTOR);
        generateBox(level, chunkBB, 10, -3, 8, 10, -3, 10, false, random, STONE_SELECTOR);
        placeBlock(level, blk("minecraft:mossy_cobblestone"), 10, -2, 9, chunkBB);
        placeBlock(level, redstoneWireNS, 8, -2, 9, chunkBB);
        placeBlock(level, redstoneWireNS, 8, -2, 10, chunkBB);
        placeBlock(level, (((blk("minecraft:redstone_wire").setValue("north", "side")).setValue("south", "side")).setValue("east", "side")).setValue("west", "side"), 10, -1, 9, chunkBB);
        placeBlock(level, blk("minecraft:sticky_piston").setValue("facing", Direction::UP), 9, -2, 8, chunkBB);
        placeBlock(level, blk("minecraft:sticky_piston").setValue("facing", Direction::WEST), 10, -2, 8, chunkBB);
        placeBlock(level, blk("minecraft:sticky_piston").setValue("facing", Direction::WEST), 10, -1, 8, chunkBB);
        placeBlock(level, blk("minecraft:repeater").setValue("facing", Direction::NORTH), 10, -2, 10, chunkBB);
        if (!placedHiddenChest) {
            placedHiddenChest = createChest(level, chunkBB, random, 9, -3, 10, "minecraft:chests/jungle_temple");
        }
    }
};

// ---------------------------------------------------------------- swamp hut

class SwampHutPiece : public ScatteredPiece {
public:
    bool spawnedWitch = false, spawnedCat = false;
    SwampHutPiece(JRandom random, int west, int north) : ScatteredPiece("minecraft:tesh", west, 64, north, 7, 7, 9, random_horizontal(random)) {}
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {

        BlockPos pos;
        if (!updateAverageGroundHeight(level, chunkBB, 0)) {
            return;
        }
        generateBox(level, chunkBB, 1, 1, 1, 5, 1, 7, blk("minecraft:spruce_planks"), blk("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 1, 4, 2, 5, 4, 7, blk("minecraft:spruce_planks"), blk("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 2, 1, 0, 4, 1, 0, blk("minecraft:spruce_planks"), blk("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 2, 2, 2, 3, 3, 2, blk("minecraft:spruce_planks"), blk("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 1, 2, 3, 1, 3, 6, blk("minecraft:spruce_planks"), blk("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 5, 2, 3, 5, 3, 6, blk("minecraft:spruce_planks"), blk("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 2, 2, 7, 4, 3, 7, blk("minecraft:spruce_planks"), blk("minecraft:spruce_planks"), false);
        generateBox(level, chunkBB, 1, 0, 2, 1, 3, 2, blk("minecraft:oak_log"), blk("minecraft:oak_log"), false);
        generateBox(level, chunkBB, 5, 0, 2, 5, 3, 2, blk("minecraft:oak_log"), blk("minecraft:oak_log"), false);
        generateBox(level, chunkBB, 1, 0, 7, 1, 3, 7, blk("minecraft:oak_log"), blk("minecraft:oak_log"), false);
        generateBox(level, chunkBB, 5, 0, 7, 5, 3, 7, blk("minecraft:oak_log"), blk("minecraft:oak_log"), false);
        placeBlock(level, blk("minecraft:oak_fence"), 2, 3, 2, chunkBB);
        placeBlock(level, blk("minecraft:oak_fence"), 3, 3, 7, chunkBB);
        placeBlock(level, blk("minecraft:air"), 1, 3, 4, chunkBB);
        placeBlock(level, blk("minecraft:air"), 5, 3, 4, chunkBB);
        placeBlock(level, blk("minecraft:air"), 5, 3, 5, chunkBB);
        placeBlock(level, blk("minecraft:potted_red_mushroom"), 1, 3, 5, chunkBB);
        placeBlock(level, blk("minecraft:crafting_table"), 3, 2, 6, chunkBB);
        placeBlock(level, blk("minecraft:cauldron"), 4, 2, 6, chunkBB);
        placeBlock(level, blk("minecraft:oak_fence"), 1, 2, 1, chunkBB);
        placeBlock(level, blk("minecraft:oak_fence"), 5, 2, 1, chunkBB);
        BS northStairs = blk("minecraft:spruce_stairs").setValue("facing", Direction::NORTH);
        BS eastStairs = blk("minecraft:spruce_stairs").setValue("facing", Direction::EAST);
        BS westStairs = blk("minecraft:spruce_stairs").setValue("facing", Direction::WEST);
        BS southStairs = blk("minecraft:spruce_stairs").setValue("facing", Direction::SOUTH);
        generateBox(level, chunkBB, 0, 4, 1, 6, 4, 1, northStairs, northStairs, false);
        generateBox(level, chunkBB, 0, 4, 2, 0, 4, 7, eastStairs, eastStairs, false);
        generateBox(level, chunkBB, 6, 4, 2, 6, 4, 7, westStairs, westStairs, false);
        generateBox(level, chunkBB, 0, 4, 8, 6, 4, 8, southStairs, southStairs, false);
        placeBlock(level, northStairs.setValue("shape", "outer_right"), 0, 4, 1, chunkBB);
        placeBlock(level, northStairs.setValue("shape", "outer_left"), 6, 4, 1, chunkBB);
        placeBlock(level, southStairs.setValue("shape", "outer_left"), 0, 4, 8, chunkBB);
        placeBlock(level, southStairs.setValue("shape", "outer_right"), 6, 4, 8, chunkBB);
        for (int z = 2; z <= 7; z += 5) {
            for (int x = 1; x <= 5; x += 4) {
                fillColumnDown(level, blk("minecraft:oak_log"), x, -1, z, chunkBB);
            }
        }
        {
            BlockPos pos = getWorldPos(2, 2, 5);
            if (!spawnedWitch && chunkBB.inside(pos)) {
                spawnedWitch = true;
                add_entity(level, "minecraft:witch", double(pos.x) + 0.5, pos.y, double(pos.z) + 0.5);
            }
        }
        spawnCat(level, chunkBB);
    }
    void spawnCat(JLevel& level, const BoundingBox& chunkBB) {

        BlockPos pos = getWorldPos(2, 2, 5);
        if (!spawnedCat && chunkBB.inside(pos)) {
            spawnedCat = true;
            add_entity(level, "minecraft:cat", double(pos.x) + 0.5, pos.y, double(pos.z) + 0.5);
        }
    }
};

// ---------------------------------------------------------------- template pieces

// ---------------------------------------------------------------- igloo

const char* const kIglooTop = "minecraft:igloo/top";
const char* const kIglooLadder = "minecraft:igloo/middle";
const char* const kIglooLab = "minecraft:igloo/bottom";

BlockPos igloo_pivot(const std::string& loc) { return loc == kIglooTop ? BlockPos(3, 5, 5) : loc == kIglooLadder ? BlockPos(1, 3, 1) : BlockPos(3, 6, 7); }
BlockPos igloo_offset(const std::string& loc) { return loc == kIglooTop ? BlockPos(0, 0, 0) : loc == kIglooLadder ? BlockPos(2, -3, 4) : BlockPos(0, -3, -2); }

PlaceSettings igloo_settings(Rotation rotation, const std::string& loc) {
    PlaceSettings s;
    s.rotation = rotation;
    s.mirror = Mirror::NONE;
    s.pivot = igloo_pivot(loc);
    s.processors.push_back(block_ignore_processor(false, true));
    s.apply_waterlogging = false;   // LiquidSettings.IGNORE_WATERLOGGING
    return s;
}

class IglooPiece : public TemplatePiece {
public:
    IglooPiece(TemplateManager& tm, const std::string& loc, const BlockPos& position, Rotation rotation, int depth)
        : TemplatePiece("minecraft:iglu", 0, tm, loc, igloo_settings(rotation, loc), position.offset(igloo_offset(loc).x, igloo_offset(loc).y - depth, igloo_offset(loc).z)) {}

    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {
        PlaceSettings settings2 = igloo_settings(settings.rotation, templateName);
        BlockPos offset = igloo_offset(templateName);
        BlockPos rel = calculate_relative_position(settings2, BlockPos(3 - offset.x, 0, -offset.z));
        BlockPos entrance = templatePosition.offset(rel.x, rel.y, rel.z);
        int height = level.getHeight(WORLD_SURFACE_WG, entrance.x, entrance.z);
        BlockPos oldTemplatePos = templatePosition;
        templatePosition = templatePosition.offset(0, height - 90 - 1, 0);
        TemplatePiece::postProcess(level, random, chunkBB, chunkPos, referencePos);
        if (templateName == kIglooTop) {
            BlockPos r2 = calculate_relative_position(settings2, BlockPos(3, 0, 5));
            BlockPos trapDoorPos = templatePosition.offset(r2.x, r2.y, r2.z);
            BS below = level.getBlockState(trapDoorPos.below());
            if (!below.isAir() && !below.is("minecraft:ladder")) level.L.set(trapDoorPos, blk("minecraft:snow_block").id);
        }
        templatePosition = oldTemplatePos;
    }

protected:
    void handleDataMarker(const std::string& markerId, const BlockPos& position, JLevel& level, JRandom random, const BoundingBox&) override {
        if (markerId != "chest") return;
        level.L.set(position, blk("minecraft:air").id);
        setLootTable(level, position.below(), "minecraft:chests/igloo_chest", random.nextLong());
    }
};

class IglooStructure : public Structure {
public:
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        return on_top_of_chunk_center(ctx, WORLD_SURFACE_WG, [&ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
            JRandom random(ctx.random);
            BlockPos start(ctx.chunk.x * 16, 90, ctx.chunk.z * 16);
            static const Rotation kRots[4] = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
            Rotation rotation = kRots[random.nextInt(4)];
            TemplateManager& tm = *ctx.env.templates;
            if (random.nextDouble() < 0.5) {
                int depth = random.nextInt(8) + 4;
                out.push_back(std::make_shared<IglooPiece>(tm, kIglooLab, start, rotation, depth * 3));
                for (int i = 0; i < depth - 1; ++i) out.push_back(std::make_shared<IglooPiece>(tm, kIglooLadder, start, rotation, i * 3));
            }
            out.push_back(std::make_shared<IglooPiece>(tm, kIglooTop, start, rotation, 0));
        });
    }
};

// ---------------------------------------------------------------- buried treasure

class BuriedTreasurePiece : public LegacyPiece {
public:
    explicit BuriedTreasurePiece(const BlockPos& offset) {
        type = "minecraft:btp";
        gen_depth = 0;
        bb = BoundingBox(offset.x, offset.y, offset.z, offset.x, offset.y, offset.z);
    }
    static bool isLiquid(const BS& s) { return s.is("minecraft:water") || s.is("minecraft:lava"); }
    void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos&, const BlockPos&) override {
        int y = level.getHeight(OCEAN_FLOOR_WG, bb.x0, bb.z0);
        BlockPos pos(bb.x0, y, bb.z0);
        while (pos.y > level.getMinY()) {
            BS current = level.getBlockState(pos);
            BS below = level.getBlockState(pos.below());
            if (below.is("minecraft:sandstone") || below.is("minecraft:stone") || below.is("minecraft:andesite") || below.is("minecraft:granite") || below.is("minecraft:diorite")) {
                BS soft = current.isAir() || isLiquid(current) ? blk("minecraft:sand") : current;
                for (int d = 0; d < 6; d++) {
                    BlockPos rel(pos.x + kDirX[d], pos.y + kDirY[d], pos.z + kDirZ[d]);
                    BS relState = level.getBlockState(rel);
                    if (!relState.isAir() && !isLiquid(relState)) continue;
                    BS belowRel = level.getBlockState(rel.below());
                    if ((belowRel.isAir() || isLiquid(belowRel)) && d != UP) level.L.set(rel, below.id);
                    else level.L.set(rel, soft.id);
                }
                bb = BoundingBox(pos.x, pos.y, pos.z, pos.x, pos.y, pos.z);
                createChest(level, chunkBB, random, pos, "minecraft:chests/buried_treasure", nullptr);
                return;
            }
            pos = pos.below();
        }
    }
};

class BuriedTreasureStructure : public Structure {
public:
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        return on_top_of_chunk_center(ctx, OCEAN_FLOOR_WG, [&ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
            out.push_back(std::make_shared<BuriedTreasurePiece>(BlockPos(ctx.chunk.x * 16 + 9, 90, ctx.chunk.z * 16 + 9)));
        });
    }
};

// ---------------------------------------------------------------- nether fossil

PlaceSettings fossil_settings(Rotation rotation) {
    PlaceSettings s;
    s.rotation = rotation;
    s.mirror = Mirror::NONE;
    s.processors.push_back(block_ignore_processor(true, true));
    return s;
}

class NetherFossilPiece : public TemplatePiece {
public:
    NetherFossilPiece(TemplateManager& tm, const std::string& loc, const BlockPos& position, Rotation rotation) : TemplatePiece("minecraft:nefos", 0, tm, loc, fossil_settings(rotation), position) {}
    void post_process(PieceCtx& ctx, const BlockPos& pivot) override {
        if (!tmpl) return;
        BoundingBox fossilBB = tmpl->bounding_box(templatePosition, settings.rotation, settings.pivot, settings.mirror);
        ctx.chunk_bb.encapsulate(fossilBB);   // the chunk's writable area grows to the whole fossil
        TemplatePiece::post_process(ctx, pivot);
        JLevel level(ctx.level, ctx);
        placeDriedGhast(level, fossilBB, ctx.chunk_bb);
    }
private:
    void placeDriedGhast(JLevel& level, const BoundingBox& fossilBB, const BoundingBox& chunkBB) {
        JRandom positional = positionalRandom(level.getSeed(), fossilBB.center());
        if (!(positional.nextFloat() < 0.5f)) return;
        int x = fossilBB.x0 + positional.nextInt(fossilBB.x_span());
        int y = fossilBB.y0;
        int z = fossilBB.z0 + positional.nextInt(fossilBB.z_span());
        BlockPos randomPos(x, y, z);
        if (level.getBlockState(randomPos).isAir() && chunkBB.inside(randomPos)) {
            static const Rotation kRots[4] = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
            level.L.set(randomPos, blk("minecraft:dried_ghast").rotate(kRots[positional.nextInt(4)]).id);
        }
    }
};

class NetherFossilStructure : public Structure {
public:
    std::function<int(Rng&, int, int)> height;
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        JRandom random(ctx.random);
        int blockX = ctx.chunk.x * 16 + random.nextInt(16);
        int blockZ = ctx.chunk.z * 16 + random.nextInt(16);
        int seaLevel = ctx.env.sea_level;
        int y = height(ctx.random, ctx.env.min_y, ctx.env.height);
        std::vector<uint16_t> column;
        ctx.env.base_column(blockX, blockZ, column);
        auto block_at = [&](int yy) -> BS { int i = yy - ctx.env.min_y; return i >= 0 && size_t(i) < column.size() ? BS(column[size_t(i)]) : BS(0); };
        while (y > seaLevel) {
            BS current = block_at(y);
            BS below = block_at(--y);
            if (!current.isAir() || (!below.is("minecraft:soul_sand") && !below.isFaceSturdy(Direction::UP))) continue;
            break;
        }
        if (y <= seaLevel) return std::nullopt;
        BlockPos position(blockX, y, blockZ);
        GenerationStub stub;
        stub.position = position;
        stub.generate = [&ctx, position](std::vector<std::shared_ptr<StructurePiece>>& out) {
            JRandom random(ctx.random);
            static const Rotation kRots[4] = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
            Rotation rotation = kRots[random.nextInt(4)];
            char buf[64];
            snprintf(buf, sizeof buf, "minecraft:nether_fossils/fossil_%d", random.nextInt(14) + 1);
            out.push_back(std::make_shared<NetherFossilPiece>(*ctx.env.templates, buf, position, rotation));
        };
        return stub;
    }
};

struct RegisterScattered {
    RegisterScattered() {
        auto& f = structure_factories();
        f["minecraft:desert_pyramid"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<DesertPyramidStructure>(); };
        f["minecraft:jungle_temple"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<SinglePieceStructure<JungleTemplePiece>>(12, 15); };
        f["minecraft:swamp_hut"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> {
            struct SwampHutStructure : Structure {
                std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
                    return on_top_of_chunk_center(ctx, WORLD_SURFACE_WG, [&ctx](std::vector<std::shared_ptr<StructurePiece>>& out) {
                        out.push_back(std::make_shared<SwampHutPiece>(JRandom(ctx.random), ctx.chunk.x * 16, ctx.chunk.z * 16));
                    });
                }
            };
            return std::make_unique<SwampHutStructure>();
        };
        f["minecraft:igloo"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<IglooStructure>(); };
        f["minecraft:buried_treasure"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<BuriedTreasureStructure>(); };
        f["minecraft:nether_fossil"] = [](const Json& j, const Registry&) -> std::unique_ptr<Structure> {
            auto s = std::make_unique<NetherFossilStructure>();
            s->height = make_height_provider(j.at("height"));
            return s;
        };
    }
} register_scattered;

}  // namespace

}  // namespace pn::vg
