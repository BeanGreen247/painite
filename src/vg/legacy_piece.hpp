// StructurePiece as Mojang's code-built pieces (mineshafts, strongholds, temples, fortresses, ...) use it: an orientation that maps local coordinates to the
// world, and the box and block helpers (generateBox, placeBlock, createChest, ...). Method names follow the Java so ported code stays recognisable.
#pragma once
#include "jcompat.hpp"
#include <optional>

namespace pn::vg::jc {

// StructurePieceAccessor / StructurePiecesBuilder
struct PieceAccessor {
    std::vector<std::shared_ptr<StructurePiece>>& pieces;
    explicit PieceAccessor(std::vector<std::shared_ptr<StructurePiece>>& p) : pieces(p) {}
    void addPiece(std::shared_ptr<StructurePiece> p) { pieces.push_back(std::move(p)); }
    StructurePiece* findCollisionPiece(const BoundingBox& box) const {
        for (auto& p : pieces) if (p->bb.intersects(box)) return p.get();
        return nullptr;
    }
    BoundingBox boundingBox() const {
        BoundingBox b = BoundingBox::infinite();
        for (auto& p : pieces) b.encapsulate(p->bb);
        return b;
    }
    void offsetPiecesVertically(int dy) { for (auto& p : pieces) p->move(0, dy, 0); }
    // StructurePiecesBuilder.moveBelowSeaLevel
    int moveBelowSeaLevel(int seaLevel, int minY, Rng& random, int offset) {
        int maxY = seaLevel - offset;
        BoundingBox box = boundingBox();
        int y1Pos = box.y_span() + minY + 1;
        if (y1Pos < maxY) y1Pos += random.next_int(maxY - y1Pos);
        int dy = y1Pos - box.y1;
        offsetPiecesVertically(dy);
        return dy;
    }
    // StructurePiecesBuilder.moveInsideHeights
    void moveInsideHeights(Rng& random, int lowestAllowed, int highestAllowed) {
        BoundingBox box = boundingBox();
        int heightSpan = highestAllowed - lowestAllowed + 1 - box.y_span();
        int y0Pos = heightSpan > 1 ? lowestAllowed + random.next_int(heightSpan) : lowestAllowed;
        offsetPiecesVertically(y0Pos - box.y0);
    }
};

// BoundingBox.orientBox: a box of width x height x depth in front of a foot position, turned to face `direction`
inline BoundingBox orientBox(int footX, int footY, int footZ, int offX, int offY, int offZ, int width, int height, int depth, const Direction& direction) {
    switch (direction.v) {
        case pn::vg::NORTH: return BoundingBox(footX + offX, footY + offY, footZ - depth + 1 + offZ, footX + width - 1 + offX, footY + height - 1 + offY, footZ + offZ);
        case pn::vg::WEST: return BoundingBox(footX - depth + 1 + offZ, footY + offY, footZ + offX, footX + offZ, footY + height - 1 + offY, footZ + width - 1 + offX);
        case pn::vg::EAST: return BoundingBox(footX + offZ, footY + offY, footZ + offX, footX + depth - 1 + offZ, footY + height - 1 + offY, footZ + width - 1 + offX);
        default: return BoundingBox(footX + offX, footY + offY, footZ + offZ, footX + width - 1 + offX, footY + height - 1 + offY, footZ + depth - 1 + offZ);
    }
}

// BlockSelector
struct BlockSelector {
    BS next_;
    virtual ~BlockSelector() = default;
    virtual void next(JRandom& random, int x, int y, int z, bool edge) = 0;
    BS getNext() const { return next_; }
};

class LegacyPiece : public StructurePiece {
public:
    // StructurePiece.postProcess
    void post_process(PieceCtx& ctx, const BlockPos& pivot) override;
    virtual void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) = 0;
    int getGenDepth() const { return gen_depth; }
    void setGenDepth(int d) { gen_depth = d; }
    std::optional<Direction> getOrientation() const { return has_orientation_ ? std::optional<Direction>(orientation_) : std::nullopt; }
    void setOrientation(std::optional<Direction> d);
    BoundingBox& boundingBox() { return bb; }
    const BoundingBox& boundingBox() const { return bb; }
    static BoundingBox makeBoundingBox(int x, int y, int z, const Direction& direction, int width, int height, int depth);
    virtual bool isCloseToChunk(const ChunkPos& pos, int distance) const;
    // addChildren(startPiece, accessor, random): the structure's own recursion
    virtual void addChildren(StructurePiece* startPiece, PieceAccessor& accessor, JRandom random) {}

protected:
    int getWorldX(int x, int z) const;
    int getWorldY(int y) const;
    int getWorldZ(int x, int z) const;
    BlockPos getWorldPos(int x, int y, int z) const { return BlockPos(getWorldX(x, z), getWorldY(y), getWorldZ(x, z)); }
    virtual bool canBeReplaced(JLevel& level, int x, int y, int z, const BoundingBox& chunkBB) { return true; }
    void placeBlock(JLevel& level, const BS& state, int x, int y, int z, const BoundingBox& chunkBB);
    BS getBlock(JLevel& level, int x, int y, int z, const BoundingBox& chunkBB) const;
    bool isInterior(JLevel& level, int x, int y, int z, const BoundingBox& chunkBB) const;
    void generateAirBox(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1);
    void generateBox(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, const BS& edge, const BS& fill, bool skipAir);
    void generateBox(JLevel& level, const BoundingBox& chunkBB, const BoundingBox& box, const BS& edge, const BS& fill, bool skipAir);
    void generateBox(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, bool skipAir, JRandom random, BlockSelector& selector);
    void generateBox(JLevel& level, const BoundingBox& chunkBB, const BoundingBox& box, bool skipAir, JRandom random, BlockSelector& selector);
    void generateMaybeBox(JLevel& level, const BoundingBox& chunkBB, JRandom random, float probability, int x0, int y0, int z0, int x1, int y1, int z1, const BS& edge, const BS& fill, bool skipAir, bool hasToBeInside);
    void maybeGenerateBlock(JLevel& level, const BoundingBox& chunkBB, JRandom random, float probability, int x, int y, int z, const BS& state);
    void generateUpperHalfSphere(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, const BS& fill, bool skipAir);
    virtual void fillColumnDown(JLevel& level, const BS& state, int x, int startY, int z, const BoundingBox& chunkBB);
    bool isReplaceableByStructures(const BS& state) const;
    virtual bool createChest(JLevel& level, const BoundingBox& chunkBB, JRandom random, int x, int y, int z, const std::string& lootTable);
    bool createChest(JLevel& level, const BoundingBox& chunkBB, JRandom random, const BlockPos& pos, const std::string& lootTable, const BS* state);
    bool createDispenser(JLevel& level, const BoundingBox& chunkBB, JRandom random, int x, int y, int z, const Direction& facing, const std::string& lootTable);
    static BS reorient(JLevel& level, const BlockPos& pos, BS state);
    // a block entity of the block at `pos` that holds a loot table
    void setLootTable(JLevel& level, const BlockPos& pos, const std::string& lootTable, int64_t seed);
    // a spawner block entity for the given entity type
    void setSpawnerEntity(JLevel& level, const BlockPos& pos, const std::string& entityId);
    // random block of the level at hand (WorldGenLevel.getRandom)
    JRandom levelRandom(JLevel& level) { return JRandom(level.ctx.random); }

    bool has_orientation_ = false;
    Direction orientation_;
};

}  // namespace pn::vg::jc
