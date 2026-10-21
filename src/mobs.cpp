// Mobs in the game: refreshing what they can see of the players, player attacks, /summon, natural spawning and despawning.
#include "game.hpp"
#include "cachestats.hpp"
#include "anvil.hpp"
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


// biome file -> spawns of a category: [{type, weight, count (min, max)}]
struct SpawnEntry { std::string type; int weight, min_count, max_count; };

static const std::vector<SpawnEntry>& biome_spawns(const std::string& data_dir, const std::string& version, const std::string& biome, const std::string& category) {
    static std::map<std::string, std::vector<SpawnEntry>> cache;
    std::string key = version + "/" + biome + "/" + category;
    static CacheStat& st = cache_stat("biome spawn lists");
    auto it = cache.find(key);
    if (it != cache.end()) { st.hit(); return it->second; }
    st.miss();
    std::vector<SpawnEntry>& out = cache[key];
    st.add(1, 64);
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

// A floor to spawn on near (x, z): `from_y` downwards (or the surface when from_y is kMinY - 1): solid and dry below, two blocks of air above
static bool find_floor(World& w, int x, int z, int from_y, int& out_y, bool& sky) {
    const Blocks& B = Blocks::get();
    if (!w.chunk_if_loaded(x >> 4, z >> 4)) return false;
    int top = w.top_y(x, z);
    int start = from_y < kMinY ? top + 1 : std::min(from_y, kMinY + kHeight - 3);
    for (int y = start; y > std::max(kMinY + 1, start - 14); --y) {
        const BlockInfo& below = B.info(w.get_block(x, y - 1, z));
        if (!below.solid || below.fluid || below.air) continue;
        if (!B.info(w.get_block(x, y, z)).air || !B.info(w.get_block(x, y + 1, z)).air) continue;
        out_y = y;
        sky = top < y;
        return true;
    }
    return false;
}

void Game::spawn_tick() {
    if (game_time_ % 40 != 0) return;
    const Blocks& B = Blocks::get();
    for (auto& kv : players_) {
        PState& p = kv.second;
        if (p.v.dead || p.gamemode == 3 || p.pos.dim < 0 || p.pos.dim > 1) continue;   // the Overworld and the Nether; the End has only endermen, which have no behaviour yet
        int dim = p.pos.dim;
        sim::EntityManager* mgr = entity_managers_[dim].get();
        if (!mgr) continue;
        World& w = s_.dim(dim);
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
            // animals only on the Overworld surface; monsters at night on the surface, in the dark underground (no sky above; without a light engine torches do not count) and in the Nether
            bool monster = diff > 0 && monsters < 20 && (dim == 1 || creatures >= 10 || attempt > 0);
            if (!monster && (creatures >= 10 || dim != 0)) break;
            double ang = mgr->random().next_double() * 6.283185307, dist = 24.0 + mgr->random().next_double() * (monster ? 40.0 : 24.0);
            int x = int(std::floor(p.pos.x + std::cos(ang) * dist)), z = int(std::floor(p.pos.z + std::sin(ang) * dist));
            bool underground_try = monster && (dim == 1 || attempt % 2 == 1);
            int y = 0;
            bool sky = false;
            if (!find_floor(w, x, z, underground_try ? int(p.pos.y) + mgr->random().next_int(25) - 12 : kMinY - 1, y, sky)) continue;
            if (monster && dim == 0 && sky && !night) continue;      // daylight on the surface
            const BlockInfo& below = B.info(w.get_block(x, y - 1, z));
            if (!monster && below.name != "minecraft:grass_block") continue;
            // the biome here decides what can spawn
            auto chunk = w.chunk_if_loaded(x >> 4, z >> 4);
            std::string biome = "minecraft:plains";
            {
                std::lock_guard<std::mutex> cl(chunk->mu);
                int sec = (y - kMinY) >> 4;
                if (sec >= 0 && sec < kSections) {
                    const uint8_t* bm = chunk->sec[sec] ? chunk->sec[sec]->biome : chunk->air_biomes[sec];
                    int idx = bm[(((y - kMinY) & 15) >> 2) * 16 + (((z & 15) >> 2) * 4) + ((x & 15) >> 2)];
                    if (idx < int(w.biome_names.size())) biome = w.biome_names[idx];
                }
            }
            const auto& list = biome_spawns(s_.cfg.data_dir, s_.world.gen_version, biome, monster ? "monster" : "creature");
            int total = 0;
            std::vector<const SpawnEntry*> usable;
            for (const SpawnEntry& e : list) {
                auto probe = sim::make_living(e.type, nullptr);
                auto* pm = dynamic_cast<sim::Mob*>(probe.get());
                if (pm && (!monster || pm->ai != sim::MobAi::None)) { usable.push_back(&e); total += e.weight; }   // monsters that would only stand there are left out
            }
            if (total <= 0) continue;
            int roll = mgr->random().next_int(total);
            const SpawnEntry* pick = usable.back();
            for (const SpawnEntry* e : usable) { roll -= e->weight; if (roll < 0) { pick = e; break; } }
            int count = pick->min_count + (pick->max_count > pick->min_count ? mgr->random().next_int(pick->max_count - pick->min_count + 1) : 0);
            for (int i = 0; i < count; i++) {
                double sx = x + 0.5 + (i ? mgr->random().next_double() * 6.0 - 3.0 : 0.0), sz = z + 0.5 + (i ? mgr->random().next_double() * 6.0 - 3.0 : 0.0);
                int sy = y;
                bool s2 = false;
                if (i && !find_floor(w, int(std::floor(sx)), int(std::floor(sz)), y + 2, sy, s2)) continue;
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

// Mobs are kept in painite_entities_<dim>.nbt next to the world: type, position, rotation, health and fire. Items, arrows and falling blocks are not.
static std::string entity_file(Server& s, int dim) { return s.dim(dim).dir + "/painite_entities_" + std::to_string(dim) + ".nbt"; }

void Game::save_entities() {
    for (int dim = 0; dim < 3; ++dim) {
        if (!entity_managers_[dim]) continue;
        Nbt list = Nbt::list_of(Nbt::Compound);
        for (auto& e : entity_managers_[dim]->entities()) {
            auto* m = dynamic_cast<sim::Mob*>(e.get());
            if (!m || m->dead || m->removed) continue;
            Nbt c = Nbt::compound();
            c.put("id", Nbt::str(m->type));
            Nbt pos = Nbt::list_of(Nbt::Double);
            for (double v : {m->pos.x, m->pos.y, m->pos.z}) { Nbt d; d.type = Nbt::Double; d.f = v; pos.list.push_back(d); }
            c.put("Pos", std::move(pos));
            Nbt rot = Nbt::list_of(Nbt::Float);
            for (float v : {m->yaw, m->pitch}) { Nbt d; d.type = Nbt::Float; d.f = v; rot.list.push_back(d); }
            c.put("Rotation", std::move(rot));
            Nbt h; h.type = Nbt::Float; h.f = m->health;
            c.put("Health", std::move(h));
            c.put("Fire", Nbt::make(Nbt::Short, m->fire_ticks));
            list.list.push_back(std::move(c));
        }
        std::string path = entity_file(s_, dim);
        if (list.list.empty()) { unlink(path.c_str()); continue; }
        Nbt root = Nbt::compound();
        root.put("Mobs", std::move(list));
        if (!write_nbt_file(path, root)) warn("could not save the mobs of dimension " + std::to_string(dim));
    }
}

void Game::load_entities() {
    int loaded = 0;
    for (int dim = 0; dim < 3; ++dim) {
        if (!entity_managers_[dim]) continue;
        Nbt root;
        if (!read_nbt_file(entity_file(s_, dim), root) || root.type != Nbt::Compound) continue;
        const Nbt* list = root.get("Mobs");
        if (!list || list->type != Nbt::List) continue;
        for (const Nbt& c : list->list) {
            const Nbt* pos = c.get("Pos");
            if (!pos || pos->list.size() != 3) continue;
            auto mob = make_mob(c.gets("id"), pos->list[0].f, pos->list[1].f, pos->list[2].f);
            if (!mob) continue;
            if (const Nbt* rot = c.get("Rotation"); rot && rot->list.size() == 2) { mob->yaw = float(rot->list[0].f); mob->pitch = float(rot->list[1].f); }
            if (const Nbt* h = c.get("Health"); h && h->f > 0) mob->health = std::min(float(h->f), mob->max_health);
            mob->fire_ticks = int(c.geti("Fire", 0));
            entity_managers_[dim]->add_entity(std::move(mob));
            loaded++;
        }
    }
    if (loaded) info("restored " + std::to_string(loaded) + " mob(s)");
}

}  // namespace pn
