#include "mob.hpp"
#include "data.hpp"
#include "../blocks.hpp"
#include "loot.hpp"
#include <cstring>
#include <cmath>

namespace pn::sim {

static double attr(const EntityProps* p, const char* key, double def) {
    if (!p) return def;
    auto it = p->attributes.find(std::string("minecraft:") + key);
    return it == p->attributes.end() ? def : it->second;
}

LivingEntity::LivingEntity(const std::string& t, EntityWorld* w) : Entity(t, w) {
    const EntityProps* p = EntityTable::get().find(t);
    max_health = float(attr(p, "max_health", 20.0));
    health = max_health;
    base_speed = speed = attr(p, "movement_speed", 0.1);
    attack_damage = attr(p, "attack_damage", 0.0);
    attack_knockback = attr(p, "attack_knockback", 0.0);
    knockback_resistance = attr(p, "knockback_resistance", 0.0);
    jump_power = float(attr(p, "jump_strength", 0.42));
    step_height = float(attr(p, "step_height", 0.6));
    yaw = 0;
}

void LivingEntity::collect_data(std::vector<DataItem>& out) const {
    DataItem d;
    d.index = 9;
    d.serializer = 3;   // FLOAT: LivingEntity.DATA_HEALTH_ID
    uint32_t bits;
    float h = health;
    memcpy(&bits, &h, 4);
    for (int i = 3; i >= 0; i--) d.value.push_back(uint8_t(bits >> (8 * i)));
    out.push_back(std::move(d));
}

void LivingEntity::knockback(double strength, double dx, double dz) {
    strength *= 1.0 - knockback_resistance;
    if (strength <= 0.0) return;
    double len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0E-5) { dx = (random().next_double() - 0.5) * 0.02; dz = (random().next_double() - 0.5) * 0.02; len = std::sqrt(dx * dx + dz * dz); }
    double nx = dx / len * strength, nz = dz / len * strength;
    delta = Vec3(delta.x / 2.0 - nx, on_ground ? std::min(0.4, delta.y / 2.0 + strength) : delta.y, delta.z / 2.0 - nz);
    needs_sync = true;
}

bool LivingEntity::hurt(float amount, const Vec3* from, uint64_t attacker_gid, bool by_player) {
    if (dead || health <= 0.0f || amount <= 0.0f) return false;
    float real = amount;
    if (invulnerable_time > 10) {
        if (amount <= last_hurt) return false;
        real = amount - last_hurt;
        last_hurt = amount;
    } else {
        last_hurt = amount;
        invulnerable_time = 20;
    }
    health -= real;
    pending_events.push_back(2);   // hurt
    if (by_player) { killed_by_player = true; last_attacker_gid = attacker_gid; }
    if (from) knockback(0.4, from->x - pos.x, from->z - pos.z);
    if (health <= 0.0f) die();
    return true;
}

void LivingEntity::die() {
    if (dead) return;
    dead = true;
    health = 0.0f;
    pending_events.push_back(3);   // death animation
    level->mob_died(*this, killed_by_player);
}

void LivingEntity::move_relative(double sp, double ix, double iy, double iz) {
    double len = ix * ix + iy * iy + iz * iz;
    if (len < 1.0E-7) return;
    double sx = ix, sy = iy, sz = iz;
    if (len > 1.0) { double n = std::sqrt(len); sx /= n; sy /= n; sz /= n; }
    sx *= sp; sy *= sp; sz *= sp;
    double r = double(yaw) * 0.017453292519943295, s = std::sin(r), c = std::cos(r);
    delta = delta + Vec3(sx * c - sz * s, sy, sz * c + sx * s);
}

// LivingEntity.travelInAir / travelInFluid, simplified for walking mobs
void LivingEntity::travel() {
    AABB box = bounding_box();
    double water = level->fluid_height(box, false), lava = water > 0.0 ? 0.0 : level->fluid_height(box, true);
    if (water > 0.4 || lava > 0.4) {
        double drag = water > 0.4 ? 0.8 : 0.5;
        move_relative(0.02, xxa, 0.0, zza);
        move_self(delta);
        delta = Vec3(delta.x * drag, delta.y * drag - (water > 0.4 ? 0.02 : 0.04) / 4.0, delta.z * drag);
        if (jumping) delta = delta + Vec3(0, 0.04, 0);
        return;
    }
    float friction = on_ground ? level->block_friction(int(std::floor(pos.x)), int(std::floor(pos.y - 0.500001)), int(std::floor(pos.z))) : 1.0f;
    float f = friction * 0.91f;
    double sp = on_ground ? speed * (0.21600002 / (double(friction) * friction * friction)) : double(flying_speed);
    move_relative(sp, xxa, 0.0, zza);
    move_self(delta);
    delta = Vec3(delta.x * f, (delta.y - gravity()) * 0.98, delta.z * f);
}

void LivingEntity::tick() {
    Entity::tick();
    if (invulnerable_time > 0) --invulnerable_time;
    if (dead) {
        xxa = zza = 0.0;
        jumping = false;
        if (++death_time >= 20) { discard(); return; }
    } else {
        xxa *= 0.98; zza *= 0.98;
        if (std::fabs(delta.x) < 0.003) delta = delta.with(0, 0.0);
        if (std::fabs(delta.y) < 0.003) delta = delta.with(1, 0.0);
        if (std::fabs(delta.z) < 0.003) delta = delta.with(2, 0.0);
        if (jumping && on_ground) { delta = delta.with(1, double(jump_power)); on_ground = false; }
        ai_step();
    }
    Vec3 before = pos;
    travel();
    if (dead) return;
    // fall damage: the distance fallen since the last time on the ground (or in water)
    if (on_ground) {
        if (fall_distance > 3.0f) hurt(std::ceil(fall_distance - 3.0f), nullptr);
        fall_distance = 0.0f;
    } else if (pos.y < before.y) fall_distance += float(before.y - pos.y);
    if (level->fluid_height(bounding_box(), false) > 0.0) fall_distance = 0.0f;
    if (pos.y < level->min_build_y() - 64) hurt(4.0f, nullptr);
    if (std::fabs(delta.x) > 0.02 || std::fabs(delta.z) > 0.02 || !on_ground) needs_sync = true;
}

// ---- mobs

Mob::Mob(const std::string& t, EntityWorld* w) : LivingEntity(t, w) {}

void Mob::move_to(double x, double y, double z, double modifier) {
    has_goal = true; goal_x = x; goal_y = y; goal_z = z; goal_speed = modifier; goal_ticks = 0;
}

static float rot_lerp(float cur, float target, float max_step) {
    float d = std::fmod(target - cur, 360.0f);
    if (d >= 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    d = std::max(-max_step, std::min(max_step, d));
    return cur + d;
}

// MoveControl.tick: turn towards the goal, walk, jump when something is in the way
void Mob::follow_goal() {
    if (!has_goal) { zza = 0.0; return; }
    double dx = goal_x - pos.x, dz = goal_z - pos.z, dy = goal_y - pos.y;
    double dist2 = dx * dx + dz * dz + dy * dy;
    if (dist2 < 2.5000003E-7 || ++goal_ticks > 200) { stop_moving(); return; }
    float target_yaw = float(std::atan2(dz, dx) * 57.2957763671875) - 90.0f;
    yaw = rot_lerp(yaw, target_yaw, 90.0f);
    pitch = 0.0f;
    float f = float(goal_speed * base_speed);
    speed_now = f;
    speed = f;   // Mob.setSpeed: the walking speed and the forward input are both modifier * attribute
    zza = f;
    if ((dy > double(step_height) && dx * dx + dz * dz < std::max(1.0, width)) || (horizontal_collision && on_ground)) jumping = true;
    else jumping = false;
}

void Mob::wander() {
    if (panic_ticks > 0) { --panic_ticks; }
    else if (!has_goal && random().next_int(120) == 0) {
        double gx = pos.x + (random().next_double() * 20.0 - 10.0), gz = pos.z + (random().next_double() * 20.0 - 10.0);
        move_to(gx, pos.y, gz, 1.0);
    }
}

void Mob::melee() {
    std::vector<EntityWorld::PlayerRef> near;
    double range = attr(EntityTable::get().find(type), "follow_range", 16.0);
    level->players_near(pos, range, near);
    const EntityWorld::PlayerRef* best = nullptr;
    double best_d = 1e30;
    for (auto& p : near) {
        if (!p.targetable) continue;
        double d = (p.pos.x - pos.x) * (p.pos.x - pos.x) + (p.pos.y - pos.y) * (p.pos.y - pos.y) + (p.pos.z - pos.z) * (p.pos.z - pos.z);
        if (d < best_d) { best_d = d; best = &p; }
    }
    if (attack_cooldown > 0) --attack_cooldown;
    if (!best) { target_gid = 0; if (has_goal && goal_speed == 1.0 && target_gid == 0) {} return; }
    target_gid = best->gid;
    move_to(best->pos.x, best->pos.y, best->pos.z, 1.0);
    double reach = (width * 2.0) * (width * 2.0) + 0.6;
    if (best_d <= reach && attack_cooldown <= 0) {   // MeleeAttackGoal
        attack_cooldown = 20;
        float dmg = float(attack_damage);
        int diff = level->difficulty();
        if (diff == 0) return;
        if (diff == 1) dmg = std::min(dmg / 2.0f + 1.0f, dmg);
        else if (diff == 3) dmg = dmg * 3.0f / 2.0f;
        double kx = best->pos.x - pos.x, kz = best->pos.z - pos.z, len = std::sqrt(kx * kx + kz * kz);
        if (len < 1.0E-4) len = 1.0;
        double strength = 0.4 + attack_knockback;
        level->hurt_player(best->gid, dmg, "mob:" + type, Vec3(kx / len * strength, 0.36, kz / len * strength));
    }
}

void Mob::ai_step() {
    switch (ai) {
        case MobAi::Wander: wander(); break;
        case MobAi::Melee: melee(); break;
        default: break;
    }
    follow_goal();
    if (has_goal && ai == MobAi::Melee) {}
    if (!has_goal) jumping = false;
}

std::unique_ptr<LivingEntity> make_living(const std::string& type, EntityWorld* w) {
    const EntityProps* p = EntityTable::get().find(type);
    if (!p || type == "minecraft:player" || type == "minecraft:item") return nullptr;
    if (p->attributes.find("minecraft:max_health") == p->attributes.end()) return nullptr;   // not a living entity
    static const char* melee[] = {"minecraft:zombie", "minecraft:husk", "minecraft:zombie_villager", "minecraft:drowned", "minecraft:spider", "minecraft:cave_spider", "minecraft:silverfish", "minecraft:endermite"};
    static const char* still[] = {"minecraft:bat", "minecraft:parrot", "minecraft:bee", "minecraft:allay", "minecraft:vex", "minecraft:ghast", "minecraft:phantom", "minecraft:armor_stand", "minecraft:happy_ghast",
                                  "minecraft:blaze", "minecraft:wither", "minecraft:ender_dragon", "minecraft:breeze", "minecraft:squid", "minecraft:glow_squid"};
    auto m = std::make_unique<Mob>(type, w);
    bool flier = false;
    for (const char* s : still) if (type == s) flier = true;
    for (const char* s : melee) if (type == s && p->attributes.count("minecraft:attack_damage")) m->ai = MobAi::Melee;
    if (m->ai == MobAi::None && !flier && p->category == "creature") m->ai = MobAi::Wander;
    return m;
}

}  // namespace pn::sim
