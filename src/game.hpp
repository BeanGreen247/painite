// Game state shared by all workers: the player registry, who can see whom, world time, the 20 Hz tick loop,
// commands and autosave. Workers (network threads) call in; events go back out through Worker::post_fn.
#pragma once
#include "auth.hpp"
#include "region.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pn {

class Server;

struct GIdentity {
    uint64_t gid = 0;
    bool bedrock = false;
    int worker = 0;
    uint64_t conn = 0;       // JConn::id or Bedrock session id, valid on that worker
    int eid = 0;
    std::string name, xuid;
    uint8_t uuid[16] = {};
    std::vector<AuthProp> props;  // Java skin properties
    std::vector<uint8_t> bedrock_skin;   // Bedrock "Skin" structure for the player list (empty: default)
    std::string addr;
    mutable std::atomic<int> ping_ms{-1};   // round trip to the client, updated by its connection worker
    int64_t joined_ms = 0;
};

struct GPos {
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, head = 0;
    bool ground = true, sneaking = false;
    int dim = 0;                 // 0 overworld, 1 nether, 2 end
    uint8_t skin_parts = 0x7f;   // displayed skin layers (Java client information)
};

struct GEvent {
    enum Kind { AddTab, RemoveTab, Spawn, Despawn, Move, Teleport, Rotate, Swing, Block, Time, Gamemode, Kick, Message, Pose, Dimension } kind = Move;
    std::shared_ptr<const GIdentity> id;   // subject
    GPos pos;
    int16_t dx = 0, dy = 0, dz = 0;
    bool moved = false, turned = false;
    int bx = 0, by = 0, bz = 0;
    uint16_t block = 0;
    int64_t t0 = 0, t1 = 0;                // Time: world age, day time
    int value = 0;
    std::string text;
};

class Worker;

class Game {
public:
    explicit Game(Server& s) : s_(s), ticker_(s) {}
    ~Game();
    void start();
    void stop();

    // --- called from worker threads ---
    uint64_t add_player(std::shared_ptr<GIdentity> id, const GPos& pos, int gamemode, int view_chunks);
    void remove_player(uint64_t gid);
    void update_pos(uint64_t gid, const GPos& pos);
    void set_view(uint64_t gid, int view_chunks);
    void block_changed(int dim, int x, int y, int z, uint16_t id);   // tell everyone who can see that block
    void swing(uint64_t gid);
    void chat(uint64_t gid, const std::string& msg);
    void command(uint64_t gid, const std::string& line);   // gid 0 = console
    void teleport(uint64_t gid, const GPos& pos);          // server-initiated, e.g. /tp (pos.dim picks the dimension)
    void set_gamemode(uint64_t gid, int mode);
    std::shared_ptr<const GIdentity> identity(uint64_t gid);
    std::vector<std::shared_ptr<const GIdentity>> identities();
    std::shared_ptr<const GIdentity> find_name(const std::string& name);
    GPos position(uint64_t gid);
    int gamemode_of(uint64_t gid);
    int op_level(uint64_t gid);
    int players();

    // world clock
    int64_t game_time() const { return game_time_; }
    int64_t day_time() const { return day_time_; }
    void set_day_time(int64_t t) { day_time_ = t; broadcast_time(); }
    double tps() const { return tps_; }
    // duration of the last 256 ticks in microseconds (written by the game thread, read for reports)
    std::atomic<uint32_t> tick_us[256] = {};
    std::atomic<uint32_t> tick_idx{0};
    double mspt() const { return mspt_; }
    bool saving_enabled = true;

    void post_task(std::function<void()> fn);
    RegionTicker& ticker() { return ticker_; }
    void save_everything(bool unload);
    void save_player_data(uint64_t gid);

    // portal.cpp
    bool portal_ignite(int dim, int x, int y, int z);   // light a nether portal whose frame contains this empty block; false when there is no frame
    void portal_tick();                                  // game thread: players standing in portals
    void portal_travel(uint64_t gid, const GPos& from, int kind);
    bool put_ender_eye(int dim, int x, int y, int z);   // an eye of ender into a frame; opens the End portal when the ring is full

private:
    struct PState {
        std::shared_ptr<GIdentity> id;
        GPos pos, sent;
        bool dirty = false, sent_valid = false;
        int gamemode = 0, view = 8;
        std::unordered_set<uint64_t> seen;   // players this one currently has spawned
        int64_t last_save_ms = 0;
    };
    void loop();
    void tick();
    void send_event(const std::shared_ptr<const GIdentity>& to, GEvent ev);
    void queue_event(const std::shared_ptr<const GIdentity>& to, GEvent ev);   // tick only: batched per worker
    void flush_events();
    void broadcast_time();
    void spawn_for(PState& viewer, PState& p);
    void despawn_for(PState& viewer, PState& p);
    void run_command(uint64_t gid, const std::string& line);   // commands.cpp
    void announce(const std::string& text);

    Server& s_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::unordered_map<uint64_t, PState> players_;
    std::unordered_map<int, std::vector<std::pair<uint64_t, GEvent>>> batch_;   // worker id -> events of the running tick
    uint64_t next_gid_ = 1;
    std::mutex task_mu_;
    std::condition_variable task_cv_;
    std::vector<std::function<void()>> tasks_;
    RegionTicker ticker_;
    std::atomic<int64_t> game_time_{0}, day_time_{0};
    double tps_ = 20, mspt_ = 0;
    int64_t last_autosave_ = 0;
    bool find_portal(int dim, int x, int z, int radius, int& ox, int& oy, int& oz);
    void build_portal(int dim, int x, int y, int z);
    void portal_site(int dim, int x, int z, int& oy, int& ox, int& oz);
    struct PortalState { int ticks = 0, cooldown = 0; };
    std::unordered_map<uint64_t, PortalState> portal_state_;
    int exec_dim_ = -1;   // set while running /execute in <dimension> run ...
    friend struct Commands;
public:
    // commands.cpp
    void exec(uint64_t gid, const std::string& line, const std::function<void(const std::string&)>& reply);
};

}  // namespace pn
