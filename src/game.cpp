#include "game.hpp"
#include "perf.hpp"
#include "anvil.hpp"
#include "dimension.hpp"
#include "log.hpp"
#include "server.hpp"
#include "sim/data.hpp"
#include "sim/entity_manager.hpp"
#include "sim/loot.hpp"
#include "sim/item_nbt.hpp"
#include <algorithm>
#include <chrono>
#include <pthread.h>
#include <cmath>

namespace pn {

Game::~Game() { stop(); }

void Game::start() {
    game_time_ = s_.world.game_time;
    day_time_ = s_.world.day_time;
    last_autosave_ = now_ms();
    for (int dim = 0; dim < 3; ++dim)
    {
        entity_managers_[dim] = std::make_unique<sim::EntityManager>(s_.dim(dim), game_time_, s_.next_entity, dim);
        entity_managers_[dim]->on_hurt_player = [this](uint64_t gid, float amount, const std::string& source, const sim::Vec3& kb) {
            damage(gid, amount, source);
            std::lock_guard<std::mutex> l(mu_);
            auto it = players_.find(gid);
            if (it == players_.end()) return;
            GEvent e;
            e.kind = GEvent::EntityMotion;   // the client moves its own player from this
            e.entity_id = it->second.id->eid;
            e.entity_velocity = kb;
            send_event(it->second.id, e);
        };
        entity_managers_[dim]->on_teleport_owner = [this](uint64_t gid, const sim::Vec3& to) {
            GPos p = position(gid);
            p.x = to.x; p.y = to.y; p.z = to.z;
            teleport(gid, p);
        };
        entity_managers_[dim]->on_block_changed = [this, dim](int x, int y, int z, uint16_t id) { block_changed(dim, x, y, z, id); };
    }
    load_entities();
    ticker_.start(s_.cfg.tick_threads > 0 ? s_.cfg.tick_threads : std::max(1, std::min(4, static_cast<int>(s_.cores.size()) / 2)));
    th_ = std::thread([this] { loop(); });
}

void Game::stop() {
    if (stop_.exchange(true)) return;
    task_cv_.notify_all();
    if (th_.joinable()) th_.join();
    ticker_.stop();
}

void Game::post_task(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> l(task_mu_);
        tasks_.push_back(std::move(fn));
    }
    task_cv_.notify_one();
}

void Game::send_event(const std::shared_ptr<const GIdentity>& to, GEvent ev) {
    s_.post_to_worker(to->worker, [conn = to->conn, ev = std::move(ev)](Worker& w) { w.on_game_event(conn, ev); });
}

void Game::queue_event(const std::shared_ptr<const GIdentity>& to, GEvent ev) { batch_[to->worker].emplace_back(to->conn, std::move(ev)); }

// One post (one lock, one wake-up) per worker per tick instead of one per event.
void Game::flush_events() {
    for (auto& kv : batch_) {
        if (kv.second.empty()) continue;
        s_.post_to_worker(kv.first, [events = std::move(kv.second)](Worker& w) {
            for (auto& e : events) w.on_game_event(e.first, e.second);
        });
        kv.second.clear();
    }
}

// ---- inventory --------------------------------------------------------------------------------------------

namespace {
// Turns what a menu wants the client to know into events for the player's worker
class MenuSync : public sim::ContainerSynchronizer {
public:
    MenuSync(Game* g, std::shared_ptr<const GIdentity> id, std::function<void(const std::shared_ptr<const GIdentity>&, GEvent)> send) : game(g), id_(std::move(id)), send_(std::move(send)) {}
    void send_initial_data(sim::Menu& m, const std::vector<sim::ItemStack>& items, const sim::ItemStack& carried, const std::vector<int>&) override {
        GEvent e;
        e.kind = GEvent::ContainerContent;
        e.container = m.container_id;
        e.state = m.increment_state_id();
        e.items = items;
        e.stack = carried;
        send_(id_, std::move(e));
    }
    void send_slot_change(sim::Menu& m, int slot, const sim::ItemStack& s) override {
        GEvent e;
        e.kind = GEvent::ContainerSlot;
        e.container = m.container_id;
        e.state = m.increment_state_id();
        e.slot = slot;
        e.stack = s;
        send_(id_, std::move(e));
    }
    void send_carried_change(sim::Menu&, const sim::ItemStack& s) override {
        GEvent e;
        e.kind = GEvent::SetCursor;
        e.stack = s;
        send_(id_, std::move(e));
    }
    void send_data_change(sim::Menu&, int, int) override {}
    Game* game;
private:
    std::shared_ptr<const GIdentity> id_;
    std::function<void(const std::shared_ptr<const GIdentity>&, GEvent)> send_;
};
}  // namespace

void Game::inv_click(uint64_t gid, int container, int state, int slot, int button, int mode, const std::map<int, sim::HashedLite>& changed, const sim::HashedLite& carried) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim || mode < 0 || mode > 6) return;
    it->second.sim->handle_container_click(container, state, slot, button, sim::ContainerInput(mode), changed, carried);
}
void Game::inv_close(uint64_t gid, int container) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return;
    it->second.sim->handle_container_close(container);
    it->second.sim->menu->broadcast_changes();
}
void Game::inv_set_held(uint64_t gid, int slot) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it != players_.end() && it->second.sim) it->second.sim->handle_set_carried_item(slot);
}
void Game::inv_creative_slot(uint64_t gid, int slot, const sim::ItemStack& stack) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return;
    it->second.sim->handle_creative_slot(slot, stack);
}
sim::ItemStack Game::inv_held(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return sim::ItemStack();
    return it->second.sim->held_item();
}
bool Game::inv_consume_held(uint64_t gid, int n) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return false;
    auto& sm = *it->second.sim;
    if (sm.has_infinite_materials()) return true;
    sim::ItemStack& h = sm.held_item();
    if (h.is_empty() || h.count() < n) return false;
    h.shrink(n);
    sm.inventory_menu->broadcast_changes();
    return true;
}
// ServerPlayerGameMode.destroyBlock + Block.playerDestroy: the drops only come when the tool is right for the block (Player.hasCorrectToolForDrops)
static bool correct_tool_for_drops(const sim::ItemStack& tool, const std::string& block) {
    const sim::BlockProps* props = sim::BlockTable::get().find(block);
    if (!props || !props->requires_tool) return true;
    if (tool.is_empty()) return false;
    for (const sim::ToolRule& r : tool.item()->tool_rules)
        if (r.has_correct && sim::Tags::get().matches("block", r.blocks, block)) return r.correct_for_drops;
    return false;
}

void Game::player_broke_block(uint64_t gid, int dim, int x, int y, int z, uint16_t state) {
    post_task([this, gid, dim, x, y, z, state] {
        if (dim < 0 || dim > 2 || !entity_managers_[dim] || sim::Items::get().size() == 0) return;
        sim::ItemStack tool;
        {
            std::lock_guard<std::mutex> l(mu_);
            auto it = players_.find(gid);
            if (it == players_.end() || !it->second.sim || it->second.sim->gamemode == 1 || it->second.sim->gamemode == 3) return;
            sim::ItemStack& held = it->second.sim->held_item();
            tool = held;
            add_exhaustion_locked(it->second, 0.005f);
            // ItemStack.mineBlock: a tool with durability loses one point per block that has hardness
            const sim::BlockProps* bp = sim::BlockTable::get().find(Blocks::get().info(state).name);
            if (!held.is_empty() && held.damageable() && bp && bp->destroy_time != 0.0f) {
                int d = held.damage_value() + held.item()->tool_damage_per_block;
                if (d >= held.item()->max_damage) held.shrink(held.count());
                else held.set_damage_value(d);
                it->second.sim->inventory_menu->broadcast_changes();
            }
        }
        const BlockInfo& bi = Blocks::get().info(state);
        if (!correct_tool_for_drops(tool, bi.name)) return;
        const sim::LootTable* table = sim::LootTables::get("minecraft:blocks/" + bi.name.substr(bi.name.find(':') + 1));
        if (!table) return;
        sim::LootBlockState bs;
        bs.name = bi.name;
        for (size_t p = 0; p < bi.props.size();) {   // "a=b,c=d"
            size_t end = bi.props.find(',', p);
            if (end == std::string::npos) end = bi.props.size();
            size_t eq = bi.props.find('=', p);
            if (eq != std::string::npos && eq < end) bs.props[bi.props.substr(p, eq - p)] = bi.props.substr(eq + 1, end - eq - 1);
            p = end + 1;
        }
        sim::LootParams lp;
        lp.tool = &tool;
        lp.block_state = &bs;
        lp.has_origin = true;
        lp.ox = x + 0.5; lp.oy = y + 0.5; lp.oz = z + 0.5;
        sim::EntityManager& mgr = *entity_managers_[dim];
        std::vector<sim::ItemStack> out;
        table->get_random_items(lp, mgr.random().next_long(), out);
        for (sim::ItemStack& st : out) {   // Block.popResource
            if (st.is_empty()) continue;
            double h = 0.25 / 2.0;
            double px = x + 0.5 + mgr.random().next_double() * 0.5 - 0.25, py = y + 0.5 + mgr.random().next_double() * 0.5 - 0.25 - h, pz = z + 0.5 + mgr.random().next_double() * 0.5 - 0.25;
            auto item = std::make_unique<sim::ItemEntity>(&mgr, px, py, pz, st);
            item->set_default_pickup_delay();
            mgr.add_entity(std::move(item));
        }
    });
}

void Game::pickup_items() {
    for (auto& kv : players_) {
        PState& p = kv.second;
        if (!p.sim || p.gamemode == 3 || p.sim->gamemode == 3 || p.pos.dim < 0 || p.pos.dim > 2 || !entity_managers_[p.pos.dim]) continue;
        sim::EntityManager& mgr = *entity_managers_[p.pos.dim];
        sim::AABB body(p.pos.x - 0.3, p.pos.y, p.pos.z - 0.3, p.pos.x + 0.3, p.pos.y + 1.8, p.pos.z + 0.3);
        std::vector<sim::Entity*> near;
        mgr.entities_in_box(body.inflate(1.0, 0.5, 1.0), nullptr, near);
        for (sim::Entity* e : near) {
            auto* item = dynamic_cast<sim::ItemEntity*>(e);
            if (!item || item->removed || item->pickup_delay != 0 || item->item.is_empty()) continue;   // ItemEntity.playerTouch
            if (item->has_target && memcmp(item->target, p.id->uuid, 16) != 0) continue;
            int before = item->item.count();
            sim::ItemStack stack = item->item;
            if (!p.sim->inv.add(stack)) { if (stack.count() == before) continue; }
            int taken = before - stack.count();
            if (taken <= 0) continue;
            item->item = stack;
            if (stack.is_empty()) item->discard();
            auto tr = entity_tracks_.find(item->id);
            auto send_take = [&](const std::shared_ptr<const GIdentity>& to) {
                GEvent ev;
                ev.kind = GEvent::EntityTake;
                ev.entity_id = item->id;
                ev.other_entity_id = p.id->eid;
                ev.value = taken;
                queue_event(to, std::move(ev));
            };
            send_take(p.id);
            if (tr != entity_tracks_.end())
                for (uint64_t gid : tr->second.viewers) {
                    auto v = players_.find(gid);
                    if (v != players_.end() && gid != kv.first) send_take(v->second.id);
                }
            p.sim->inventory_menu->broadcast_changes();
        }
    }
}

void Game::inv_sync(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return;
    it->second.sim->menu->broadcast_full_state();
}

// ---- registry ---------------------------------------------------------------------------------------------

uint64_t Game::add_player(std::shared_ptr<GIdentity> id, const GPos& pos, int gamemode, int view_chunks) {
    std::lock_guard<std::mutex> l(mu_);
    id->gid = next_gid_++;
    PState ps;
    ps.id = id;
    ps.pos = pos;
    ps.sent = pos;
    ps.sent_valid = true;
    ps.gamemode = gamemode;
    ps.view = view_chunks;
    ps.last_save_ms = now_ms();
    {   // health and food from the player file
        Nbt saved;
        if (s_.world.load_player(uuid_string(id->uuid), saved) && saved.type == Nbt::Compound) {
            auto num = [&](const char* k, float def) { const Nbt* n = saved.get(k); return n && (n->type == Nbt::Float || n->type == Nbt::Double) ? float(n->f) : n ? float(n->i) : def; };
            float h = num("Health", 20.0f);
            ps.v.health = h > 0.0f && h <= 20.0f ? h : 20.0f;
            ps.v.food = int(std::max(0.0f, std::min(20.0f, num("foodLevel", 20.0f))));
            ps.v.saturation = std::max(0.0f, num("foodSaturationLevel", 5.0f));
            ps.v.exhaustion = std::max(0.0f, num("foodExhaustionLevel", 0.0f));
            ps.v.sent_health = ps.v.health; ps.v.sent_food = ps.v.food; ps.v.sent_sat = ps.v.saturation;
        }
    }
    if (sim::Items::get().size() > 0) {
    ps.sim = std::make_unique<sim::PlayerSim>();
    ps.sim->gamemode = gamemode;
    {   // the saved inventory
        Nbt saved;
        if (s_.world.load_player(uuid_string(id->uuid), saved) && saved.type == Nbt::Compound) {
            if (const Nbt* inv = saved.get("Inventory"); inv && inv->type == Nbt::List)
                for (auto& e : inv->list) {
                    sim::ItemStack st;
                    int slot = -1;
                    if (sim::item_from_nbt(e, st, &slot) && !st.is_empty() && slot >= 0 && slot < ps.sim->inv.size()) ps.sim->inv.set_item(slot, st);
                }
            ps.sim->inv.selected = int(std::max<int64_t>(0, std::min<int64_t>(8, saved.geti("SelectedItemSlot", 0))));
        }
    }
    ps.sync = std::make_unique<MenuSync>(this, id, [this](const std::shared_ptr<const GIdentity>& to, GEvent ev) { send_event(to, std::move(ev)); });
    ps.sim->inventory_menu->set_synchronizer(ps.sync.get());
    GEvent held;
    held.kind = GEvent::HeldSlot;
    held.slot = ps.sim->inv.selected;
    send_event(id, held);
    send_health_locked(ps);
    }
    for (auto& kv : players_) {
        GEvent a;
        a.kind = GEvent::AddTab;
        a.id = kv.second.id;
        a.pos = kv.second.pos;
        a.value = kv.second.gamemode;
        send_event(id, a);
        GEvent b;
        b.kind = GEvent::AddTab;
        b.id = id;
        b.pos = pos;
        b.value = gamemode;
        send_event(kv.second.id, b);
    }
    players_.emplace(id->gid, std::move(ps));
    return id->gid;
}

void Game::add_entity(int dim, std::unique_ptr<sim::Entity> entity) {
    if (dim < 0 || dim > 2 || !entity) {
        warn("cannot add entity: invalid dimension or null entity");
        return;
    }
    auto pending = std::make_shared<std::unique_ptr<sim::Entity>>(std::move(entity));
    post_task([this, dim, pending] {
        if (!entity_managers_[dim]) {
            warn("cannot add entity: entity manager is not running");
            return;
        }
        entity_managers_[dim]->add_entity(std::move(*pending));
    });
}

void Game::remove_player(uint64_t gid) {
    save_player_data(gid);
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end()) return;
    auto gone = it->second.id;
    players_.erase(it);
    for (auto& tracked : entity_tracks_) tracked.second.viewers.erase(gid);
    for (auto& kv : players_) {
        GEvent d;
        d.id = gone;
        if (kv.second.seen.erase(gid)) { d.kind = GEvent::Despawn; send_event(kv.second.id, d); }
        d.kind = GEvent::RemoveTab;
        send_event(kv.second.id, d);
    }
}

void Game::update_pos(uint64_t gid, const GPos& pos) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end()) return;
    if (pos.dim != it->second.pos.dim) return;   // a late move packet from before a dimension change
    if (it->second.v.dead) return;
    track_movement_locked(it->second, pos);
    it->second.pos = pos;
    it->second.dirty = true;
}

void Game::set_view(uint64_t gid, int view_chunks) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it != players_.end()) it->second.view = view_chunks;
}

void Game::swing(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end()) return;
    for (auto& v : players_)
        if (v.first != gid && v.second.seen.count(gid)) {
            GEvent e;
            e.kind = GEvent::Swing;
            e.id = it->second.id;
            send_event(v.second.id, e);
        }
}

void Game::block_changed(int dim, int x, int y, int z, uint16_t id) {
    ticker_.schedule_around(dim, x, y, z);
    std::lock_guard<std::mutex> l(mu_);
    int cx = x >> 4, cz = z >> 4;
    for (auto& kv : players_) {
        PState& p = kv.second;
        if (p.pos.dim != dim) continue;
        int pcx = static_cast<int>(std::floor(p.pos.x / 16)), pcz = static_cast<int>(std::floor(p.pos.z / 16));
        if (std::abs(pcx - cx) > p.view + 1 || std::abs(pcz - cz) > p.view + 1) continue;
        GEvent e;
        e.kind = GEvent::Block;
        e.bx = x; e.by = y; e.bz = z;
        e.block = id;
        send_event(p.id, e);
    }
}

void Game::chat(uint64_t gid, const std::string& msg) {
    auto id = identity(gid);
    if (!id) return;
    info("<" + id->name + "> " + msg);
    s_.broadcast("<" + id->name + "> " + msg);
}

void Game::command(uint64_t gid, const std::string& line) {
    post_task([this, gid, line] { run_command(gid, line); });
}

void Game::teleport(uint64_t gid, const GPos& pos) {
    std::shared_ptr<GIdentity> id;
    bool changed_dim = false;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end()) return;
        id = it->second.id;
        changed_dim = it->second.pos.dim != pos.dim;
        it->second.pos = pos;
        it->second.dirty = true;
        if (changed_dim) it->second.sent = pos;   // others learn of the move by seeing or not seeing the player, not by a delta
    }
    GEvent e;
    e.kind = changed_dim ? GEvent::Dimension : GEvent::Teleport;
    e.id = id;
    e.pos = pos;
    e.value = 1;   // 1 = the subject itself must be moved (server-initiated)
    send_event(id, e);
}

void Game::set_gamemode(uint64_t gid, int mode) {
    std::shared_ptr<GIdentity> id;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end()) return;
        id = it->second.id;
        it->second.gamemode = mode;
        if (it->second.sim) it->second.sim->gamemode = mode;
        for (auto& kv : players_) {
            GEvent e;
            e.kind = GEvent::Gamemode;
            e.id = id;
            e.value = mode;
            send_event(kv.second.id, e);
        }
    }
}

std::shared_ptr<const GIdentity> Game::identity(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    return it == players_.end() ? nullptr : it->second.id;
}

std::vector<std::shared_ptr<const GIdentity>> Game::identities() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<std::shared_ptr<const GIdentity>> v;
    for (auto& kv : players_) v.push_back(kv.second.id);
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a->gid < b->gid; });
    return v;
}

std::shared_ptr<const GIdentity> Game::find_name(const std::string& name) {
    std::string n = lower(name);
    std::lock_guard<std::mutex> l(mu_);
    for (auto& kv : players_) if (lower(kv.second.id->name) == n) return kv.second.id;
    return nullptr;
}

GPos Game::position(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    return it == players_.end() ? GPos{} : it->second.pos;
}

int Game::gamemode_of(uint64_t gid) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    return it == players_.end() ? 0 : it->second.gamemode;
}

int Game::players() { std::lock_guard<std::mutex> l(mu_); return static_cast<int>(players_.size()); }

int Game::op_level(uint64_t gid) {
    if (gid == 0) return 4;
    auto id = identity(gid);
    if (!id) return 0;
    return s_.lists.op_level(id->name, uuid_string(id->uuid));
}

// ---- player data ------------------------------------------------------------------------------------------

void Game::save_player_data(uint64_t gid) {
    std::shared_ptr<GIdentity> id;
    GPos pos;
    int gm = 0;
    Nbt inventory_list = Nbt::list_of(Nbt::Compound);
    int selected_slot = 0;
    Vitals vit;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end()) return;
        id = it->second.id;
        pos = it->second.pos;
        vit = it->second.v;
        gm = it->second.gamemode;
        it->second.last_save_ms = now_ms();
        if (it->second.sim) {
            auto& inv = it->second.sim->inv;
            for (int i = 0; i < inv.size(); i++)
                if (!inv.item(i).is_empty()) inventory_list.list.push_back(sim::item_to_nbt(inv.item(i), i));
            selected_slot = inv.selected;
        }
    }
    World& w = s_.world;
    std::string u = uuid_string(id->uuid);
    Nbt root;
    if (!w.load_player(u, root) || root.type != Nbt::Compound) {
        root = Nbt::compound();
        root.put("Health", [] { Nbt n; n.type = Nbt::Float; n.f = 20; return n; }());
        root.put("foodLevel", Nbt::make(Nbt::Int, 20));
        root.put("Air", Nbt::make(Nbt::Short, 300));
        Nbt uu; uu.type = Nbt::IntArray;
        for (int i = 0; i < 4; i++) uu.ia.push_back(static_cast<int32_t>(static_cast<uint32_t>(id->uuid[i * 4]) << 24 | static_cast<uint32_t>(id->uuid[i * 4 + 1]) << 16 | static_cast<uint32_t>(id->uuid[i * 4 + 2]) << 8 | id->uuid[i * 4 + 3]));
        root.put("UUID", std::move(uu));
    }
    auto dbl = [](double v) { Nbt n; n.type = Nbt::Double; n.f = v; return n; };
    auto flt = [](double v) { Nbt n; n.type = Nbt::Float; n.f = v; return n; };
    root.put("Health", flt(vit.dead ? 20.0f : vit.health));
    root.put("foodLevel", Nbt::make(Nbt::Int, vit.dead ? 20 : vit.food));
    root.put("foodSaturationLevel", flt(vit.dead ? 5.0f : vit.saturation));
    root.put("foodExhaustionLevel", flt(vit.exhaustion));
    Nbt p = Nbt::list_of(Nbt::Double);
    p.list = {dbl(pos.x), dbl(pos.y), dbl(pos.z)};
    root.put("Pos", std::move(p));
    Nbt r = Nbt::list_of(Nbt::Float);
    r.list = {flt(pos.yaw), flt(pos.pitch)};
    root.put("Rotation", std::move(r));
    Nbt m = Nbt::list_of(Nbt::Double);
    m.list = {dbl(0), dbl(0), dbl(0)};
    root.put("Motion", std::move(m));
    root.put("OnGround", Nbt::make(Nbt::Byte, pos.ground));
    root.put("Dimension", Nbt::str(dim_info(pos.dim).name));
    root.put("playerGameType", Nbt::make(Nbt::Int, gm));
    root.put("Inventory", std::move(inventory_list));
    root.put("SelectedItemSlot", Nbt::make(Nbt::Int, selected_slot));
    root.put("DataVersion", Nbt::make(Nbt::Int, w.data_version));
    if (!w.save_player(u, root)) warn("could not save player data for " + id->name);
}

void Game::save_everything(bool unload) {
    PN_PERF("save.world_and_players");
    std::vector<uint64_t> gids;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& kv : players_) gids.push_back(kv.first);
    }
    for (uint64_t g : gids) save_player_data(g);
    save_entities();
    s_.world.game_time = game_time_;
    s_.world.day_time = day_time_;
    s_.world.save(unload, now_ms());
    for (auto& d : s_.other_dims) d->save(unload, now_ms());
    s_.world.save_level();
}

// ---- tick loop --------------------------------------------------------------------------------------------

void Game::broadcast_time() {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& kv : players_) {
        GEvent e;
        e.kind = GEvent::Time;
        e.t0 = game_time_;
        e.t1 = day_time_;
        send_event(kv.second.id, e);
    }
}

static int64_t region_key(int dim, double x, double z) {
    int rx = static_cast<int>(std::floor(x / 512)), rz = static_cast<int>(std::floor(z / 512));
    return ((static_cast<int64_t>(rx) << 32) | static_cast<uint32_t>(rz)) ^ (static_cast<int64_t>(dim) << 58);
}

void Game::tick() {
    PN_PERF("tick.total");
    game_time_++;
    { PN_PERF("tick.block_updates"); ticker_.run(game_time_); }
    refresh_entity_world();
    { PN_PERF("tick.entities"); for (auto& manager : entity_managers_) if (manager) manager->tick(); }
    day_time_++;
    portal_tick();
    if (game_time_ % 20 == 0) broadcast_time();
    std::lock_guard<std::mutex> l(mu_);
    if (players_.empty()) {
        std::unordered_set<int> live_entities;
        for (const auto& manager : entity_managers_)
            if (manager)
                for (const auto& entity : manager->entities())
                    live_entities.insert(entity->id);
        for (auto it = entity_tracks_.begin(); it != entity_tracks_.end();) {
            if (!live_entities.count(it->first)) it = entity_tracks_.erase(it);
            else { it->second.viewers.clear(); ++it; }
        }
        return;
    }
    PN_PERF("tick.players");
    struct Move { PState* p; GEvent ev; bool any = false; };
    std::vector<Move> moves;
    std::unordered_map<int64_t, std::vector<PState*>> grid;   // 512 block regions
    for (auto& kv : players_) {
        PState& p = kv.second;
        if (p.sim) p.sim->menu->broadcast_changes();
        Move m;
        m.p = &p;
        if (p.dirty) {
            p.dirty = false;
            GEvent e;
            e.id = p.id;
            double dx = p.pos.x - p.sent.x, dy = p.pos.y - p.sent.y, dz = p.pos.z - p.sent.z;
            long qx = std::lround(dx * 4096), qy = std::lround(dy * 4096), qz = std::lround(dz * 4096);
            bool far = std::labs(qx) > 32000 || std::labs(qy) > 32000 || std::labs(qz) > 32000;
            auto ang = [](float a) { return static_cast<int>(std::floor(a * 256.0f / 360.0f)) & 255; };
            bool turned = ang(p.pos.yaw) != ang(p.sent.yaw) || ang(p.pos.pitch) != ang(p.sent.pitch);
            bool moved = qx || qy || qz;
            bool sneak = p.pos.sneaking != p.sent.sneaking;
            if (far) {
                e.kind = GEvent::Teleport;
                p.sent = p.pos;
                e.pos = p.sent;
                m.any = true;
            } else if (moved || turned || p.pos.ground != p.sent.ground) {
                e.kind = GEvent::Move;
                e.moved = moved;
                e.turned = turned;
                e.dx = static_cast<int16_t>(qx); e.dy = static_cast<int16_t>(qy); e.dz = static_cast<int16_t>(qz);
                p.sent.x += static_cast<double>(qx) / 4096.0;
                p.sent.y += static_cast<double>(qy) / 4096.0;
                p.sent.z += static_cast<double>(qz) / 4096.0;
                p.sent.yaw = p.pos.yaw; p.sent.pitch = p.pos.pitch; p.sent.head = p.pos.yaw; p.sent.ground = p.pos.ground;
                e.pos = p.sent;
                m.any = true;
            }
            if (sneak) {
                p.sent.sneaking = p.pos.sneaking;
                GEvent pe;
                pe.kind = GEvent::Pose;
                pe.id = p.id;
                pe.pos = p.sent;
                for (auto& v : players_) if (v.first != p.id->gid && v.second.seen.count(p.id->gid)) queue_event(v.second.id, pe);
            }
            m.ev = e;
        }
        moves.push_back(std::move(m));
        if (!p.v.dead) grid[region_key(p.pos.dim, p.sent.x, p.sent.z)].push_back(&p);   // the dead are not seen
    }
    std::unordered_set<uint64_t> fresh;   // (viewer, subject) pairs spawned this tick already carry the new position
    // who sees whom: only players in the same or a neighbouring 512 block region are candidates
    for (auto& vkv : players_) {
        PState& v = vkv.second;
        double range = std::min(v.view * 16.0, 128.0);
        double out = range + 16.0;
        int rx = static_cast<int>(std::floor(v.sent.x / 512)), rz = static_cast<int>(std::floor(v.sent.z / 512));
        std::unordered_set<uint64_t> near;
        for (int ax = -1; ax <= 1; ax++)
            for (int az = -1; az <= 1; az++) {
                auto g = grid.find(((static_cast<int64_t>(rx + ax) << 32) | static_cast<uint32_t>(rz + az)) ^ (static_cast<int64_t>(v.pos.dim) << 58));
                if (g == grid.end()) continue;
                for (PState* p : g->second) {
                    if (p == &v) continue;
                    double ddx = p->sent.x - v.sent.x, ddz = p->sent.z - v.sent.z, ddy = p->sent.y - v.sent.y;
                    double d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                    bool seen = v.seen.count(p->id->gid) != 0;
                    if (d2 <= range * range || (seen && d2 <= out * out)) near.insert(p->id->gid);
                }
            }
        for (auto it = v.seen.begin(); it != v.seen.end();) {
            if (!near.count(*it)) {
                auto pit = players_.find(*it);
                if (pit != players_.end()) { GEvent d; d.kind = GEvent::Despawn; d.id = pit->second.id; queue_event(v.id, d); }
                it = v.seen.erase(it);
            } else ++it;
        }
        for (uint64_t gid : near) {
            auto pit = players_.find(gid);
            if (pit == players_.end() || v.seen.count(gid)) continue;
            v.seen.insert(gid);
            fresh.insert(vkv.first * 1000003ull + gid);
            GEvent e;
            e.kind = GEvent::Spawn;
            e.id = pit->second.id;
            e.pos = pit->second.sent;
            queue_event(v.id, e);
        }
    }
    // movement to everyone who already had the subject spawned before this tick
    for (auto& m : moves) {
        if (!m.any) continue;
        for (auto& vkv : players_) {
            if (vkv.first == m.p->id->gid) continue;
            PState& v = vkv.second;
            if (!v.seen.count(m.p->id->gid)) continue;
            if (fresh.count(vkv.first * 1000003ull + m.p->id->gid)) continue;
            queue_event(v.id, m.ev);
        }
    }

    std::unordered_set<int> live_entities;
    for (int dim = 0; dim < 3; ++dim) {
        sim::EntityManager* manager = entity_managers_[dim].get();
        if (!manager) continue;
        for (const auto& owned : manager->entities()) {
            sim::Entity& entity = *owned;
            live_entities.insert(entity.id);
            EntityTrack& track = entity_tracks_[entity.id];
            bool first = track.entity != &entity;
            if (first) {
                track = EntityTrack{};
                track.entity = &entity;
                track.pos = entity.pos;
                track.delta = entity.delta;
                track.yaw = entity.yaw;
                track.pitch = entity.pitch;
                track.on_ground = entity.on_ground;
                entity.collect_data(track.data);
            }

            std::unordered_set<uint64_t> near;
            const sim::EntityProps* props = sim::EntityTable::get().find(entity.type);
            double entity_range = props ? props->tracking_range * 16.0 : 80.0;
            for (auto& player : players_) {
                PState& viewer = player.second;
                if (viewer.id->bedrock || viewer.pos.dim != dim) continue;
                double range = std::min(viewer.view * 16.0, entity_range);
                double dx = viewer.pos.x - entity.pos.x;
                double dy = viewer.pos.y - entity.pos.y;
                double dz = viewer.pos.z - entity.pos.z;
                if (dx * dx + dy * dy + dz * dz <= range * range) near.insert(player.first);
            }

            for (auto it = track.viewers.begin(); it != track.viewers.end();) {
                if (near.count(*it)) { ++it; continue; }
                auto viewer = players_.find(*it);
                if (viewer != players_.end()) {
                    GEvent event;
                    event.kind = GEvent::EntityRemove;
                    event.entity_id = entity.id;
                    queue_event(viewer->second.id, std::move(event));
                }
                it = track.viewers.erase(it);
            }
            for (uint64_t gid : near) {
                if (track.viewers.count(gid)) continue;
                auto viewer = players_.find(gid);
                if (viewer == players_.end()) continue;
                track.viewers.insert(gid);
                GEvent event;
                event.kind = GEvent::EntityAdd;
                event.entity_id = entity.id;
                event.entity_kind = entity.type;
                memcpy(event.entity_uuid, entity.uuid, sizeof event.entity_uuid);
                event.pos.x = entity.pos.x; event.pos.y = entity.pos.y; event.pos.z = entity.pos.z;
                event.pos.yaw = entity.yaw; event.pos.pitch = entity.pitch; event.pos.head = entity.yaw;
                event.pos.ground = entity.on_ground;
                event.entity_velocity = entity.delta;
                event.entity_data = track.data;
                event.block = entity.block_state_id() < 0 ? 0 : static_cast<uint16_t>(entity.block_state_id());
                queue_event(viewer->second.id, std::move(event));
                if (auto* living = dynamic_cast<sim::LivingEntity*>(&entity); living && !living->main_hand.is_empty()) {
                    GEvent eq;
                    eq.kind = GEvent::EntityEquip;
                    eq.entity_id = entity.id;
                    eq.stack = living->main_hand;
                    queue_event(viewer->second.id, std::move(eq));
                }
            }

            if (!first) {
                const double dx = entity.pos.x - track.pos.x, dy = entity.pos.y - track.pos.y, dz = entity.pos.z - track.pos.z;
                const long qx = std::lround(dx * 4096.0), qy = std::lround(dy * 4096.0), qz = std::lround(dz * 4096.0);
                auto angle = [](float value) { return static_cast<uint8_t>(static_cast<int>(std::floor(value * 256.0f / 360.0f))); };
                const bool turned = angle(entity.yaw) != angle(track.yaw) || angle(entity.pitch) != angle(track.pitch);
                const bool moved = qx != 0 || qy != 0 || qz != 0;
                const bool too_far = std::labs(qx) > 32767 || std::labs(qy) > 32767 || std::labs(qz) > 32767;
                if (moved || turned || entity.on_ground != track.on_ground) {
                    for (uint64_t gid : track.viewers) {
                        auto viewer = players_.find(gid);
                        if (viewer == players_.end()) continue;
                        GEvent event;
                        event.kind = too_far ? GEvent::EntitySync : GEvent::EntityMove;
                        event.entity_id = entity.id;
                        event.pos.x = too_far ? entity.pos.x : track.pos.x + static_cast<double>(qx) / 4096.0;
                        event.pos.y = too_far ? entity.pos.y : track.pos.y + static_cast<double>(qy) / 4096.0;
                        event.pos.z = too_far ? entity.pos.z : track.pos.z + static_cast<double>(qz) / 4096.0;
                        event.pos.yaw = entity.yaw; event.pos.pitch = entity.pitch; event.pos.head = entity.yaw;
                        event.pos.ground = entity.on_ground;
                        if (!too_far) {
                            event.dx = static_cast<int16_t>(qx); event.dy = static_cast<int16_t>(qy); event.dz = static_cast<int16_t>(qz);
                        }
                        event.moved = moved; event.turned = turned;
                        event.entity_velocity = entity.delta;
                        queue_event(viewer->second.id, std::move(event));
                    }
                    if (too_far) track.pos = entity.pos;
                    else {
                        track.pos.x += static_cast<double>(qx) / 4096.0;
                        track.pos.y += static_cast<double>(qy) / 4096.0;
                        track.pos.z += static_cast<double>(qz) / 4096.0;
                    }
                }
                if (turned) { track.yaw = entity.yaw; track.pitch = entity.pitch; }
                if (entity.on_ground != track.on_ground) track.on_ground = entity.on_ground;

                if (!(entity.delta == track.delta) && (!entity.is_living() || entity.velocity_dirty)) {   // as ServerEntity: only pushes and projectiles/items send velocity
                    for (uint64_t gid : track.viewers) {
                        auto viewer = players_.find(gid);
                        if (viewer == players_.end()) continue;
                        GEvent event;
                        event.kind = GEvent::EntityMotion;
                        event.entity_id = entity.id;
                        event.entity_velocity = entity.delta;
                        queue_event(viewer->second.id, std::move(event));
                    }
                    track.delta = entity.delta;
                    entity.velocity_dirty = false;
                }
                for (int status : entity.pending_events)
                    for (uint64_t gid : track.viewers) {
                        auto viewer = players_.find(gid);
                        if (viewer == players_.end()) continue;
                        GEvent event;
                        event.kind = GEvent::EntityEvent;
                        event.entity_id = entity.id;
                        event.value = status;
                        queue_event(viewer->second.id, std::move(event));
                    }
                entity.pending_events.clear();
                uint64_t sig = entity.data_signature();
                if (sig == track.data_sig) continue;   // nothing to say about this entity's data this tick
                track.data_sig = sig;
                std::vector<sim::Entity::DataItem> data;
                entity.collect_data(data);
                bool changed = data.size() != track.data.size();
                if (!changed) {
                    for (size_t i = 0; i < data.size(); ++i)
                        if (data[i].index != track.data[i].index || data[i].serializer != track.data[i].serializer || data[i].value != track.data[i].value) {
                            changed = true;
                            break;
                        }
                }
                if (changed) {
                    for (uint64_t gid : track.viewers) {
                        auto viewer = players_.find(gid);
                        if (viewer == players_.end()) continue;
                        GEvent event;
                        event.kind = GEvent::EntityData;
                        event.entity_id = entity.id;
                        event.entity_data = data;
                        queue_event(viewer->second.id, std::move(event));
                    }
                    track.data = std::move(data);
                }
            }
        }
    }
    pickup_items();
    vitals_tick();
    spawn_tick();
    for (auto it = entity_tracks_.begin(); it != entity_tracks_.end();) {
        if (live_entities.count(it->first)) { ++it; continue; }
        for (uint64_t gid : it->second.viewers) {
            auto viewer = players_.find(gid);
            if (viewer == players_.end()) continue;
            GEvent event;
            event.kind = GEvent::EntityRemove;
            event.entity_id = it->first;
            queue_event(viewer->second.id, std::move(event));
        }
        it = entity_tracks_.erase(it);
    }
    flush_events();
}

void Game::loop() {
    pthread_setname_np(pthread_self(), "pn-game");
    using clock = std::chrono::steady_clock;
    auto next = clock::now() + std::chrono::milliseconds(50);
    int64_t win_start = now_ms();
    int win_ticks = 0;
    double busy_acc = 0;
    int64_t last_lists = 0, last_unload = 0;
    while (!stop_) {
        std::vector<std::function<void()>> tasks;
        {
            std::unique_lock<std::mutex> l(task_mu_);
            task_cv_.wait_until(l, next, [&] { return stop_.load() || !tasks_.empty(); });
            tasks.swap(tasks_);
        }
        if (stop_) break;
        auto t0 = clock::now();
        for (auto& t : tasks) {
            try { t(); } catch (const std::exception& e) { error(std::string("game task failed: ") + e.what()); }
        }
        if (clock::now() >= next) {
            auto tt0 = clock::now();
            try { tick(); } catch (const std::exception& e) { error(std::string("tick failed: ") + e.what()); }
            tick_us[tick_idx++ & 255] = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - tt0).count());
            win_ticks++;
            next += std::chrono::milliseconds(50);
            if (clock::now() > next + std::chrono::milliseconds(500)) next = clock::now() + std::chrono::milliseconds(50);  // fell behind: skip, do not burst
        }
        busy_acc += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        int64_t now = now_ms();
        if (now - win_start >= 1000) {
            tps_ = std::min(20.0, win_ticks * 1000.0 / static_cast<double>(now - win_start));
            mspt_ = win_ticks ? busy_acc / win_ticks : 0;
            win_start = now;
            win_ticks = 0;
            busy_acc = 0;
        }
        if (now - last_unload > 30000) {   // keep memory bounded when players roam: write and drop chunks nobody touched for a minute
            last_unload = now;
            if (saving_enabled) {
                int lim = std::max(1500, s_.cfg.memory_cap_mb * 5);
                if (s_.world.loaded() > lim) s_.world.save(true, now);
                for (auto& d : s_.other_dims) if (d->loaded() > lim) d->save(true, now);
            }
        }
        if (s_.cfg.autosave_s > 0 && now - last_autosave_ >= static_cast<int64_t>(s_.cfg.autosave_s) * 1000) {
            last_autosave_ = now;
            if (saving_enabled) save_everything(true);
        }
        if (now - last_lists > s_.cfg.files_poll_ms) { last_lists = now; s_.lists.reload_if_changed(); s_.reload_icon(false); }
    }
}

}  // namespace pn
