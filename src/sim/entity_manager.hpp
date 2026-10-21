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
    const std::vector<AABB>& boxes(uint16_t state) const { return state < flat_.size() && flat_[state] ? *flat_[state] : empty_; }
    bool is_full_block(uint16_t state) const { return state < flat_full_.size() && flat_full_[state]; }
    size_t states() const { return state_shapes_.size(); }
    size_t memory_bytes() const { return flat_.size() * sizeof(void*) + flat_full_.size() + state_shapes_.size() * 24 + shapes_.size() * 64; }

private:
    std::unordered_map<uint16_t, int> state_shapes_;
    std::vector<const std::vector<AABB>*> flat_;   // by state id: the boxes, null for no collision (the hot path of every entity move)
    std::vector<uint8_t> flat_full_;
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
    void players_near(const Vec3& p, double range, std::vector<PlayerRef>& out) override;
    void hurt_player(uint64_t gid, float amount, const std::string& source, const Vec3& knockback) override { if (on_hurt_player) on_hurt_player(gid, amount, source, knockback); }
    void mob_died(Entity& e, bool by_player) override;
    bool can_see_sky(int x, int y, int z) const override;
    void explode(const Vec3& at, float power, Entity* source) override;
    int difficulty() const override { return difficulty_value; }
    int64_t day_time() const override { return day_time_value; }
    std::vector<PlayerRef> players;                  // set by the game once a tick
    int difficulty_value = 2;
    int64_t day_time_value = 0;
    std::function<void(uint64_t, float, const std::string&, const Vec3&)> on_hurt_player;
    std::function<void(uint64_t, const Vec3&)> on_teleport_owner;
    CellCache* cell_cache(int size_key) override { return &cell_caches_[size_key]; }
    void teleport_owner(uint64_t gid, const Vec3& to) override { if (on_teleport_owner) on_teleport_owner(gid, to); }
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
    std::unordered_map<int, CellCache> cell_caches_;   // by body size; emptied every tick
    World& world_;
    std::atomic<int64_t>& game_time_;
    std::atomic<int>& next_id_;
    vg::LegacyRandom random_;
    std::mt19937_64 uuid_random_;
    std::vector<std::unique_ptr<Entity>> entities_;
};

}  // namespace pn::sim
