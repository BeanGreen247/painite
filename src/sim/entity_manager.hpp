// Per-dimension entity storage and the world queries used by simulated entities.
#pragma once
#include "entity.hpp"
#include "../world.hpp"
#include <atomic>
#include <functional>
#include <random>
#include <unordered_map>

namespace pn::sim {

class BlockCollisionShapes {
public:
    static BlockCollisionShapes& get();
    bool load(const std::string& states_path, const std::string& shapes_path, std::string& err);
    const std::vector<AABB>& boxes(uint16_t state) const;
    bool is_full_block(uint16_t state) const;

private:
    std::unordered_map<uint16_t, int> state_shapes_;
    std::unordered_map<int, std::vector<AABB>> shapes_;
    static const std::vector<AABB> empty_;
};

class EntityManager : public EntityWorld {
public:
    EntityManager(World& world, std::atomic<int64_t>& game_time, std::atomic<int>& next_id, int dim);

    int64_t game_time() const override { return game_time_; }
    vg::Rng& random() override { return random_; }
    float block_friction(int x, int y, int z) const override;
    bool is_full_block(int x, int y, int z) const override;
    double fluid_height(const AABB& box, bool lava) const override;
    uint16_t block_state(int x, int y, int z) const override { return block_at(x, y, z); }
    bool set_block_state(int x, int y, int z, uint16_t state) override;
    int min_build_y() const override;
    int max_build_y() const override;
    std::function<void(int, int, int, uint16_t)> on_block_changed;   // set by the game: sends the change to the players
    void block_collisions(const AABB& area, std::vector<AABB>& out) const override;
    void entities_in_box(const AABB& box, const Entity* except, std::vector<Entity*>& out) override;
    Entity* add_entity(std::unique_ptr<Entity> entity) override;

    void tick();
    const std::vector<std::unique_ptr<Entity>>& entities() const { return entities_; }

private:
    uint16_t block_at(int x, int y, int z) const;
    bool chunk_loaded(int x, int z) const;

    int dim_ = 0;
    World& world_;
    std::atomic<int64_t>& game_time_;
    std::atomic<int>& next_id_;
    vg::LegacyRandom random_;
    std::mt19937_64 uuid_random_;
    std::vector<std::unique_ptr<Entity>> entities_;
};

}  // namespace pn::sim
