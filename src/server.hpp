// Shared server state, worker threads (one epoll loop each) and the autoscaling pool.
#pragma once
#include "config.hpp"
#include "data.hpp"
#include "util.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pn {

int64_t now_ms();

enum class St { Handshake, Status, Login, Config, Play };

struct JConn {
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

    // play state
    int entity_id = 0;
    int radius = 8;          // chunk radius we serve (min of client and server view distance)
    int cx = 0, cz = 0;
    double x = 0.5, y = -60, z = 0.5;
    float chunk_rate = 9;    // client-requested chunks per tick
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
    void start_play(JConn& c);
    void send(JConn& c, const std::vector<uint8_t>& body);
    void send_raw_frame(JConn& c, const uint8_t* body, size_t n);
    void flush(JConn& c);
    void kick(JConn& c, const std::string& reason);
    void rebuild_pending(JConn& c);
    void pump_chunks(JConn& c);
    void send_chunk(JConn& c, int x, int z);
    void send_chat(JConn& c, const std::string& text);
    void send_server_status(JConn& c);
    void on_move(JConn& c, double nx, double nz);
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
    std::unordered_map<int64_t, std::vector<uint8_t>> chunk_cache_;  // framed (compressed) chunk packets
    size_t chunk_cache_bytes_ = 0;
    std::vector<uint8_t> scratch_;
    int64_t last_tick_ = 0;
};

class Server {
public:
    Config cfg;
    std::vector<VanillaData> data;
    std::string exe_dir;
    uint64_t guid = 0;
    std::atomic<bool> stopping{false};
    std::atomic<int64_t> mem_used{0};
    int64_t mem_cap = 0;
    std::atomic<int> online{0};
    std::atomic<int> next_entity{1};
    std::vector<int> cores;  // resolved CPU list

    bool start(std::string& err);
    void shutdown();
    const VanillaData* find_data(int protocol) const;
    bool mem_add(int64_t n);
    void mem_sub(int64_t n) { mem_used -= n; }
    bool reserve_name(const std::string& name);
    void release_name(const std::string& name);
    bool whitelisted(const std::string& name) const;
    bool banned(const std::string& name) const;
    void broadcast(const std::string& text);
    std::string console_status();
    std::vector<std::string> player_names();
    void register_player(const std::string& n) { std::lock_guard<std::mutex> l(pm_); names_shown_.push_back(n); }
    void unregister_player(const std::string& n);
    bool local_peer(const std::string& addr) const;
    int worker_count();

private:
    friend class Worker;
    bool spawn_worker(std::string& err);
    void monitor();
    void reload_lists();
    std::mutex wm_;
    std::vector<std::unique_ptr<Worker>> workers_;
    int next_worker_id_ = 0;
    std::thread mon_;
    std::mutex pm_;
    std::set<std::string> names_lower_;
    std::vector<std::string> names_shown_;
    std::vector<std::string> whitelist_, banlist_;
public:
    std::string favicon;  // data URI from server-icon.png, empty if absent
};

}  // namespace pn
