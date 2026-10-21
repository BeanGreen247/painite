// Mobs in the game: refreshing what they can see of the players, player attacks, /summon, natural spawning and despawning.
#include "game.hpp"
#include "dimension.hpp"
#include "log.hpp"
#include "server.hpp"
#include "sim/data.hpp"
#include "sim/mob.hpp"
#include <cmath>
#include <unistd.h>

namespace pn {

static const char* const kDimNames[] = {"overworld", "the_nether", "the_end"};

void Game::refresh_entity_world() {
    std::lock_guard<std::mutex> l(mu_);
    for (int dim = 0; dim < 3; ++dim) {
        sim::EntityManager* m = entity_managers_[dim].get();
        if (!m) continue;
        m->players.clear();
        m->difficulty_value = s_.cfg.difficulty >= 0 ? s_.cfg.difficulty : s_.world.difficulty;
        m->day_time_value = day_time_;
        for (auto& kv : players_) {
            const PState& p = kv.second;
            if (p.pos.dim != dim) continue;
            bool targetable = !p.v.dead && (p.gamemode == 0 || p.gamemode == 2);
            m->players.push_back({kv.first, sim::Vec3(p.pos.x, p.pos.y, p.pos.z), targetable});
        }
    }
}

std::unique_ptr<sim::LivingEntity> Game::make_mob(const std::string& type, double x, double y, double z) {
    auto m = sim::make_living(type, nullptr);
    if (m) m->set_pos(x, y, z);
    return m;
}

bool Game::summon(const std::string& type_in, int dim, double x, double y, double z) {
    std::string type = type_in.find(':') == std::string::npos ? "minecraft:" + type_in : type_in;
    if (dim < 0 || dim > 2 || !entity_managers_[dim] || !sim::EntityTable::get().find(type)) return false;
    auto m = make_mob(type, x, y, z);
    if (!m) return false;
    m->yaw = float(entity_managers_[dim]->random().next_double() * 360.0);
    add_entity(dim, std::move(m));
    return true;
}

// Player.attack: the damage of the held item plus the base 1, +50 % for a hit while falling
void Game::attack_entity(uint64_t gid, int entity_id) {
    post_task([this, gid, entity_id] {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end() || it->second.v.dead || !it->second.sim) return;
        PState& p = it->second;
        if (p.gamemode == 3) return;
        sim::EntityManager* mgr = p.pos.dim >= 0 && p.pos.dim <= 2 ? entity_managers_[p.pos.dim].get() : nullptr;
        if (!mgr) return;
        sim::LivingEntity* target = nullptr;
        for (auto& e : mgr->entities()) if (e->id == entity_id) { target = dynamic_cast<sim::LivingEntity*>(e.get()); break; }
        if (!target || target->dead) return;
        double dx = target->pos.x - p.pos.x, dy = target->pos.y + target->height / 2 - (p.pos.y + 1.62), dz = target->pos.z - p.pos.z;
        if (dx * dx + dy * dy + dz * dz > 6.0 * 6.0) return;   // entity_interaction_range 3 plus the hitbox, with some slack for lag
        sim::ItemStack& held = p.sim->held_item();
        double dmg = 1.0;
        if (!held.is_empty())
            if (const vg::Json* mods = held.item()->component("minecraft:attribute_modifiers"); mods && mods->is_arr())
                for (const vg::Json& m : mods->arr)
                    if (m.has("type") && m.at("type").str == "minecraft:attack_damage" && m.has("amount")) dmg += m.at("amount").float_value();
        if (p.v.fall > 0.0f && !p.pos.ground) dmg *= 1.5;   // critical hit
        sim::Vec3 from(p.pos.x, p.pos.y, p.pos.z);
        target->hurt(float(dmg), &from, gid, true);
        add_exhaustion_locked(p, 0.1f);
        if (!held.is_empty() && held.damageable() && gamemode_survival(p)) {   // ItemStack.hurtEnemy
            int d = held.damage_value() + 1;
            if (d >= held.item()->max_damage) held.shrink(held.count()); else held.set_damage_value(d);
            p.sim->inventory_menu->broadcast_changes();
        }
    });
}

static int top_solid(World& w, int x, int z) { return w.top_y(x, z); }

// biome file -> spawns of a category: [{type, weight, count (min, max)}]
struct SpawnEntry { std::string type; int weight, min_count, max_count; };

static const std::vector<SpawnEntry>& biome_spawns(const std::string& data_dir, const std::string& version, const std::string& biome, const std::string& category) {
    static std::map<std::string, std::vector<SpawnEntry>> cache;
    std::string key = version + "/" + biome + "/" + category;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::vector<SpawnEntry>& out = cache[key];
    std::string name = biome.substr(biome.find(':') + 1);
    std::string exe_dir;
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) { buf[n] = 0; exe_dir = buf; exe_dir = exe_dir.substr(0, exe_dir.rfind('/')); }
    }
    for (std::string d : {data_dir, std::string("data"), std::string("../data"), exe_dir + "/data", exe_dir + "/../data"}) {
        if (d.empty()) continue;
        std::string path = d + "/worldgen/" + version + "/data/minecraft/worldgen/biome/" + name + ".json";
        if (access(path.c_str(), R_OK) != 0) continue;
        try {
            vg::Json j = vg::Json::parse_file(path);
            const vg::Json* attrs = j.find("attributes");
            const vg::Json* ns = attrs ? attrs->find("minecraft:gameplay/natural_mob_spawns") : nullptr;
            const vg::Json* arg = ns ? ns->find("argument") : nullptr;
            const vg::Json* sbc = arg ? arg->find("spawns_by_category") : nullptr;
            const vg::Json* list = sbc ? sbc->find(category) : nullptr;
            if (!list) {   // 26.2 and older: "spawners": {category: [{type, weight, minCount, maxCount}]}
                const vg::Json* sp = j.find("spawners");
                const vg::Json* l2 = sp ? sp->find(category) : nullptr;
                if (l2 && l2->is_arr())
                    for (const vg::Json& e : l2->arr) {
                        SpawnEntry s;
                        s.type = e.at("type").str;
                        s.weight = int(e.get_int("weight", 1));
                        s.min_count = int(e.get_int("minCount", 1));
                        s.max_count = int(e.get_int("maxCount", s.min_count));
                        out.push_back(s);
                    }
            }
            if (list && list->is_arr())
                for (const vg::Json& e : list->arr) {
                    SpawnEntry s;
                    s.type = e.at("type").str;
                    s.weight = int(e.get_int("weight", 1));
                    const vg::Json* c = e.find("count");
                    s.min_count = s.max_count = 1;
                    if (c && c->is_num()) s.min_count = s.max_count = int(c->int_value());
                    else if (c && c->is_obj() && c->has("min_inclusive")) { s.min_count = int(c->at("min_inclusive").int_value()); s.max_count = int(c->at("max_inclusive").int_value()); }
                    out.push_back(s);
                }
        } catch (const std::exception&) {}
        break;
    }
    return out;
}

void Game::spawn_tick() {
    if (game_time_ % 40 != 0) return;
    const Blocks& B = Blocks::get();
    for (auto& kv : players_) {
        PState& p = kv.second;
        if (p.v.dead || p.gamemode == 3 || p.pos.dim != 0) continue;   // natural spawning of the Overworld only for now
        sim::EntityManager* mgr = entity_managers_[0].get();
        if (!mgr) continue;
        World& w = s_.dim(0);
        // what lives near this player
        int monsters = 0, creatures = 0;
        for (auto& e : mgr->entities()) {
            auto* m = dynamic_cast<sim::Mob*>(e.get());
            if (!m || m->dead) continue;
            double dx = m->pos.x - p.pos.x, dz = m->pos.z - p.pos.z;
            if (dx * dx + dz * dz > 128.0 * 128.0) continue;
            (m->hostile() ? monsters : creatures)++;
        }
        int64_t t = day_time_ % 24000;
        bool night = t >= 13000 && t < 23000;
        int diff = s_.cfg.difficulty >= 0 ? s_.cfg.difficulty : s_.world.difficulty;
        for (int attempt = 0; attempt < 3; attempt++) {
            bool monster = night && diff > 0 && monsters < 20 && (creatures >= 10 || attempt > 0);
            if (!monster && creatures >= 10) break;
            double ang = mgr->random().next_double() * 6.283185307, dist = 24.0 + mgr->random().next_double() * (monster ? 40.0 : 24.0);
            int x = int(std::floor(p.pos.x + std::cos(ang) * dist)), z = int(std::floor(p.pos.z + std::sin(ang) * dist));
            if (!w.chunk_if_loaded(x >> 4, z >> 4)) continue;
            int top = top_solid(w, x, z);
            if (top < kMinY) continue;
            int y = top + 1;
            const BlockInfo& below = B.info(w.get_block(x, top, z));
            if (!below.solid || below.fluid) continue;
            if (!B.info(w.get_block(x, y, z)).air || !B.info(w.get_block(x, y + 1, z)).air) continue;
            if (!monster && below.name != "minecraft:grass_block") continue;
            // the biome here decides what can spawn
            auto chunk = w.chunk_if_loaded(x >> 4, z >> 4);
            std::string biome = "minecraft:plains";
            {
                std::lock_guard<std::mutex> cl(chunk->mu);
                int s = (y - kMinY) >> 4;
                if (s >= 0 && s < kSections) {
                    const uint8_t* bm = chunk->sec[s] ? chunk->sec[s]->biome : chunk->air_biomes[s];
                    int idx = bm[(((y - kMinY) & 15) >> 2) * 16 + (((z & 15) >> 2) * 4) + ((x & 15) >> 2)];
                    if (idx < int(w.biome_names.size())) biome = w.biome_names[idx];
                }
            }
            const auto& list = biome_spawns(s_.cfg.data_dir, s_.world.gen_version, biome, monster ? "monster" : "creature");
            int total = 0;
            std::vector<const SpawnEntry*> usable;
            for (const SpawnEntry& e : list) if (sim::make_living(e.type, nullptr)) { usable.push_back(&e); total += e.weight; }
            if (total <= 0) continue;
            int roll = mgr->random().next_int(total);
            const SpawnEntry* pick = usable.back();
            for (const SpawnEntry* e : usable) { roll -= e->weight; if (roll < 0) { pick = e; break; } }
            int count = pick->min_count + (pick->max_count > pick->min_count ? mgr->random().next_int(pick->max_count - pick->min_count + 1) : 0);
            for (int i = 0; i < count; i++) {
                double sx = x + 0.5 + (i ? mgr->random().next_double() * 6.0 - 3.0 : 0.0), sz = z + 0.5 + (i ? mgr->random().next_double() * 6.0 - 3.0 : 0.0);
                int sy = top_solid(w, int(std::floor(sx)), int(std::floor(sz))) + 1;
                if (!w.chunk_if_loaded(int(std::floor(sx)) >> 4, int(std::floor(sz)) >> 4) || sy < kMinY) continue;
                auto mob = make_mob(pick->type, sx, sy, sz);
                if (!mob) continue;
                mob->yaw = float(mgr->random().next_double() * 360.0);
                mgr->add_entity(std::move(mob));
                (monster ? monsters : creatures)++;
            }
            break;
        }
    }
    // hostile mobs far from every player go away (Mob.checkDespawn)
    if (game_time_ % 120 == 0) {
        for (int dim = 0; dim < 3; ++dim) {
            sim::EntityManager* mgr = entity_managers_[dim].get();
            if (!mgr) continue;
            for (auto& e : mgr->entities()) {
                auto* m = dynamic_cast<sim::Mob*>(e.get());
                if (!m || !m->hostile() || m->dead) continue;
                double best = 1e30;
                for (auto& pl : mgr->players) best = std::min(best, (pl.pos.x - m->pos.x) * (pl.pos.x - m->pos.x) + (pl.pos.z - m->pos.z) * (pl.pos.z - m->pos.z));
                if (best > 128.0 * 128.0 || (best > 32.0 * 32.0 && mgr->random().next_int(20) == 0)) m->discard();
            }
        }
    }
}

}  // namespace pn
