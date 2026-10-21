// Entities (net.minecraft.world.entity.Entity and the ones Painite has so far), ticked by the game on its own thread.
#pragma once
#include "item.hpp"
#include "phys.hpp"
#include "../vg/random.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pn::sim {

class Entity;

// What entities ask of the world around them
class EntityWorld : public CollisionSource {
public:
    virtual ~EntityWorld() = default;
    virtual int64_t game_time() const = 0;
    virtual vg::Rng& random() = 0;
    virtual float block_friction(int x, int y, int z) const { return 0.6f; }
    // 0..1 height of the fluid the box stands in at (x, z), the largest over the blocks it touches; 0 when dry
    virtual double fluid_height(const AABB& box, bool lava) const { return 0.0; }
    virtual bool is_full_block(int x, int y, int z) const { return false; }   // BlockState.isCollisionShapeFullBlock
    virtual uint16_t block_state(int x, int y, int z) const { return 0; }
    virtual bool set_block_state(int x, int y, int z, uint16_t state) { return false; }   // tells the players who see it
    virtual int min_build_y() const { return -64; }
    virtual int max_build_y() const { return 320; }
    virtual bool no_collision(const AABB& box) const { std::vector<AABB> v; block_collisions(box, v); return v.empty(); }
    virtual void entities_in_box(const AABB& box, const Entity* except, std::vector<Entity*>& out) = 0;
    virtual Entity* add_entity(std::unique_ptr<Entity> e) = 0;
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
    void move_self(Vec3 d);               // Entity.move(MoverType.SELF, delta)
    void move_towards_closest_space(double x, double y, double z);
    int block_x() const { return int(std::floor(pos.x)); }
    int block_y() const { return int(std::floor(pos.y)); }
    int block_z() const { return int(std::floor(pos.z)); }
    // entity data (SynchedEntityData) as the wire carries it: (index, serializer id, value bytes); filled by subclasses
    struct DataItem { int index; int serializer; std::vector<uint8_t> value; };
    virtual void collect_data(std::vector<DataItem>& out) const;
    virtual int block_state_id() const { return -1; }   // the "data" of the add entity packet for entities that carry a block state (falling block)
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
    static bool are_mergable(const ItemStack& a, const ItemStack& b);
private:
    void merge_with_neighbours();
    void try_to_merge(ItemEntity* other);
    static void merge(ItemEntity* to, ItemStack& to_stack, ItemEntity* from, ItemStack& from_stack);
};

}  // namespace pn::sim
