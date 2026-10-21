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
    if (key.rfind("mob:", 0) == 0) {   // "mob:minecraft:zombie": <player> was slain by <mob>
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

void Game::start_eating(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim || it->second.v.dead || it->second.v.eating >= 0) return;
    PState& p = it->second;
    if (p.gamemode == 3) return;
    const sim::ItemStack& h = p.sim->held_item();
    if (h.is_empty() || !h.item()->has_food || !(p.v.food < 20 || h.item()->food_can_always_eat)) return;
    p.v.eating = std::max(1, int(std::ceil(h.item()->consume_seconds * 20.0f)));
}

void Game::stop_eating(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it != players_.end()) it->second.v.eating = -1;
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
