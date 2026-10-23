#include "entity_manager.hpp"
#include "data.hpp"
#include "loot.hpp"
#include "../cachestats.hpp"
#include "mob.hpp"
#include <array>
#include <set>
#include "../blocks.hpp"
#include "../dimension.hpp"
#include <algorithm>
#include <cmath>

namespace pn::sim {

static uint64_t uuid_seed(uint64_t world_seed, int dim) {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ world_seed ^ (uint64_t(dim + 1) * 0x9e3779b97f4a7c15ULL);
}

EntityManager::EntityManager(World& world, std::atomic<int64_t>& game_time, std::atomic<int>& next_id, int dim)
    : dim_(dim), world_(world), game_time_(game_time), next_id_(next_id),
      random_(static_cast<int64_t>(world.seed) ^ (static_cast<int64_t>(dim) * 0x9e3779b97f4a7c15LL)),
      uuid_random_(uuid_seed(world.seed, dim)) {}

uint16_t EntityManager::block_at(int x, int y, int z) const {
    auto chunk = world_.chunk_if_loaded(x >> 4, z >> 4);
    if (!chunk) return Blocks::get().air();
    std::lock_guard<std::mutex> lock(chunk->mu);
    return chunk->get(x & 15, y, z & 15);
}

bool EntityManager::chunk_loaded(int x, int z) const {
    return static_cast<bool>(world_.chunk_if_loaded(x >> 4, z >> 4));
}

float EntityManager::block_friction(int x, int y, int z) const {
    const BlockInfo& block = Blocks::get().info(block_at(x, y, z));
    const BlockProps* props = BlockTable::get().find(block.name);
    return props ? props->friction : 0.6f;
}

bool EntityManager::is_full_block(int x, int y, int z) const {
    return BlockCollisionShapes::get().is_full_block(block_at(x, y, z));
}

// Entity.updateFluidHeightAndDoFluidPushing's height: how far above the bottom of the box the fluid in the blocks it touches reaches (0 when dry).
// A fluid block is as high as its level says (source 8/9, flowing 8-level over 9, falling 1) unless the same fluid is above it, and a waterlogged block holds water.
double EntityManager::fluid_height(const AABB& box, bool lava) const {
    const Blocks& B = Blocks::get();
    const char* want = lava ? "minecraft:lava" : "minecraft:water";
    AABB b = box.deflate(0.001);
    int x0 = int(std::floor(b.min_x)), x1 = int(std::ceil(b.max_x)), y0 = int(std::floor(b.min_y)), y1 = int(std::ceil(b.max_y)), z0 = int(std::floor(b.min_z)), z1 = int(std::ceil(b.max_z));
    // the chunk of the last lookup is kept for the next one: a box almost always sits inside one or two chunks
    std::shared_ptr<Chunk> last;
    int lcx = INT32_MIN, lcz = INT32_MIN;
    auto state_at = [&](int x, int y, int z) -> uint16_t {
        int cx = x >> 4, cz = z >> 4;
        if (cx != lcx || cz != lcz) { last = world_.chunk_if_loaded(cx, cz); lcx = cx; lcz = cz; }
        if (!last) return 0;
        std::lock_guard<std::mutex> lock(last->mu);
        return last->get(x & 15, y, z & 15);
    };
    auto amount = [&](uint16_t state) -> int {   // FluidState amount 0..8 of the wanted fluid in a block state
        if (!state) return 0;
        const BlockInfo& bi = B.info(state);
        if (bi.air) return 0;
        if (bi.name == want) {
            size_t p = bi.props.find("level=");
            int lvl = p == std::string::npos ? 0 : atoi(bi.props.c_str() + p + 6);
            return lvl == 0 || lvl >= 8 ? 8 : 8 - lvl;
        }
        if (!lava && !bi.props.empty() && bi.props.find("waterlogged=true") != std::string::npos) return 8;
        return 0;
    };
    double result = 0.0;
    for (int y = y0; y < y1; ++y)
        for (int z = z0; z < z1; ++z)
            for (int x = x0; x < x1; ++x) {
                int a = amount(state_at(x, y, z));
                if (a == 0) continue;
                double h = amount(state_at(x, y + 1, z)) > 0 ? 1.0 : a / 9.0;   // FlowingFluid.getHeight: full when the same fluid is above
                double top = y + h;
                if (top >= box.min_y) result = std::max(result, top - box.min_y);
            }
    return result;
}

static bool falling_level(const std::string& props) {
    size_t p = props.find("level=");
    return p != std::string::npos && atoi(props.c_str() + p + 6) >= 8;
}

// FlowingFluid.getFlow summed over the fluid blocks the box touches (each weighted by how deep the box is in it), then normalized
Vec3 EntityManager::fluid_flow(const AABB& box, bool lava) const {
    const Blocks& B = Blocks::get();
    const char* want = lava ? "minecraft:lava" : "minecraft:water";
    AABB b = box.deflate(0.001);
    int x0 = int(std::floor(b.min_x)), x1 = int(std::ceil(b.max_x)), y0 = int(std::floor(b.min_y)), y1 = int(std::ceil(b.max_y)), z0 = int(std::floor(b.min_z)), z1 = int(std::ceil(b.max_z));
    auto amount = [&](int x, int y, int z) -> int {
        const BlockInfo& bi = B.info(block_at(x, y, z));
        if (bi.name == want) {
            size_t p = bi.props.find("level=");
            int lvl = p == std::string::npos ? 0 : atoi(bi.props.c_str() + p + 6);
            return lvl == 0 || lvl >= 8 ? 8 : 8 - lvl;
        }
        return !lava && bi.props.find("waterlogged=true") != std::string::npos ? 8 : 0;
    };
    static const int DX[4] = {0, 0, -1, 1}, DZ[4] = {-1, 1, 0, 0};
    Vec3 total;
    int count = 0;
    for (int y = y0; y < y1; ++y)
        for (int z = z0; z < z1; ++z)
            for (int x = x0; x < x1; ++x) {
                int a = amount(x, y, z);
                if (a == 0) continue;
                double h = a / 9.0, d = y + (amount(x, y + 1, z) > 0 ? 1.0 : h) - box.min_y;
                if (d < 0.001) continue;
                Vec3 flow;
                for (int k = 0; k < 4; k++) {
                    int nx = x + DX[k], nz = z + DZ[k];
                    int na = amount(nx, y, nz);
                    double f = 0.0;
                    if (na == 0) {
                        if (!B.info(block_at(nx, y, nz)).solid) {
                            int ba = amount(nx, y - 1, nz);
                            if (ba > 0) f = h - (ba / 9.0 - 0.8888889);
                        }
                    } else f = h - na / 9.0;
                    if (f != 0.0) flow = flow + Vec3(DX[k] * f, 0, DZ[k] * f);
                }
                if (a == 8 && falling_level(B.info(block_at(x, y, z)).props)) {   // falling fluid pulled down against walls
                    for (int k = 0; k < 4; k++)
                        if (B.info(block_at(x + DX[k], y, z + DZ[k])).solid || B.info(block_at(x + DX[k], y + 1, z + DZ[k])).solid) { flow = flow.normalize() + Vec3(0, -6.0, 0); break; }
                }
                flow = flow.normalize();
                if (d < 0.4) flow = flow.scale(d);
                total = total + flow;
                count++;
            }
    if (count == 0 || total.length_sqr() == 0.0) return Vec3();
    return total.scale(1.0 / count).normalize();
}

bool EntityManager::set_block_state(int x, int y, int z, uint16_t state) {
    if (!chunk_loaded(x, z) || !world_.set_block(x, y, z, state)) return false;
    if (on_block_changed) on_block_changed(x, y, z, state);
    return true;
}

void EntityManager::players_near(const Vec3& p, double range, std::vector<PlayerRef>& out) {
    for (const PlayerRef& r : players) {
        double dx = r.pos.x - p.x, dy = r.pos.y - p.y, dz = r.pos.z - p.z;
        if (dx * dx + dy * dy + dz * dz <= range * range) out.push_back(r);
    }
}

// LivingEntity.dropAllDeathLoot: the entity's loot table, rolled when it dies
void EntityManager::mob_died(Entity& e, bool by_player) {
    const LootTable* table = LootTables::get("minecraft:entities/" + e.type.substr(e.type.find(':') + 1));
    if (!table) return;
    LootEntityInfo info;
    info.type = e.type;
    LootParams lp;
    lp.this_entity = &info;
    lp.killed_by_player = by_player;
    lp.has_origin = true;
    lp.ox = e.pos.x; lp.oy = e.pos.y; lp.oz = e.pos.z;
    ItemStack none;
    lp.tool = &none;
    std::vector<ItemStack> out;
    table->get_random_items(lp, random().next_long(), out);
    for (ItemStack& st : out) {
        if (st.is_empty()) continue;
        auto item = std::make_unique<ItemEntity>(this, e.pos.x, e.pos.y + 0.5, e.pos.z, st);
        item->set_default_pickup_delay();
        add_entity(std::move(item));
    }
}

bool EntityManager::can_see_sky(int x, int y, int z) const {
    if (!chunk_loaded(x, z)) return false;
    return world_.top_y(x, z) < y;
}

// Explosion.explode: rays eat through blocks by their blast resistance, entities in range get hurt and pushed
void EntityManager::explode(const Vec3& at, float power, Entity* source) {
    const Blocks& B = Blocks::get();
    std::set<std::array<int, 3>> blocks;
    for (int j = 0; j < 16; ++j)
        for (int k = 0; k < 16; ++k)
            for (int l = 0; l < 16; ++l) {
                if (j != 0 && j != 15 && k != 0 && k != 15 && l != 0 && l != 15) continue;
                double dx = j / 15.0 * 2.0 - 1.0, dy = k / 15.0 * 2.0 - 1.0, dz = l / 15.0 * 2.0 - 1.0;
                double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= len; dy /= len; dz /= len;
                float intensity = power * (0.7f + random_.next_float() * 0.6f);
                double x = at.x, y = at.y, z = at.z;
                for (; intensity > 0.0f; intensity -= 0.22500001f) {
                    int bx = int(std::floor(x)), by = int(std::floor(y)), bz = int(std::floor(z));
                    if (chunk_loaded(bx, bz)) {
                        const BlockInfo& bi = B.info(block_at(bx, by, bz));
                        if (!bi.air) {
                            const BlockProps* bp = BlockTable::get().find(bi.name);
                            float resist = bp ? bp->explosion_resistance : 0.0f;
                            intensity -= (resist + 0.3f) * 0.3f;
                            if (intensity > 0.0f) blocks.insert({bx, by, bz});
                        }
                    }
                    x += dx * 0.3; y += dy * 0.3; z += dz * 0.3;
                }
            }
    double q = double(power) * 2.0;
    // Explosion.getSeenPercent: the share of a grid of points in the box that the blast can see without a block in between
    auto seen = [&](const AABB& box) {
        double dx = 1.0 / ((box.max_x - box.min_x) * 2.0 + 1.0), dy = 1.0 / ((box.max_y - box.min_y) * 2.0 + 1.0), dz = 1.0 / ((box.max_z - box.min_z) * 2.0 + 1.0);
        double ox = (1.0 - std::floor(1.0 / dx) * dx) / 2.0, oz = (1.0 - std::floor(1.0 / dz) * dz) / 2.0;
        int hit = 0, total = 0;
        for (double fx = 0.0; fx <= 1.0; fx += dx)
            for (double fy = 0.0; fy <= 1.0; fy += dy)
                for (double fz = 0.0; fz <= 1.0; fz += dz) {
                    Vec3 p(box.min_x + fx * (box.max_x - box.min_x) + ox, box.min_y + fy * (box.max_y - box.min_y), box.min_z + fz * (box.max_z - box.min_z) + oz);
                    Vec3 d = at - p;
                    int steps = std::max(1, int(d.length() / 0.3));
                    bool clear = true;
                    for (int i = 0; i <= steps && clear; ++i) {
                        Vec3 s = p + d.scale(double(i) / steps);
                        std::vector<AABB> hits;
                        block_collisions(AABB(s.x - 0.001, s.y - 0.001, s.z - 0.001, s.x + 0.001, s.y + 0.001, s.z + 0.001), hits);
                        clear = hits.empty();
                    }
                    hit += clear;
                    ++total;
                }
        return total ? double(hit) / total : 0.0;
    };
    auto damage_of = [&](double dist, double exposure) {
        double impact = (1.0 - dist / q) * exposure;
        return std::pair<float, double>(float(int((impact * impact + impact) / 2.0 * 7.0 * q + 1.0)), impact);
    };
    for (PlayerRef& pl : players) {   // players: the ones near enough (exposure is taken as 1)
        Vec3 c(pl.pos.x, pl.pos.y + 0.9, pl.pos.z);
        double dist = (c - at).length();
        if (dist > q || !pl.targetable) continue;
        double exposure = seen(AABB(pl.pos.x - 0.3, pl.pos.y, pl.pos.z - 0.3, pl.pos.x + 0.3, pl.pos.y + 1.8, pl.pos.z + 0.3));
        if (exposure <= 0.0) continue;
        auto d = damage_of(dist, exposure);
        Vec3 dir = (Vec3(pl.pos.x, pl.pos.y + 1.62, pl.pos.z) - at).normalize().scale(d.second);
        if (on_hurt_player) on_hurt_player(pl.gid, d.first, source ? "explosion:" + source->type : "explosion", dir);
    }
    for (auto& e : entities_) {
        if (e.get() == source || e->removed) continue;
        auto* le = dynamic_cast<LivingEntity*>(e.get());
        if (!le || le->dead) continue;
        double dist = (Vec3(le->pos.x, le->pos.y + le->height / 2, le->pos.z) - at).length();
        if (dist > q) continue;
        double exposure = seen(le->bounding_box());
        if (exposure <= 0.0) continue;
        auto d = damage_of(dist, exposure);
        le->hurt(d.first, &at);
        Vec3 dir = (Vec3(le->pos.x, le->pos.y + le->height * 0.9, le->pos.z) - at).normalize().scale(d.second * (1.0 - le->knockback_resistance));
        le->delta = le->delta + dir;
        le->velocity_dirty = true;
    }
    // destroyed blocks drop their loot, each with a 1 in `power` chance of surviving the blast (the loot tables' survives_explosion)
    int drops = 0;
    for (auto& b : blocks) {
        uint16_t state = block_at(b[0], b[1], b[2]);
        const BlockInfo& bi = B.info(state);
        set_block_state(b[0], b[1], b[2], B.air());
        if (drops >= 150 || bi.fluid || bi.air) continue;
        const LootTable* table = LootTables::get("minecraft:blocks/" + bi.name.substr(bi.name.find(':') + 1));
        if (!table) continue;
        LootBlockState bs;
        bs.name = bi.name;
        for (size_t p = 0; p < bi.props.size();) {
            size_t end = bi.props.find(',', p);
            if (end == std::string::npos) end = bi.props.size();
            size_t eq = bi.props.find('=', p);
            if (eq != std::string::npos && eq < end) bs.props[bi.props.substr(p, eq - p)] = bi.props.substr(eq + 1, end - eq - 1);
            p = end + 1;
        }
        ItemStack none;
        LootParams lp;
        lp.tool = &none;
        lp.block_state = &bs;
        lp.has_origin = true;
        lp.ox = b[0] + 0.5; lp.oy = b[1] + 0.5; lp.oz = b[2] + 0.5;
        lp.explosion_radius = power;
        std::vector<ItemStack> out;
        table->get_random_items(lp, random_.next_long(), out);
        for (ItemStack& st : out) {
            if (st.is_empty() || drops >= 150) continue;
            auto item = std::make_unique<ItemEntity>(this, b[0] + 0.5 + random_.next_double() * 0.5 - 0.25, b[1] + 0.5 - 0.125 + random_.next_double() * 0.5 - 0.25, b[2] + 0.5 + random_.next_double() * 0.5 - 0.25, st);
            item->set_default_pickup_delay();
            add_entity(std::move(item));
            ++drops;
        }
    }
}

int EntityManager::min_build_y() const { return dim_info(dim_).min_y; }
int EntityManager::max_build_y() const { return dim_info(dim_).min_y + dim_info(dim_).height; }

void EntityManager::block_collisions(const AABB& area, std::vector<AABB>& out) const {
    const int min_x = static_cast<int>(std::floor(area.min_x));
    const int min_y = static_cast<int>(std::floor(area.min_y));
    const int min_z = static_cast<int>(std::floor(area.min_z));
    const int max_x = static_cast<int>(std::ceil(area.max_x)) - 1;
    const int max_y = static_cast<int>(std::ceil(area.max_y)) - 1;
    const int max_z = static_cast<int>(std::ceil(area.max_z)) - 1;
    // one chunk lookup and one lock per chunk, not per block: this runs several times for every moving entity every tick
    const BlockCollisionShapes& shapes = BlockCollisionShapes::get();
    for (int cz = min_z >> 4; cz <= max_z >> 4; ++cz)
        for (int cx = min_x >> 4; cx <= max_x >> 4; ++cx) {
            auto chunk = world_.chunk_if_loaded(cx, cz);
            if (!chunk) continue;   // unloaded chunks are air
            std::lock_guard<std::mutex> lock(chunk->mu);
            int x0 = std::max(min_x, cx << 4), x1 = std::min(max_x, (cx << 4) + 15), z0 = std::max(min_z, cz << 4), z1 = std::min(max_z, (cz << 4) + 15);
            for (int y = min_y; y <= max_y; ++y)
                for (int z = z0; z <= z1; ++z)
                    for (int x = x0; x <= x1; ++x) {
                        uint16_t state = chunk->get(x & 15, y, z & 15);
                        if (!state) continue;
                        for (const AABB& box : shapes.boxes(state)) out.push_back(box.move(x, y, z));
                    }
        }
}

void EntityManager::entities_in_box(const AABB& box, const Entity* except, std::vector<Entity*>& out) {
    for (const auto& entity : entities_)
        if (entity.get() != except && !entity->removed && entity->bounding_box().intersects(box))
            out.push_back(entity.get());
}

Entity* EntityManager::add_entity(std::unique_ptr<Entity> entity) {
    if (!entity) return nullptr;
    entity->level = this;
    if (const EntityProps* props = EntityTable::get().find(entity->type)) {
        entity->width = props->width;
        entity->height = props->height;
        entity->eye_height = props->eye_height;
    }
    entity->id = next_id_++;
    for (int i = 0; i < 16; i += 8) {
        uint64_t value = uuid_random_();
        for (int j = 0; j < 8; ++j) entity->uuid[i + j] = static_cast<uint8_t>(value >> (j * 8));
    }
    entity->uuid[6] = static_cast<uint8_t>((entity->uuid[6] & 0x0f) | 0x40);
    entity->uuid[8] = static_cast<uint8_t>((entity->uuid[8] & 0x3f) | 0x80);
    Entity* result = entity.get();
    entities_.push_back(std::move(entity));
    return result;
}

void EntityManager::tick() {
    {   // what the path searches of the last tick kept, for the `cache` command; then start over
        static CacheStat& st = cache_stat("path cells (per tick)");
        int64_t n = 0;
        for (auto& kv : cell_caches_) n += int64_t(kv.second.clear.size() + kv.second.stand.size());
        if (n) { st.entries.store(n, std::memory_order_relaxed); st.bytes.store(n * 48, std::memory_order_relaxed); }
    }
    for (auto& kv : cell_caches_) { kv.second.clear.clear(); kv.second.stand.clear(); }   // blocks may have changed since the last tick
    const size_t tick_count = entities_.size();
    for (size_t i = 0; i < tick_count; ++i) {
        Entity* entity = entities_[i].get();
        if (entity->removed || !chunk_loaded(entity->block_x(), entity->block_z())) continue;
        entity->tick();
    }
    entities_.erase(std::remove_if(entities_.begin(), entities_.end(), [](const auto& entity) { return entity->removed; }), entities_.end());
}

}  // namespace pn::sim
