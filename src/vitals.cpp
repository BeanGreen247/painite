// Health, hunger, damage and death of players (LivingEntity.hurt, FoodData, Player.die), run on the game thread with players_ locked.
#include "game.hpp"
#include "dimension.hpp"
#include "log.hpp"
#include "server.hpp"
#include "sim/components.hpp"
#include "sim/data.hpp"
#include <cmath>

namespace pn {

static int difficulty_of(Server& s) { return s.cfg.difficulty >= 0 ? s.cfg.difficulty : s.world.difficulty; }

void Game::send_health_locked(PState& p) {
    GEvent e;
    e.kind = GEvent::Health;
    e.health = p.v.health;
    e.value = p.v.food;
    e.saturation = p.v.saturation;
    send_event(p.id, e);
}

void Game::damage(uint64_t gid, float amount, const std::string& source) {
    post_task([this, gid, amount, source] {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it != players_.end()) hurt_locked(it->second, amount, source);
    });
}

void Game::kill_player(uint64_t gid) {
    post_task([this, gid] {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end() || it->second.v.dead) return;
        it->second.v.health = 0;
        die_locked(it->second, "genericKill");
    });
}

float Game::health_of(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    return it == players_.end() ? 0.0f : it->second.v.health;
}

// CombatRules.getDamageAfterAbsorb: the armor and toughness of the worn pieces (their attribute modifiers) soak up part of a hit
static void armor_values(sim::PlayerSim& s, double& armor, double& tough) {
    armor = tough = 0.0;
    for (sim::EquipmentSlot slot : {sim::EquipmentSlot::FEET, sim::EquipmentSlot::LEGS, sim::EquipmentSlot::CHEST, sim::EquipmentSlot::HEAD}) {
        sim::ItemStack& st = s.inv.equipment_at(slot);
        if (st.is_empty()) continue;
        const vg::Json* mods = st.item()->component("minecraft:attribute_modifiers");
        if (!mods || !mods->is_arr()) continue;
        for (const vg::Json& m : mods->arr) {
            if (!m.has("type") || !m.has("amount")) continue;
            if (m.at("type").str == "minecraft:armor") armor += m.at("amount").float_value();
            else if (m.at("type").str == "minecraft:armor_toughness") tough += m.at("amount").float_value();
        }
    }
}

// LivingEntity.hurtServer: a hit inside the 10 ticks after another only counts by how much it exceeds the last one
void Game::hurt_locked(PState& p, float amount, const std::string& source) {
    if (p.v.dead || amount <= 0.0f) return;
    bool bypass = source == "outOfWorld" || source == "genericKill";
    if (!bypass && (p.gamemode == 1 || p.gamemode == 3)) return;
    if (p.gamemode == 3) return;
    if (difficulty_of(s_) == 0 && (source == "starve")) return;
    float real = amount;
    if (!bypass && p.v.invulnerable > 10) {
        if (amount <= p.v.last_hurt) return;
        real = amount - p.v.last_hurt;
        p.v.last_hurt = amount;
    } else if (!bypass) {
        p.v.last_hurt = amount;
        p.v.invulnerable = 20;
    }
    bool soaked = !bypass && source != "fall" && source != "starve" && source != "drown";
    if (soaked && p.sim) {
        double armor, tough;
        armor_values(*p.sim, armor, tough);
        if (armor > 0.0) {
            double f = 2.0 + tough / 4.0, g = std::max(armor * 0.2, std::min(20.0, armor - real / f));
            float before = real;
            real = float(real * (1.0 - g / 25.0));
            int wear = std::max(1, int(before / 4.0f));   // each piece loses durability
            for (sim::EquipmentSlot slot : {sim::EquipmentSlot::FEET, sim::EquipmentSlot::LEGS, sim::EquipmentSlot::CHEST, sim::EquipmentSlot::HEAD}) {
                sim::ItemStack& st = p.sim->inv.equipment_at(slot);
                if (st.is_empty() || !st.damageable()) continue;
                int d = st.damage_value() + wear;
                if (d >= st.item()->max_damage) st.shrink(st.count()); else st.set_damage_value(d);
            }
            p.sim->inventory_menu->broadcast_changes();
        }
    }
    p.v.health -= real;
    add_exhaustion_locked(p, 0.1f);
    GEvent h;
    h.kind = GEvent::Hurt;
    h.id = p.id;
    send_event(p.id, h);
    for (auto& kv : players_)
        if (kv.first != p.id->gid && kv.second.seen.count(p.id->gid)) send_event(kv.second.id, h);
    if (p.v.health <= 0.0f) die_locked(p, source);
    else send_health_locked(p);
}

// Player.die: the inventory drops, the client gets the death screen, everybody else stops seeing the player
void Game::die_locked(PState& p, const std::string& source) {
    p.v.dead = true;
    p.v.health = 0.0f;
    p.v.eating = -1;
    std::string key = source == "outOfWorld" ? "outOfWorld" : source;
    info(p.id->name + " died (" + key + ")");
    if (p.sim && p.pos.dim >= 0 && p.pos.dim <= 2 && entity_managers_[p.pos.dim]) {
        sim::EntityManager& mgr = *entity_managers_[p.pos.dim];
        auto drop = [&](sim::ItemStack& st) {
            if (st.is_empty()) return;
            auto item = std::make_unique<sim::ItemEntity>(&mgr, p.pos.x, p.pos.y + 1.32, p.pos.z, st);
            float speed = mgr.random().next_float() * 0.5f, angle = mgr.random().next_float() * 6.2831855f;   // Player.drop(throwRandomly)
            item->delta = sim::Vec3(-std::sin(angle) * speed, 0.2, std::cos(angle) * speed);
            item->set_pickup_delay(40);
            mgr.add_entity(std::move(item));
            st = sim::ItemStack();
        };
        for (auto& st : p.sim->inv.items) drop(st);
        for (auto& st : p.sim->inv.equipment) drop(st);
        p.sim->inventory_menu->broadcast_full_state();
    }
    GEvent d;
    d.kind = GEvent::Death;
    d.id = p.id;
    if (key.rfind("arrow:", 0) == 0 || key.rfind("explosion:", 0) == 0) {   // shot by / blown up by <mob>
        std::string t = key.substr(key.find(':') + 1);
        d.text = std::string("{\"translate\":\"") + (key[0] == 'a' ? "death.attack.arrow" : "death.attack.explosion.player") + "\",\"with\":[{\"text\":\"" + p.id->name + "\"},{\"translate\":\"entity.minecraft." + t.substr(t.find(':') + 1) + "\"}]}";
    } else if (key.rfind("mob:", 0) == 0) {   // "mob:minecraft:zombie": <player> was slain by <mob>
        std::string t = key.substr(4);
        d.text = "{\"translate\":\"death.attack.mob\",\"with\":[{\"text\":\"" + p.id->name + "\"},{\"translate\":\"entity.minecraft." + t.substr(t.find(':') + 1) + "\"}]}";
    } else
        d.text = "{\"translate\":\"death.attack." + key + "\",\"with\":[{\"text\":\"" + p.id->name + "\"}]}";
    send_event(p.id, d);
}

void Game::respawn(uint64_t gid) {
    post_task([this, gid] {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end() || !it->second.v.dead) return;
        PState& p = it->second;
        World::Spawn sp = s_.world.pick_spawn("", p.gamemode);
        GPos pos = p.pos;
        pos.x = sp.x; pos.y = sp.y; pos.z = sp.z; pos.yaw = 0; pos.pitch = 0; pos.dim = 0; pos.ground = true;
        p.v = Vitals{};
        p.pos = pos;
        p.sent = pos;
        p.dirty = true;
        GEvent e;
        e.kind = GEvent::Dimension;   // the worker answers with a respawn packet, the new chunks and the inventory
        e.id = p.id;
        e.pos = pos;
        e.value = 1;
        send_event(p.id, e);
        send_health_locked(p);
    });
}

// Where a player's gaze points (yaw and pitch in degrees, Minecraft's convention)
static sim::Vec3 look_vector(const GPos& p) {
    double yr = double(p.yaw) * 0.017453292519943295, pr = double(p.pitch) * 0.017453292519943295;
    return sim::Vec3(-std::sin(yr) * std::cos(pr), -std::sin(pr), std::cos(yr) * std::cos(pr));
}

// use_item: eat or drink, start drawing a bow, throw a snowball, an egg or an ender pearl
void Game::start_eating(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim || it->second.v.dead || it->second.v.eating >= 0) return;
    PState& p = it->second;
    if (p.gamemode == 3) return;
    sim::ItemStack& h = p.sim->held_item();
    if (h.is_empty()) return;
    const std::string& name = h.item()->name;
    if (name == "minecraft:bucket" || name == "minecraft:water_bucket" || name == "minecraft:lava_bucket" || name == "minecraft:powder_snow_bucket") { post_task([this, gid] { bucket_use(gid); }); return; }
    if (name == "minecraft:bow") { p.v.bow_start = game_time_; return; }
    if (name == "minecraft:snowball" || name == "minecraft:egg" || name == "minecraft:ender_pearl") {
        if (p.pos.dim < 0 || p.pos.dim > 2 || !entity_managers_[p.pos.dim]) return;
        sim::Vec3 dir = look_vector(p.pos), eye(p.pos.x, p.pos.y + 1.62, p.pos.z);
        auto thrown = std::make_unique<sim::ThrownEntity>(nullptr, name, eye, dir.scale(1.5), gid);
        add_entity(p.pos.dim, std::move(thrown));
        if (p.gamemode != 1) { h.shrink(1); p.sim->inventory_menu->broadcast_changes(); }
        return;
    }
    if (!h.item()->has_food || !(p.v.food < 20 || h.item()->food_can_always_eat)) return;
    p.v.eating = std::max(1, int(std::ceil(h.item()->consume_seconds * 20.0f)));
}

// release use item: a drawn bow shoots (BowItem.releaseUsing)
void Game::stop_eating(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end()) return;
    PState& p = it->second;
    p.v.eating = -1;
    if (p.v.bow_start < 0) return;
    int64_t drawn = game_time_ - p.v.bow_start;
    p.v.bow_start = -1;
    if (!p.sim || p.v.dead || p.pos.dim < 0 || p.pos.dim > 2 || !entity_managers_[p.pos.dim]) return;
    sim::ItemStack& bow = p.sim->held_item();
    if (bow.is_empty() || bow.item()->name != "minecraft:bow") return;
    double f = double(drawn) / 20.0;
    f = (f * f + f * 2.0) / 3.0;
    if (f < 0.1) return;
    if (f > 1.0) f = 1.0;
    if (p.gamemode != 1) {   // an arrow from the inventory is used up
        sim::ItemStack* arrows = nullptr;
        for (auto& st : p.sim->inv.items) if (!st.is_empty() && st.item()->name == "minecraft:arrow") { arrows = &st; break; }
        if (!arrows) return;
        arrows->shrink(1);
        if (bow.damageable()) { int d = bow.damage_value() + 1; if (d >= bow.item()->max_damage) bow.shrink(bow.count()); else bow.set_damage_value(d); }
        p.sim->inventory_menu->broadcast_changes();
    }
    sim::Vec3 dir = look_vector(p.pos), eye(p.pos.x, p.pos.y + 1.62, p.pos.z);
    auto arrow = std::make_unique<sim::ArrowEntity>(nullptr, "minecraft:player", eye, dir.scale(3.0 * f));
    arrow->owner_gid = gid;
    add_entity(p.pos.dim, std::move(arrow));
}

// the effect of the fluid or climbable block a falling player lands in: no fall damage
static bool breaks_fall(const BlockInfo& b) {
    return b.name == "minecraft:water" || b.name == "minecraft:ladder" || b.name == "minecraft:vine" || b.name == "minecraft:cobweb" || b.name == "minecraft:scaffolding" ||
           b.name == "minecraft:powder_snow" || b.name == "minecraft:honey_block" || b.name == "minecraft:slime_block" || b.name == "minecraft:bubble_column" || b.name == "minecraft:twisting_vines" ||
           b.name == "minecraft:weeping_vines" || b.name == "minecraft:cave_vines" || b.name == "minecraft:cave_vines_plant";
}

// Called from update_pos with players_ locked: fall distance and the exhaustion of moving
void Game::track_movement_locked(PState& p, const GPos& now) {
    if (p.v.dead) return;
    const GPos& old = p.pos;
    if (now.dim != old.dim) { p.v.fall = 0; return; }
    World& w = s_.dim(now.dim);
    if (now.y < old.y) {   // Entity.checkFallDamage: the move that lands counts too
        p.v.fall += float(old.y - now.y);
        const Blocks& B = Blocks::get();
        if (B.info(w.get_block(int(std::floor(now.x)), int(std::floor(now.y)), int(std::floor(now.z)))).name == "minecraft:water" ||
            breaks_fall(B.info(w.get_block(int(std::floor(now.x)), int(std::floor(now.y + 0.5)), int(std::floor(now.z))))))
            p.v.fall = 0;
    }
    if (now.ground) {
        if (p.v.fall > 3.0f) p.v.pending_fall += std::ceil(p.v.fall - 3.0f);
        p.v.fall = 0;
    }
    if (old.ground && !now.ground && now.y > old.y) add_exhaustion_locked(p, now.sprinting ? 0.2f : 0.05f);   // jumping
    if (now.sprinting && now.ground) add_exhaustion_locked(p, 0.1f * float(std::hypot(now.x - old.x, now.z - old.z)));
}

void Game::vitals_tick() {
    int diff = difficulty_of(s_);
    for (auto& kv : players_) {
        PState& p = kv.second;
        Vitals& v = p.v;
        if (v.dead) continue;
        bool vulnerable = p.gamemode == 0 || p.gamemode == 2;
        if (v.invulnerable > 0) v.invulnerable--;
        if (v.pending_fall > 0) { float f = v.pending_fall; v.pending_fall = 0; hurt_locked(p, f, "fall"); if (v.dead) continue; }
        const DimInfo& di = dim_info(p.pos.dim);
        if (p.pos.y < di.min_y - 64) { hurt_locked(p, 4.0f, "outOfWorld"); if (v.dead) continue; }
        if (!vulnerable) { if (p.gamemode == 1) { v.air = 300; } continue; }
        World& w = s_.dim(p.pos.dim);
        const Blocks& B = Blocks::get();
        if (game_time_ % 10 == 0) {
            const BlockInfo& feet = B.info(w.get_block(int(std::floor(p.pos.x)), int(std::floor(p.pos.y)), int(std::floor(p.pos.z))));
            if (feet.name == "minecraft:lava") { hurt_locked(p, 4.0f, "lava"); if (v.dead) continue; }
        }
        // drowning: the air supply runs down 1 a tick with the head in water and refills 4 a tick outside
        const BlockInfo& head = B.info(w.get_block(int(std::floor(p.pos.x)), int(std::floor(p.pos.y + 1.62)), int(std::floor(p.pos.z))));
        if (head.name == "minecraft:water" || (head.props.find("waterlogged=true") != std::string::npos && head.solid)) {
            if (--v.air <= -20) { v.air = 0; hurt_locked(p, 2.0f, "drown"); if (v.dead) continue; }
        } else v.air = std::min(300, v.air + 4);
        // FoodData.tick
        if (v.exhaustion > 4.0f) {
            v.exhaustion -= 4.0f;
            if (v.saturation > 0.0f) v.saturation = std::max(v.saturation - 1.0f, 0.0f);
            else if (diff != 0) v.food = std::max(v.food - 1, 0);
        }
        bool hurt = v.health < 20.0f;
        if (diff == 0) {   // peaceful: health and food refill by themselves
            if (hurt && game_time_ % 20 == 0) v.health = std::min(20.0f, v.health + 1.0f);
            v.food = 20; v.saturation = std::max(v.saturation, 5.0f);
        } else if (v.saturation > 0.0f && hurt && v.food >= 20) {
            if (++v.food_timer >= 10) {
                float spent = std::min(v.saturation, 6.0f);
                v.health = std::min(20.0f, v.health + spent / 6.0f);
                add_exhaustion_locked(p, spent);
                v.food_timer = 0;
            }
        } else if (hurt && v.food >= 18) {
            if (++v.food_timer >= 80) { v.health = std::min(20.0f, v.health + 1.0f); add_exhaustion_locked(p, 6.0f); v.food_timer = 0; }
        } else if (v.food <= 0) {
            if (++v.food_timer >= 80) {
                v.food_timer = 0;
                if (v.health > 10.0f || diff == 3 || (diff == 2 && v.health > 1.0f)) { hurt_locked(p, 1.0f, "starve"); if (v.dead) continue; }
            }
        } else v.food_timer = 0;
        // eating: the item is used up when the animation is over
        if (v.eating >= 0 && p.sim) {
            sim::ItemStack& h = p.sim->held_item();
            if (h.is_empty() || !h.item()->has_food) v.eating = -1;
            else if (--v.eating <= 0) {
                v.eating = -1;
                v.food = std::min(20, v.food + h.item()->food_nutrition);
                v.saturation = std::min(v.saturation + h.item()->food_saturation, float(v.food));
                h.shrink(1);
                p.sim->inventory_menu->broadcast_changes();
            }
        }
        if (v.health != v.sent_health || v.food != v.sent_food || v.saturation != v.sent_sat) {
            v.sent_health = v.health; v.sent_food = v.food; v.sent_sat = v.saturation;
            send_health_locked(p);
        }
    }
}

}  // namespace pn
