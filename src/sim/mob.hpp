// Living entities and mobs (LivingEntity, Mob and a few goals): health, damage, knockback, walking physics, wandering animals and melee monsters.
#pragma once
#include "entity.hpp"

namespace pn::sim {

class LivingEntity : public Entity {
public:
    LivingEntity(const std::string& type, EntityWorld* w);
    float health = 20.0f, max_health = 20.0f;
    double base_speed = 0.1, speed = 0.1, attack_damage = 0.0, attack_knockback = 0.0, knockback_resistance = 0.0;
    float jump_power = 0.42f;
    float flying_speed = 0.02f;
    int invulnerable_time = 0, death_time = 0;
    float last_hurt = 0.0f;
    bool dead = false;
    bool killed_by_player = false;
    uint64_t last_attacker_gid = 0;
    double xxa = 0.0, zza = 0.0;   // movement input (sideways, forward)
    bool jumping = false;
    float fall_start_y = 0.0f;
    bool is_living() const override { return true; }
    double default_gravity() const override { return 0.08; }
    void tick() override;
    void collect_data(std::vector<DataItem>& out) const override;
    // LivingEntity.hurt: returns false when the hit was ignored. `from` is where the hit came from (knockback pushes away from it).
    bool hurt(float amount, const Vec3* from, uint64_t attacker_gid = 0, bool by_player = false);
    void knockback(double strength, double dx, double dz);
    virtual void die();
    virtual void ai_step() {}
protected:
    void travel();
    void move_relative(double speed_value, double ix, double iy, double iz);
};

enum class MobAi { None, Wander, Melee };

class Mob : public LivingEntity {
public:
    Mob(const std::string& type, EntityWorld* w);
    MobAi ai = MobAi::None;
    void ai_step() override;
    // MoveControl: walk to a point at a fraction of the walking speed
    void move_to(double x, double y, double z, double modifier);
    void stop_moving() { has_goal = false; zza = 0.0; }
    bool has_goal = false;
    double goal_x = 0, goal_y = 0, goal_z = 0, goal_speed = 1.0;
    int goal_ticks = 0;
    int attack_cooldown = 0;
    int panic_ticks = 0;
    uint64_t target_gid = 0;
    float speed_now = 0.0f;
    bool hostile() const { return ai == MobAi::Melee; }
private:
    void wander();
    void melee();
    void follow_goal();
};

// A mob for an entity type, or null when Painite has no behaviour for it
std::unique_ptr<LivingEntity> make_living(const std::string& type, EntityWorld* w);

}  // namespace pn::sim
