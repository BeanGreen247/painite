// Shared server state, worker threads (one epoll loop each) and the autoscaling pool.
#pragma once
#include "config.hpp"
#include "auth.hpp"
#include "bedrock.hpp"
#include "data.hpp"
#include "util.hpp"
#include "world.hpp"
#include "lists.hpp"
#include "game.hpp"
#include "xbox.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace pn {

int64_t now_ms();

enum class St { Handshake, Status, Login, Config, Play };

struct JConn {
    ~JConn();
    uint64_t id = 0;
    int fd = -1;
    St st = St::Handshake;
    const VanillaData* vd = nullptr;
    int proto = 0;
    int next_state = 0;
    std::vector<uint8_t> in;
    size_t inpos = 0;
    std::vector<uint8_t> out;
    size_t outpos = 0;
    int threshold = -1;
    bool dead = false;       // close once the output buffer is flushed
    bool want_out = false;   // EPOLLOUT armed
    bool counted = false;    // included in the online player count
    bool reserved = false;   // holds a name reservation
    std::string name, addr;
    uint8_t uuid[16] = {};
    int64_t accepted_ms = 0;
    size_t acct = 0;         // bytes charged to the memory budget
    bool known_packs_sent = false;

    // online mode
    bool awaiting_key = false, auth_pending = false;
    uint8_t token[4] = {};
    std::vector<AuthProp> props;
    std::vector<uint8_t> skin_rgba;
    bool skin_slim = false;
    EVP_CIPHER_CTX* enc = nullptr;
    EVP_CIPHER_CTX* dec = nullptr;
    bool dec_pending = false;
    size_t dec_done = 0;

    // play state
    int entity_id = 0;
    int radius = 8;          // chunk radius we serve (min of client and server view distance)
    int cx = 0, cz = 0;
    double x = 0.5, y = -60, z = 0.5;
    float yaw = 0, pitch = 0;
    int gamemode = 0;
    float chunk_rate = 9;    // client-requested chunks per tick
    uint64_t gid = 0;        // Game registry id
    int tp_id = 0;
    uint16_t hotbar[9] = {};  // internal block ids of the creative hotbar items (0 = none / not a block)
    int held = 0;
    bool sneaking = false, ground = true;
    uint8_t skin_parts = 0x7f;
    int batches_out = 0;
    std::unordered_set<int64_t> sent;
    std::vector<std::pair<int, int>> pending;  // chunks still to send, nearest last (pop_back)
    bool pending_dirty = false;
    int64_t last_ka_sent = 0, last_ka_ack = 0;
    int64_t ka_id = 0;
};

class Server;

class Worker {
public:
    Worker(Server& s, int id, int core, bool with_udp);
    ~Worker();
    bool start(std::string& err);
    void stop();           // ask the thread to exit
    void drain();          // stop accepting; exit once the last connection leaves
    void join();
    void post_chat(const std::string& text);  // thread-safe
    void post_auth(uint64_t conn_id, AuthResult&& r);  // thread-safe
    void post_fn(std::function<void(Worker&)> fn);      // thread-safe, runs on this worker's thread
    void on_game_event(uint64_t conn, const GEvent& ev);
    void send_chat_to(uint64_t conn, const std::string& text);
    void kick_conn(uint64_t conn, const std::string& reason);
    void bedrock_xbox_result(uint64_t id, const XboxResult& res) { if (bedrock_) bedrock_->on_xbox_result(id, res); }
    int id() const { return id_; }
    std::atomic<int> nconn{0};
    std::atomic<uint64_t> busy_ns{0}, wall_ns{0};
    std::atomic<bool> finished{false};
    std::atomic<int64_t> idle_since_ms{0};

private:
    void run();
    bool open_tcp(std::string& err);
    bool open_udp(std::string& err);
    void accept_ready();
    void tcp_readable(JConn& c);
    void tcp_writable(JConn& c);
    void close_conn(JConn& c);
    void tick(int64_t now);
    void drain_inbox();
    void account(JConn& c);

    // java.cpp
    void parse_frames(JConn& c);
    void handle_packet(JConn& c, const uint8_t* p, size_t n);
    void on_handshake(JConn& c, Rd& r, int pid);
    void on_status(JConn& c, Rd& r, int pid);
    void on_login(JConn& c, Rd& r, int pid);
    void on_config(JConn& c, Rd& r, int pid);
    void on_play(JConn& c, Rd& r, int pid);
    void start_config(JConn& c);
    void begin_online_login(JConn& c);
    void on_encryption_response(JConn& c, Rd& r);
    void on_auth_result(uint64_t conn_id, AuthResult& r);
    void finish_login(JConn& c);
    void append_out(JConn& c, const uint8_t* p, size_t n);
    void decrypt_new(JConn& c);
    void start_play(JConn& c);
    void choose_spawn(JConn& c);
    void send(JConn& c, const std::vector<uint8_t>& body);
    void send_raw_frame(JConn& c, const uint8_t* body, size_t n);
    void flush(JConn& c);
    void kick(JConn& c, const std::string& reason);
    void rebuild_pending(JConn& c);
    void pump_chunks(JConn& c);
    void send_chunk(JConn& c, int x, int z);
    void send_chat(JConn& c, const std::string& text);
    void send_server_status(JConn& c);
    void on_move(JConn& c, double nx, double ny, double nz, float yaw, float pitch, bool has_pos, bool has_rot, bool ground);
    void java_event(JConn& c, const GEvent& ev);
    void on_dig(JConn& c, Rd& r);
    void on_place(JConn& c, Rd& r);
    void on_creative_slot(JConn& c, Rd& r);
    void send_block_ack(JConn& c, int seq);
    void send_block_update(JConn& c, int x, int y, int z, uint16_t block);
    void send_time(JConn& c, int64_t age, int64_t day);
    void send_command_tree(JConn& c);
    void teleport_player(JConn& c, const GPos& p);
    void plan_view(JConn& c, int ncx, int ncz);

    // bedrock.cpp
    void udp_readable();

    Server& s_;
    int id_, core_;
    bool with_udp_;
    int ep_ = -1, tcp_ = -1, udp_ = -1, evfd_ = -1;
    std::thread th_;
    std::atomic<bool> stop_{false}, draining_{false};
    std::unordered_map<int, std::unique_ptr<JConn>> conns_;
    std::mutex inbox_mu_;
    std::vector<std::string> inbox_;
    std::vector<std::pair<uint64_t, AuthResult>> auth_inbox_;
    std::vector<std::function<void(Worker&)>> fn_inbox_;
    std::unordered_map<uint64_t, int> id_fd_;
    JConn* conn_by_id(uint64_t id);
    struct CachedChunk { uint32_t ver; std::vector<uint8_t> f; };
    std::unordered_map<int64_t, CachedChunk> chunk_cache_;  // framed (compressed) chunk packets, valid for one chunk version
    size_t chunk_cache_bytes_ = 0;
    std::vector<uint8_t> scratch_;
    std::unique_ptr<Bedrock> bedrock_;
    int64_t last_tick_ = 0;
};

class Server {
public:
    Config cfg;
    std::vector<VanillaData> data;
    std::string exe_dir;
    uint64_t guid = 0;
    std::atomic<bool> stopping{false};
    std::atomic<bool> stop_requested{false};  // set by /stop
    std::atomic<int64_t> mem_used{0};
    int64_t mem_cap = 0;
    std::atomic<int> online{0};
    std::atomic<int> next_entity{1};
    std::atomic<uint64_t> next_conn_id{1};
    Auth auth;
    World world;
    Lists lists;
    Game game{*this};
    std::vector<int> cores;  // resolved CPU list

    bool start(std::string& err);
    void shutdown();
    const VanillaData* find_data(int protocol) const;
    bool mem_add(int64_t n);
    void mem_sub(int64_t n) { mem_used -= n; }
    bool reserve_name(const std::string& name);
    void release_name(const std::string& name);
    bool whitelisted(const std::string& name, const std::string& uuid) const { return lists.whitelisted(name, uuid); }
    void post_to_worker(int worker_id, std::function<void(Worker&)> fn);
    bool kick_player(const std::string& name, const std::string& reason);
    void broadcast(const std::string& text);
    std::string console_status();
    std::vector<std::string> player_names();
    void register_player(const std::string& n) { std::lock_guard<std::mutex> l(pm_); names_shown_.push_back(n); }
    void unregister_player(const std::string& n);
    bool local_peer(const std::string& addr) const;
    int worker_count();
    void route_auth(const AuthRequest& r, AuthResult&& res);

private:
    friend class Worker;
    bool spawn_worker(std::string& err);
    void monitor();
    std::mutex wm_;
    std::vector<std::unique_ptr<Worker>> workers_;
    int next_worker_id_ = 0;
    std::thread mon_;
    std::mutex pm_;
    std::set<std::string> names_lower_;
    std::vector<std::string> names_shown_;
public:
    std::shared_ptr<const std::string> favicon_uri() const { std::lock_guard<std::mutex> l(fav_mu_); return favicon_; }   // data URI of server-icon.png or null
    void reload_icon(bool log_missing);   // re-reads server-icon.png when it changed (checked from the game loop)

private:
    mutable std::mutex fav_mu_;
    std::shared_ptr<const std::string> favicon_;
    int64_t icon_mtime_ = -1;
};

}  // namespace pn
