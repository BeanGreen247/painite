// Entities (net.minecraft.world.entity.Entity and the ones Painite has so far), ticked by the game on its own thread.
#pragma once
#include "item.hpp"
#include "phys.hpp"
#include "../vg/random.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn::sim {

class Entity;

// "Can a body of this size stand / fit in this cell": the same answers are asked for by every path search of a tick, so they are kept for the tick
struct CellCache {
    std::unordered_map<uint64_t, bool> clear, stand;
};

// What entities ask of the world around them
class EntityWorld : public CollisionSource {
public:
    virtual ~EntityWorld() = default;
    virtual int64_t game_time() const = 0;
    virtual vg::Rng& random() = 0;
    virtual float block_friction(int x, int y, int z) const { return 0.6f; }
    // 0..1 height of the fluid the box stands in at (x, z), the largest over the blocks it touches; 0 when dry
    virtual double fluid_height(const AABB& box, bool lava) const { return 0.0; }
    // the push of the water or lava flowing around the box (Entity.updateFluidHeightAndDoFluidPushing); zero when still or dry
    virtual Vec3 fluid_flow(const AABB& box, bool lava) const { return Vec3(); }
    virtual bool is_full_block(int x, int y, int z) const { return false; }   // BlockState.isCollisionShapeFullBlock
    virtual uint16_t block_state(int x, int y, int z) const { return 0; }
    virtual bool set_block_state(int x, int y, int z, uint16_t state) { return false; }   // tells the players who see it
    virtual int min_build_y() const { return -64; }
    virtual int max_build_y() const { return 320; }
    virtual bool no_collision(const AABB& box) const { thread_local std::vector<AABB> v; v.clear(); block_collisions(box, v); return v.empty(); }
    virtual void entities_in_box(const AABB& box, const Entity* except, std::vector<Entity*>& out) = 0;
    virtual Entity* add_entity(std::unique_ptr<Entity> e) = 0;
    // players, for mob AI and attacks
    struct PlayerRef { uint64_t gid; Vec3 pos; bool targetable; };
    virtual void players_near(const Vec3& p, double range, std::vector<PlayerRef>& out) {}
    virtual void hurt_player(uint64_t gid, float amount, const std::string& source, const Vec3& knockback) {}
    virtual void mob_died(Entity& e, bool by_player) {}
    virtual CellCache* cell_cache(int size_key) { return nullptr; }   // valid until the end of the tick; null: no sharing
    virtual bool can_see_sky(int x, int y, int z) const { return false; }
    virtual void explode(const Vec3& at, float power, Entity* source) {}
    virtual void teleport_owner(uint64_t gid, const Vec3& to) {}   // an ender pearl landed
    virtual int difficulty() const { return 2; }
    virtual int64_t day_time() const { return 0; }
};

class Entity {
public:
    Entity(const std::string& type, EntityWorld* w);
    virtual ~Entity() = default;
    int id = 0;
    uint8_t uuid[16] = {};
    std::string type;                     // "minecraft:item"
    EntityWorld* level;
    Vec3 pos, delta;
    float yaw = 0, pitch = 0;
    double width = 0.6, height = 1.8, eye_height = 1.62;
    float step_height = 0.0f;
    bool on_ground = false, horizontal_collision = false, vertical_collision = false, vertical_collision_below = false;
    bool no_physics = false, no_gravity = false, removed = false, needs_sync = false;
    bool velocity_dirty = false;          // a push (knockback, explosion) the clients must be told about; walking mobs are followed by their position changes alone
    std::vector<int> pending_events;      // entity event statuses (2 hurt, 3 death ...) for the game to send to the players who see the entity
    virtual bool is_living() const { return false; }
    int tick_count = 0;
    float fall_distance = 0;
    std::unique_ptr<vg::LegacyRandom> rng;
    vg::Rng& random() { return *rng; }

    AABB bounding_box() const { return AABB(pos.x - width / 2, pos.y, pos.z - width / 2, pos.x + width / 2, pos.y + height, pos.z + width / 2); }
    void set_pos(double x, double y, double z) { pos = Vec3(x, y, z); }
    void discard() { removed = true; }
    virtual void tick() { tick_count++; }
    virtual double default_gravity() const { return 0.0; }
    double gravity() const { return no_gravity ? 0.0 : default_gravity(); }
    void apply_gravity() { double g = gravity(); if (g != 0.0) delta = delta + Vec3(0, -g, 0); }
    virtual float air_drag() const { return 0.98f; }
    virtual double entity_bounciness() const { return 0.0; }
    void fluid_push();                    // flowing water pushes the entity along
    void move_self(Vec3 d);               // Entity.move(MoverType.SELF, delta)
    void move_towards_closest_space(double x, double y, double z);
    int block_x() const { return int(std::floor(pos.x)); }
    int block_y() const { return int(std::floor(pos.y)); }
    int block_z() const { return int(std::floor(pos.z)); }
    // entity data (SynchedEntityData) as the wire carries it: (index, serializer id, value bytes); filled by subclasses
    struct DataItem { int index; int serializer; std::vector<uint8_t> value; };
    virtual void collect_data(std::vector<DataItem>& out) const;
    virtual uint64_t data_signature() const { return 0; }   // changes whenever collect_data would give something new; lets the game skip building the data of entities that did not change
    virtual int block_state_id() const { return -1; }   // the "data" of the add entity packet for entities that carry a block state (falling block)
};

// PrimedTnt: a lit block of TNT, 80 ticks to the blast
class PrimedTnt : public Entity {
public:
    PrimedTnt(EntityWorld* w, double x, double y, double z);
    int fuse = 80;
    void tick() override;
    double default_gravity() const override { return 0.04; }
    void collect_data(std::vector<DataItem>& out) const override;
    uint64_t data_signature() const override { return uint64_t(fuse); }
};

// FallingBlockEntity: a block that lost its support. Falls with gravity 0.04 and 0.98 drag, lands back as a block or as an item.
class FallingBlockEntity : public Entity {
public:
    FallingBlockEntity(EntityWorld* w, double x, double y, double z, uint16_t state);
    uint16_t state;
    int time = 0;
    void tick() override;
    double default_gravity() const override { return 0.04; }
    int block_state_id() const override { return state; }
private:
    void drop_as_item();
};

class ItemEntity : public Entity {
public:
    ItemEntity(EntityWorld* w, double x, double y, double z, const ItemStack& stack);
    ItemStack item;
    int age = 0, pickup_delay = 0, health = 5;
    uint8_t target[16] = {};
    bool has_target = false;
    float bob_offs = 0;
    void tick() override;
    double default_gravity() const override { return 0.04; }
    void set_default_pickup_delay() { pickup_delay = 10; }
    void set_no_pickup_delay() { pickup_delay = 0; }
    void set_never_pickup() { pickup_delay = 32767; }
    void set_pickup_delay(int t) { pickup_delay = t; }
    bool mergable() const { return !removed && pickup_delay != 32767 && age != -32768 && age < 6000 && item.count() < item.max_stack_size(); }
    void collect_data(std::vector<DataItem>& out) const override;
    uint64_t data_signature() const override { return (uint64_t(item.count()) << 32) ^ uint64_t(item.is_empty() ? 0 : item.item()->index + 1) ^ (uint64_t(item.patch().added.size()) << 56); }
    static bool are_mergable(const ItemStack& a, const ItemStack& b);
private:
    void merge_with_neighbours();
    void try_to_merge(ItemEntity* other);
    static void merge(ItemEntity* to, ItemStack& to_stack, ItemEntity* from, ItemStack& from_stack);
};

}  // namespace pn::sim
