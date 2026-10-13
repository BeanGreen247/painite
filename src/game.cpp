#include "game.hpp"
#include "perf.hpp"
#include "anvil.hpp"
#include "log.hpp"
#include "server.hpp"
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

void Game::remove_player(uint64_t gid) {
    save_player_data(gid);
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end()) return;
    auto gone = it->second.id;
    players_.erase(it);
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

void Game::block_changed(int x, int y, int z, uint16_t id) {
    ticker_.schedule_around(x, y, z);
    std::lock_guard<std::mutex> l(mu_);
    int cx = x >> 4, cz = z >> 4;
    for (auto& kv : players_) {
        PState& p = kv.second;
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
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end()) return;
        id = it->second.id;
        it->second.pos = pos;
        it->second.dirty = true;
    }
    GEvent e;
    e.kind = GEvent::Teleport;
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
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end()) return;
        id = it->second.id;
        pos = it->second.pos;
        gm = it->second.gamemode;
        it->second.last_save_ms = now_ms();
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
    root.put("Dimension", Nbt::str("minecraft:overworld"));
    root.put("playerGameType", Nbt::make(Nbt::Int, gm));
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
    s_.world.game_time = game_time_;
    s_.world.day_time = day_time_;
    s_.world.save(unload, now_ms());
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

static int64_t region_key(double x, double z) {
    int rx = static_cast<int>(std::floor(x / 512)), rz = static_cast<int>(std::floor(z / 512));
    return (static_cast<int64_t>(rx) << 32) | static_cast<uint32_t>(rz);
}

void Game::tick() {
    PN_PERF("tick.total");
    game_time_++;
    { PN_PERF("tick.block_updates"); ticker_.run(game_time_); }
    day_time_++;
    if (game_time_ % 20 == 0) broadcast_time();
    std::lock_guard<std::mutex> l(mu_);
    if (players_.empty()) return;
    PN_PERF("tick.players");
    struct Move { PState* p; GEvent ev; bool any = false; };
    std::vector<Move> moves;
    std::unordered_map<int64_t, std::vector<PState*>> grid;   // 512 block regions
    for (auto& kv : players_) {
        PState& p = kv.second;
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
        grid[region_key(p.sent.x, p.sent.z)].push_back(&p);
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
                auto g = grid.find((static_cast<int64_t>(rx + ax) << 32) | static_cast<uint32_t>(rz + az));
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
            if (saving_enabled && s_.world.loaded() > std::max(1500, s_.cfg.memory_cap_mb * 5)) s_.world.save(true, now);
        }
        if (s_.cfg.autosave_s > 0 && now - last_autosave_ >= static_cast<int64_t>(s_.cfg.autosave_s) * 1000) {
            last_autosave_ = now;
            if (saving_enabled) save_everything(true);
        }
        if (now - last_lists > 3000) { last_lists = now; s_.lists.reload_if_changed(); s_.reload_icon(false); }
    }
}

}  // namespace pn
