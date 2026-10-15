#include "legacy_piece.hpp"
#include <map>
#include <mutex>
#include <set>

namespace pn::vg::jc {

static const BlockData* g_bd = nullptr;
const BlockData& bd() { return *g_bd; }
void set_bd(const BlockData* b) { g_bd = b; }

const Direction Direction::DOWN(pn::vg::DOWN), Direction::UP(pn::vg::UP), Direction::NORTH(pn::vg::NORTH), Direction::SOUTH(pn::vg::SOUTH), Direction::WEST(pn::vg::WEST), Direction::EAST(pn::vg::EAST);
const Direction Direction::HORIZONTAL[4] = {Direction(pn::vg::NORTH), Direction(pn::vg::EAST), Direction(pn::vg::SOUTH), Direction(pn::vg::WEST)};

Direction Direction::getClockWise() const {
    switch (v) { case pn::vg::NORTH: return EAST; case pn::vg::EAST: return SOUTH; case pn::vg::SOUTH: return WEST; case pn::vg::WEST: return NORTH; default: return *this; }
}
Direction Direction::getCounterClockWise() const {
    switch (v) { case pn::vg::NORTH: return WEST; case pn::vg::WEST: return SOUTH; case pn::vg::SOUTH: return EAST; case pn::vg::EAST: return NORTH; default: return *this; }
}
int Direction::get2DDataValue() const {
    switch (v) { case pn::vg::SOUTH: return 0; case pn::vg::WEST: return 1; case pn::vg::NORTH: return 2; case pn::vg::EAST: return 3; default: return -1; }
}
Direction Direction::from2DDataValue(int i) {
    static const Direction by[4] = {Direction(pn::vg::SOUTH), Direction(pn::vg::WEST), Direction(pn::vg::NORTH), Direction(pn::vg::EAST)};
    return by[std::abs(i % 4)];
}
const char* Direction::getName() const {
    static const char* n[6] = {"down", "up", "north", "south", "west", "east"};
    return n[v];
}

bool JLevel::biomeIs(const BlockPos& p, const Tag& tag) const {
    int b = L.biome(p);
    const std::vector<std::string>& names = *ctx.senv.biome_names;
    if (b < 0 || size_t(b) >= names.size()) return false;
    static std::mutex mu;
    static std::map<std::string, std::set<std::string>> cache;
    std::lock_guard<std::mutex> l(mu);
    auto it = cache.find(tag.id);
    if (it == cache.end()) {
        std::set<std::string> members;
        std::function<void(const std::string&, int)> collect = [&](const std::string& id, int depth) {
            if (depth > 8) return;
            const Json& j = ctx.senv.reg->tag("biome/" + Registry::strip(id));
            if (const Json* v = j.find("values"))
                for (auto& e : v->arr) {
                    const std::string& s = e.is_str() ? e.str : e.get_string("id");
                    if (!s.empty() && s[0] == '#') collect(s.substr(1), depth + 1);
                    else members.insert(s.rfind("minecraft:", 0) == 0 ? s : "minecraft:" + s);
                }
        };
        collect(tag.id[0] == '#' ? tag.id.substr(1) : tag.id, 0);
        it = cache.emplace(tag.id, std::move(members)).first;
    }
    return it->second.count(names[size_t(b)]) != 0;
}

void LegacyPiece::post_process(PieceCtx& ctx, const BlockPos& pivot) {
    JLevel level(ctx.level, ctx);
    postProcess(level, JRandom(ctx.random), ctx.chunk_bb, ctx.chunk, pivot);
}

void LegacyPiece::setOrientation(std::optional<Direction> d) {
    has_orientation_ = d.has_value();
    if (!d) { rotation = Rotation::NONE; mirror = Mirror::NONE; return; }
    orientation_ = *d;
    orientation = d->v;
    switch (d->v) {
        case pn::vg::SOUTH: mirror = Mirror::LEFT_RIGHT; rotation = Rotation::NONE; break;
        case pn::vg::WEST: mirror = Mirror::LEFT_RIGHT; rotation = Rotation::CLOCKWISE_90; break;
        case pn::vg::EAST: mirror = Mirror::NONE; rotation = Rotation::CLOCKWISE_90; break;
        default: mirror = Mirror::NONE; rotation = Rotation::NONE; break;
    }
}

BoundingBox LegacyPiece::makeBoundingBox(int x, int y, int z, const Direction& direction, int width, int height, int depth) {
    if (direction.axis() == 'z') return BoundingBox(x, y, z, x + width - 1, y + height - 1, z + depth - 1);
    return BoundingBox(x, y, z, x + depth - 1, y + height - 1, z + width - 1);
}

bool LegacyPiece::isCloseToChunk(const ChunkPos& pos, int distance) const {
    int cx = pos.x * 16, cz = pos.z * 16;
    return bb.intersects(cx - distance, cz - distance, cx + 15 + distance, cz + 15 + distance);
}

int LegacyPiece::getWorldX(int x, int z) const {
    if (!has_orientation_) return x;
    switch (orientation_.v) {
        case pn::vg::NORTH: case pn::vg::SOUTH: return bb.x0 + x;
        case pn::vg::WEST: return bb.x1 - z;
        case pn::vg::EAST: return bb.x0 + z;
        default: return x;
    }
}
int LegacyPiece::getWorldY(int y) const { return has_orientation_ ? y + bb.y0 : y; }
int LegacyPiece::getWorldZ(int x, int z) const {
    if (!has_orientation_) return z;
    switch (orientation_.v) {
        case pn::vg::NORTH: return bb.z1 - z;
        case pn::vg::SOUTH: return bb.z0 + z;
        case pn::vg::WEST: case pn::vg::EAST: return bb.z0 + x;
        default: return z;
    }
}

void LegacyPiece::placeBlock(JLevel& level, const BS& state_in, int x, int y, int z, const BoundingBox& chunkBB) {
    BlockPos pos = getWorldPos(x, y, z);
    if (!chunkBB.inside(pos)) return;
    if (!canBeReplaced(level, x, y, z, chunkBB)) return;
    BS state = state_in;
    if (mirror != Mirror::NONE) state = state.mirror(mirror);
    if (rotation != Rotation::NONE) state = state.rotate(rotation);
    level.L.set(pos, state.id);
    level.L.remove_block_entity(pos);
    // SHAPE_CHECK_BLOCKS: fences, torches, ladders and iron bars are shape-updated once the chunk is complete
    const std::string& n = bd().name(state.id);
    if (n == "minecraft:nether_brick_fence" || n == "minecraft:torch" || n == "minecraft:wall_torch" || n.size() > 6 && n.compare(n.size() - 6, 6, "_fence") == 0 && n.find("fence_gate") == std::string::npos ||
        n == "minecraft:ladder" || n == "minecraft:iron_bars")
        level.L.mark_for_post_processing(pos);
}

BS LegacyPiece::getBlock(JLevel& level, int x, int y, int z, const BoundingBox& chunkBB) const {
    BlockPos pos = getWorldPos(x, y, z);
    if (!chunkBB.inside(pos)) return BS(0);
    return level.getBlockState(pos);
}

bool LegacyPiece::isInterior(JLevel& level, int x, int y, int z, const BoundingBox& chunkBB) const {
    BlockPos pos = getWorldPos(x, y + 1, z);
    if (!chunkBB.inside(pos)) return false;
    return pos.y < level.getHeight(OCEAN_FLOOR_WG, pos.x, pos.z);
}

void LegacyPiece::generateAirBox(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1) {
    BS air = blk("minecraft:air");
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) placeBlock(level, air, x, y, z, chunkBB);
}

void LegacyPiece::generateBox(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, const BS& edge, const BS& fill, bool skipAir) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) {
                if (skipAir && getBlock(level, x, y, z, chunkBB).isAir()) continue;
                if (y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1) placeBlock(level, edge, x, y, z, chunkBB);
                else placeBlock(level, fill, x, y, z, chunkBB);
            }
}

void LegacyPiece::generateBox(JLevel& level, const BoundingBox& chunkBB, const BoundingBox& box, const BS& edge, const BS& fill, bool skipAir) {
    generateBox(level, chunkBB, box.x0, box.y0, box.z0, box.x1, box.y1, box.z1, edge, fill, skipAir);
}

void LegacyPiece::generateBox(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, bool skipAir, JRandom random, BlockSelector& selector) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) {
                if (skipAir && getBlock(level, x, y, z, chunkBB).isAir()) continue;
                selector.next(random, x, y, z, y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1);
                placeBlock(level, selector.getNext(), x, y, z, chunkBB);
            }
}

void LegacyPiece::generateBox(JLevel& level, const BoundingBox& chunkBB, const BoundingBox& box, bool skipAir, JRandom random, BlockSelector& selector) {
    generateBox(level, chunkBB, box.x0, box.y0, box.z0, box.x1, box.y1, box.z1, skipAir, random, selector);
}

void LegacyPiece::generateMaybeBox(JLevel& level, const BoundingBox& chunkBB, JRandom random, float probability, int x0, int y0, int z0, int x1, int y1, int z1, const BS& edge, const BS& fill, bool skipAir, bool hasToBeInside) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) {
                if (random.nextFloat() > probability || (skipAir && getBlock(level, x, y, z, chunkBB).isAir()) || (hasToBeInside && !isInterior(level, x, y, z, chunkBB))) continue;
                if (y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1) placeBlock(level, edge, x, y, z, chunkBB);
                else placeBlock(level, fill, x, y, z, chunkBB);
            }
}

void LegacyPiece::maybeGenerateBlock(JLevel& level, const BoundingBox& chunkBB, JRandom random, float probability, int x, int y, int z, const BS& state) {
    if (random.nextFloat() < probability) placeBlock(level, state, x, y, z, chunkBB);
}

void LegacyPiece::generateUpperHalfSphere(JLevel& level, const BoundingBox& chunkBB, int x0, int y0, int z0, int x1, int y1, int z1, const BS& fill, bool skipAir) {
    float diagX = float(x1 - x0 + 1), diagY = float(y1 - y0 + 1), diagZ = float(z1 - z0 + 1);
    float cx = float(x0) + diagX / 2.0f, cz = float(z0) + diagZ / 2.0f;
    for (int y = y0; y <= y1; ++y) {
        float ny = float(y - y0) / diagY;
        for (int x = x0; x <= x1; ++x) {
            float nx = (float(x) - cx) / (diagX * 0.5f);
            for (int z = z0; z <= z1; ++z) {
                float nz = (float(z) - cz) / (diagZ * 0.5f);
                if (skipAir && getBlock(level, x, y, z, chunkBB).isAir()) continue;
                float dist = nx * nx + ny * ny + nz * nz;
                if (!(dist <= 1.05f)) continue;
                placeBlock(level, fill, x, y, z, chunkBB);
            }
        }
    }
}

bool LegacyPiece::isReplaceableByStructures(const BS& state) const {
    return state.isAir() || state.liquid() || state.is("minecraft:glow_lichen") || state.is("minecraft:seagrass") || state.is("minecraft:tall_seagrass");
}

void LegacyPiece::fillColumnDown(JLevel& level, const BS& state, int x, int startY, int z, const BoundingBox& chunkBB) {
    BlockPos pos = getWorldPos(x, startY, z);
    if (!chunkBB.inside(pos)) return;
    while (isReplaceableByStructures(level.getBlockState(pos)) && pos.y > level.getMinY() + 1) {
        level.L.set(pos, state.id);
        pos = BlockPos(pos.x, pos.y - 1, pos.z);
    }
}

void LegacyPiece::setLootTable(JLevel& level, const BlockPos& pos, const std::string& lootTable, int64_t seed) {
    ChunkBlocks* c = level.L.chunk_for(pos.x, pos.z);
    if (!c) return;
    auto it = c->block_entities.find({pos.x, pos.y, pos.z});
    if (it == c->block_entities.end()) return;
    it->second.put("LootTable", pn::Nbt::str(lootTable));
    it->second.put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, seed));
}

BS LegacyPiece::reorient(JLevel& level, const BlockPos& pos, BS state) {
    std::optional<Direction> solid;
    bool multiple = false;
    for (const Direction& d : Direction::HORIZONTAL) {
        BlockPos rel(pos.x + d.getStepX(), pos.y, pos.z + d.getStepZ());
        BS s = level.getBlockState(rel);
        if (s.is("minecraft:chest")) return state;
        if (!s.isSolidRender()) continue;
        if (!solid) { solid = d; continue; }
        solid.reset();
        multiple = true;
        break;
    }
    (void)multiple;
    if (solid) return state.setValue("facing", solid->getOpposite());
    Direction lock = Direction::NORTH;
    {
        std::string f = state.getValue("facing");
        for (const Direction& d : Direction::HORIZONTAL) if (f == d.getName()) lock = d;
    }
    BlockPos rel(pos.x + lock.getStepX(), pos.y, pos.z + lock.getStepZ());
    if (level.getBlockState(rel).isSolidRender()) { lock = lock.getOpposite(); rel = BlockPos(pos.x + lock.getStepX(), pos.y, pos.z + lock.getStepZ()); }
    if (level.getBlockState(rel).isSolidRender()) { lock = lock.getClockWise(); rel = BlockPos(pos.x + lock.getStepX(), pos.y, pos.z + lock.getStepZ()); }
    if (level.getBlockState(rel).isSolidRender()) { lock = lock.getOpposite(); }
    return state.setValue("facing", lock);
}

bool LegacyPiece::createChest(JLevel& level, const BoundingBox& chunkBB, JRandom random, int x, int y, int z, const std::string& lootTable) {
    return createChest(level, chunkBB, random, getWorldPos(x, y, z), lootTable, nullptr);
}

bool LegacyPiece::createChest(JLevel& level, const BoundingBox& chunkBB, JRandom random, const BlockPos& pos, const std::string& lootTable, const BS* state) {
    if (!chunkBB.inside(pos) || level.getBlockState(pos).is("minecraft:chest")) return false;
    BS st = state ? *state : reorient(level, pos, blk("minecraft:chest"));
    level.L.set(pos, st.id);
    pn::Nbt be = pn::Nbt::compound();
    be.put("id", pn::Nbt::str("minecraft:chest"));
    be.put("x", pn::Nbt::make(pn::Nbt::Int, pos.x));
    be.put("y", pn::Nbt::make(pn::Nbt::Int, pos.y));
    be.put("z", pn::Nbt::make(pn::Nbt::Int, pos.z));
    be.put("LootTable", pn::Nbt::str(lootTable));
    be.put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, random.nextLong()));
    level.L.set_block_entity(pos, std::move(be));
    return true;
}

bool LegacyPiece::createDispenser(JLevel& level, const BoundingBox& chunkBB, JRandom random, int x, int y, int z, const Direction& facing, const std::string& lootTable) {
    BlockPos pos = getWorldPos(x, y, z);
    if (!chunkBB.inside(pos) || level.getBlockState(pos).is("minecraft:dispenser")) return false;
    placeBlock(level, blk("minecraft:dispenser").setValue("facing", facing), x, y, z, chunkBB);
    pn::Nbt be = pn::Nbt::compound();
    be.put("id", pn::Nbt::str("minecraft:dispenser"));
    be.put("x", pn::Nbt::make(pn::Nbt::Int, pos.x));
    be.put("y", pn::Nbt::make(pn::Nbt::Int, pos.y));
    be.put("z", pn::Nbt::make(pn::Nbt::Int, pos.z));
    be.put("LootTable", pn::Nbt::str(lootTable));
    be.put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, random.nextLong()));
    level.L.set_block_entity(pos, std::move(be));
    return true;
}

}  // namespace pn::vg::jc
