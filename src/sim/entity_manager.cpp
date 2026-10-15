#include "entity_manager.hpp"
#include "data.hpp"
#include "../blocks.hpp"
#include <algorithm>
#include <cmath>

namespace pn::sim {

static uint64_t uuid_seed(uint64_t world_seed, int dim) {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ world_seed ^ (uint64_t(dim + 1) * 0x9e3779b97f4a7c15ULL);
}

EntityManager::EntityManager(World& world, std::atomic<int64_t>& game_time, std::atomic<int>& next_id, int dim)
    : world_(world), game_time_(game_time), next_id_(next_id),
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

void EntityManager::block_collisions(const AABB& area, std::vector<AABB>& out) const {
    const int min_x = static_cast<int>(std::floor(area.min_x));
    const int min_y = static_cast<int>(std::floor(area.min_y));
    const int min_z = static_cast<int>(std::floor(area.min_z));
    const int max_x = static_cast<int>(std::ceil(area.max_x)) - 1;
    const int max_y = static_cast<int>(std::ceil(area.max_y)) - 1;
    const int max_z = static_cast<int>(std::ceil(area.max_z)) - 1;
    for (int y = min_y; y <= max_y; ++y) {
        for (int z = min_z; z <= max_z; ++z) {
            for (int x = min_x; x <= max_x; ++x) {
                uint16_t state = block_at(x, y, z);
                for (const AABB& box : BlockCollisionShapes::get().boxes(state))
                    out.push_back(box.move(x, y, z));
            }
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
    const size_t tick_count = entities_.size();
    for (size_t i = 0; i < tick_count; ++i) {
        Entity* entity = entities_[i].get();
        if (entity->removed || !chunk_loaded(entity->block_x(), entity->block_z())) continue;
        entity->tick();
    }
    entities_.erase(std::remove_if(entities_.begin(), entities_.end(), [](const auto& entity) { return entity->removed; }), entities_.end());
}

}  // namespace pn::sim
