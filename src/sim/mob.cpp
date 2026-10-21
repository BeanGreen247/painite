#include "mob.hpp"
#include "data.hpp"
#include "../blocks.hpp"
#include "loot.hpp"
#include "path.hpp"
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
    if (fire_ticks > 0) {   // Entity.DATA_SHARED_FLAGS_ID: bit 0 is "on fire"
        DataItem f;
        f.index = 0;
        f.serializer = 0;   // BYTE
        f.value.push_back(1);
        out.push_back(std::move(f));
    }
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
    else on_hurt(from);
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
    if (fire_ticks > 0) {   // Entity.baseTick: burning hurts once a second, water puts it out
        if (level->fluid_height(bounding_box(), false) > 0.0) fire_ticks = 0;
        else { if (fire_ticks % 20 == 0 && !dead) hurt(1.0f, nullptr); --fire_ticks; }
    }
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
    double gdx = goal_x - pos.x, gdz = goal_z - pos.z, gdy = goal_y - pos.y;
    double hd2 = gdx * gdx + gdz * gdz;
    if ((hd2 < 0.36 && std::fabs(gdy) < 1.5) || ++goal_ticks > 400) { stop_moving(); return; }
    double tx = goal_x, ty = goal_y, tz = goal_z;
    if (hd2 > 6.25 || std::fabs(gdy) > 1.5) {   // further than 2.5 blocks (or on another level): plan a way around what is in between
        double moved = (Vec3(goal_x, goal_y, goal_z) - path_goal).length();
        if (path.empty() || path_i >= path.size() || --repath <= 0 || moved > 1.5) {
            path = find_path(*level, pos, Vec3(goal_x, goal_y, goal_z), width, height);
            path_goal = Vec3(goal_x, goal_y, goal_z);
            path_i = 0;
            repath = 20 + (id % 10);
        }
        while (path_i < path.size() && (path[path_i].x - pos.x) * (path[path_i].x - pos.x) + (path[path_i].z - pos.z) * (path[path_i].z - pos.z) < 0.16 && std::fabs(path[path_i].y - pos.y) < 1.2) ++path_i;
        if (path_i < path.size()) { tx = path[path_i].x; ty = path[path_i].y; tz = path[path_i].z; }
    }
    double dx = tx - pos.x, dz = tz - pos.z, dy = ty - pos.y;
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

const EntityWorld::PlayerRef* Mob::pick_target(double range, double& dist2) {
    std::vector<EntityWorld::PlayerRef> near;
    level->players_near(pos, range, near);
    static thread_local std::vector<EntityWorld::PlayerRef> keep;
    keep.clear();
    const EntityWorld::PlayerRef* best = nullptr;
    double best_d = 1e30;
    for (auto& p : near) {
        if (!p.targetable) continue;
        double d = (p.pos.x - pos.x) * (p.pos.x - pos.x) + (p.pos.y - pos.y) * (p.pos.y - pos.y) + (p.pos.z - pos.z) * (p.pos.z - pos.z);
        if (d < best_d) { best_d = d; keep.assign(1, p); best = &keep[0]; }
    }
    dist2 = best_d;
    return best;
}

void Mob::melee() {
    double best_d;
    const EntityWorld::PlayerRef* best = pick_target(attr(EntityTable::get().find(type), "follow_range", 16.0), best_d);
    if (attack_cooldown > 0) --attack_cooldown;
    if (!best) { target_gid = 0; return; }
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

// Creeper: swells while a player is within 3 blocks and goes off after 30 ticks
void Mob::creeper() {
    double d2;
    const EntityWorld::PlayerRef* t = pick_target(16.0, d2);
    if (!t || level->difficulty() == 0) { target_gid = 0; swell_dir = -1; swell = std::max(0, swell - 1); return; }
    target_gid = t->gid;
    if (d2 < 9.0) { swell_dir = 1; ++swell; stop_moving(); yaw = rot_lerp(yaw, float(std::atan2(t->pos.z - pos.z, t->pos.x - pos.x) * 57.2957763671875) - 90.0f, 30.0f); }
    else { swell_dir = -1; swell = std::max(0, swell - 1); move_to(t->pos.x, t->pos.y, t->pos.z, 1.0); }
    if (swell >= 30) {
        level->explode(Vec3(pos.x, pos.y + height * 0.0625, pos.z), 3.0f, this);
        dead = true;
        discard();
    }
}

// Skeleton: keeps its distance and shoots arrows at the nearest player
void Mob::skeleton() {
    double d2;
    const EntityWorld::PlayerRef* t = pick_target(16.0, d2);
    if (shoot_cooldown > 0) --shoot_cooldown;
    if (!t || level->difficulty() == 0) { target_gid = 0; return; }
    target_gid = t->gid;
    double d = std::sqrt(d2), dx = t->pos.x - pos.x, dz = t->pos.z - pos.z;
    if (d > 12.0) move_to(t->pos.x, t->pos.y, t->pos.z, 1.0);
    else if (d < 6.0 && d > 1.0e-3) move_to(pos.x - dx / d * 4.0, pos.y, pos.z - dz / d * 4.0, 1.0);
    else stop_moving();
    float face = float(std::atan2(dz, dx) * 57.2957763671875) - 90.0f;
    if (!has_goal) yaw = rot_lerp(yaw, face, 30.0f);
    if (d <= 15.0 && shoot_cooldown <= 0) {   // RangedBowAttackGoal: every 2 s (1 s on hard), 1.6 speed, less accurate on easier difficulties
        int diff = level->difficulty();
        shoot_cooldown = diff >= 3 ? 20 : 40;
        Vec3 from(pos.x, pos.y + eye_height - 0.1, pos.z);
        double ex = t->pos.x - from.x, ey = (t->pos.y + 1.62 - 1.1) - from.y, ez = t->pos.z - from.z;
        double h = std::sqrt(ex * ex + ez * ez);
        Vec3 dir = Vec3(ex, ey + h * 0.2, ez).normalize();
        double inacc = 0.0075 * (14 - diff * 4);
        Vec3 v(dir.x + random().next_gaussian() * inacc, dir.y + random().next_gaussian() * inacc, dir.z + random().next_gaussian() * inacc);
        auto arrow = std::make_unique<ArrowEntity>(level, type, from, v.scale(1.6));
        arrow->shooter = this;
        level->add_entity(std::move(arrow));
    }
}

void Mob::on_hurt(const Vec3* from) {
    if (ai != MobAi::Wander) return;
    panic_ticks = 60;   // PanicGoal: run away from where the hit came from
    double dx = from ? pos.x - from->x : random().next_double() - 0.5, dz = from ? pos.z - from->z : random().next_double() - 0.5;
    double len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0E-4) { dx = 1; dz = 0; len = 1; }
    move_to(pos.x + dx / len * 7.0, pos.y, pos.z + dz / len * 7.0, 1.25);
}

void Mob::collect_data(std::vector<DataItem>& out) const {
    LivingEntity::collect_data(out);
    if (ai == MobAi::Creeper) {   // Creeper.DATA_SWELL_DIR: an int, 1 swelling, -1 not
        DataItem d;
        d.index = 16;
        d.serializer = 1;
        uint32_t v = uint32_t(swell_dir);
        do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; d.value.push_back(b); } while (v);
        out.push_back(std::move(d));
    }
}

void Mob::ai_step() {
    if (burns_in_daylight && fire_ticks == 0) {   // Mob.isSunBurnTick
        int64_t t = level->day_time() % 24000;
        if ((t < 12500 || t > 23500) && random().next_float() * 30.0f < 0.8f && level->can_see_sky(block_x(), int(std::ceil(pos.y + height)), block_z()) && level->fluid_height(bounding_box(), false) == 0.0)
            set_on_fire(160);
    }
    switch (ai) {
        case MobAi::Wander: wander(); break;
        case MobAi::Melee: melee(); break;
        case MobAi::Creeper: creeper(); break;
        case MobAi::Skeleton: skeleton(); break;
        default: break;
    }
    follow_goal();
    if (!has_goal) jumping = false;
}

std::unique_ptr<LivingEntity> make_living(const std::string& type, EntityWorld* w) {
    const EntityProps* p = EntityTable::get().find(type);
    if (!p || type == "minecraft:player" || type == "minecraft:item") return nullptr;
    if (p->attributes.find("minecraft:max_health") == p->attributes.end()) return nullptr;   // not a living entity
    static const char* melee[] = {"minecraft:zombie", "minecraft:husk", "minecraft:zombie_villager", "minecraft:drowned", "minecraft:zombified_piglin", "minecraft:spider", "minecraft:cave_spider", "minecraft:silverfish", "minecraft:endermite"};
    static const char* still[] = {"minecraft:bat", "minecraft:parrot", "minecraft:bee", "minecraft:allay", "minecraft:vex", "minecraft:ghast", "minecraft:phantom", "minecraft:armor_stand", "minecraft:happy_ghast",
                                  "minecraft:blaze", "minecraft:wither", "minecraft:ender_dragon", "minecraft:breeze", "minecraft:squid", "minecraft:glow_squid"};
    auto m = std::make_unique<Mob>(type, w);
    bool flier = false;
    for (const char* s : still) if (type == s) flier = true;
    for (const char* s : melee) if (type == s && p->attributes.count("minecraft:attack_damage")) m->ai = MobAi::Melee;
    if (type == "minecraft:creeper") m->ai = MobAi::Creeper;
    if (type == "minecraft:skeleton" || type == "minecraft:stray" || type == "minecraft:bogged") {
        m->ai = MobAi::Skeleton;
        if (const ItemType* bow = Items::get().by_name("minecraft:bow")) m->main_hand = ItemStack(bow, 1);
    }
    if (type == "minecraft:zombie" || type == "minecraft:zombie_villager" || type == "minecraft:skeleton" || type == "minecraft:bogged") m->burns_in_daylight = true;
    if (m->ai == MobAi::None && !flier && p->category == "creature") m->ai = MobAi::Wander;
    return m;
}

// ---- arrows

ArrowEntity::ArrowEntity(EntityWorld* w, const std::string& st, const Vec3& at, const Vec3& v) : Entity("minecraft:arrow", w), shooter_type(st) {
    width = 0.5; height = 0.5; eye_height = 0.13;
    set_pos(at.x, at.y - height / 2.0, at.z);
    delta = v;
    yaw = float(std::atan2(v.x, v.z) * 57.2957763671875);
    pitch = float(std::atan2(v.y, v.horizontal_distance()) * 57.2957763671875);
}

void ArrowEntity::tick() {
    Entity::tick();
    if (stuck) { if (++stuck_ticks > 1200) discard(); return; }
    if (tick_count > 1200) { discard(); return; }
    Vec3 before = pos;
    move_self(delta);
    // anything the arrow passed through or ended up in
    Vec3 mid((before.x + pos.x) / 2, (before.y + pos.y) / 2, (before.z + pos.z) / 2);
    for (const Vec3& p : {mid, pos}) {
        AABB probe(p.x - 0.3, p.y - 0.3, p.z - 0.3, p.x + 0.3, p.y + 0.3, p.z + 0.3);
        std::vector<EntityWorld::PlayerRef> players;
        level->players_near(p, 4.0, players);
        float damage = float(std::ceil(delta.length() * 2.0));
        Vec3 from_hit = p - delta;
        for (auto& pl : players) {
            if (!pl.targetable || owner_gid) continue;
            AABB body(pl.pos.x - 0.3, pl.pos.y, pl.pos.z - 0.3, pl.pos.x + 0.3, pl.pos.y + 1.8, pl.pos.z + 0.3);
            if (!body.intersects(probe)) continue;
            Vec3 kb = delta.normalize().scale(0.15);
            level->hurt_player(pl.gid, damage, "arrow:" + shooter_type, Vec3(kb.x, 0.1, kb.z));
            discard();
            return;
        }
        std::vector<Entity*> near;
        level->entities_in_box(probe, this, near);
        for (Entity* e : near) {
            auto* le = dynamic_cast<LivingEntity*>(e);
            if (!le || le->dead || e == shooter) continue;
            le->hurt(damage, &from_hit, owner_gid, owner_gid != 0);
            discard();
            return;
        }
    }
    if (horizontal_collision || vertical_collision) { stuck = true; delta = Vec3(); return; }
    bool water = level->fluid_height(bounding_box(), false) > 0.0;
    double drag = water ? 0.6 : 0.99;
    delta = Vec3(delta.x * drag, delta.y * drag - 0.05, delta.z * drag);
    yaw = float(std::atan2(delta.x, delta.z) * 57.2957763671875);
    pitch = float(std::atan2(delta.y, delta.horizontal_distance()) * 57.2957763671875);
    needs_sync = true;
}

// ---- thrown items

ThrownEntity::ThrownEntity(EntityWorld* w, const std::string& t, const Vec3& at, const Vec3& v, uint64_t owner) : Entity(t, w), owner_gid(owner) {
    width = 0.25; height = 0.25; eye_height = 0.0;
    set_pos(at.x, at.y - height / 2.0, at.z);
    delta = v;
}

void ThrownEntity::hit(LivingEntity* target) {
    if (type == "minecraft:ender_pearl") {
        if (owner_gid) {
            level->teleport_owner(owner_gid, Vec3(pos.x, pos.y, pos.z));
            level->hurt_player(owner_gid, 5.0f, "fall", Vec3());   // ThrownEnderpearl: 5 points of fall damage
        }
    } else {
        if (target) {   // snowballs and eggs do no damage but push and startle
            target->knockback(0.4, target->pos.x - (pos.x - delta.x), target->pos.z - (pos.z - delta.z));
            target->pending_events.push_back(2);
        }
        if (type == "minecraft:egg" && level->random().next_int(8) == 0) {   // ThrownEgg: a chicken hatches (one in 32 hatches four)
            int n = level->random().next_int(32) == 0 ? 4 : 1;
            for (int i = 0; i < n; i++) {
                auto chick = make_living("minecraft:chicken", level);
                if (!chick) continue;
                chick->set_pos(pos.x, pos.y, pos.z);
                level->add_entity(std::move(chick));
            }
        }
    }
    discard();
}

void ThrownEntity::tick() {
    Entity::tick();
    if (tick_count > 400) { discard(); return; }
    Vec3 before = pos;
    move_self(delta);
    Vec3 mid((before.x + pos.x) / 2, (before.y + pos.y) / 2, (before.z + pos.z) / 2);
    for (const Vec3& p : {mid, pos}) {
        AABB probe(p.x - 0.25, p.y - 0.25, p.z - 0.25, p.x + 0.25, p.y + 0.25, p.z + 0.25);
        std::vector<Entity*> near;
        level->entities_in_box(probe, this, near);
        for (Entity* e : near) {
            auto* le = dynamic_cast<LivingEntity*>(e);
            if (le && !le->dead) { hit(le); return; }
        }
    }
    if (horizontal_collision || vertical_collision) { hit(nullptr); return; }
    bool water = level->fluid_height(bounding_box(), false) > 0.0;
    double drag = water ? 0.8 : 0.99;
    delta = Vec3(delta.x * drag, delta.y * drag - 0.03, delta.z * drag);
    needs_sync = true;
}

}  // namespace pn::sim
