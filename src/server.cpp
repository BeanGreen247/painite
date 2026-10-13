#include "server.hpp"
#include "log.hpp"
#include "util.hpp"
#include "bchunk.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <sys/stat.h>

namespace pn {

static std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == in.size()) { uint32_t v = uint8_t(in[i]) << 16; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "=="; }
    else if (i + 2 == in.size()) { uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8); o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '='; }
    return o;
}

void Server::post_to_worker(int worker_id, std::function<void(Worker&)> fn) {
    std::lock_guard<std::mutex> l(wm_);
    for (auto& w : workers_) if (w->id() == worker_id) { w->post_fn(std::move(fn)); return; }
}

bool Server::kick_player(const std::string& name, const std::string& reason) {
    auto id = game.find_name(name);
    if (!id) return false;
    post_to_worker(id->worker, [conn = id->conn, reason](Worker& w) { w.kick_conn(conn, reason); });
    return true;
}

const VanillaData* Server::find_data(int protocol) const {
    for (auto& d : data) if (d.protocol == protocol) return &d;
    return nullptr;
}

bool Server::mem_add(int64_t n) {
    int64_t cur = mem_used.load();
    while (true) {
        if (cur + n > mem_cap) return false;
        if (mem_used.compare_exchange_weak(cur, cur + n)) return true;
    }
}

bool Server::reserve_name(const std::string& name) {
    std::lock_guard<std::mutex> l(pm_);
    return names_lower_.insert(lower(name)).second;
}
void Server::release_name(const std::string& name) {
    std::lock_guard<std::mutex> l(pm_);
    names_lower_.erase(lower(name));
}
void Server::unregister_player(const std::string& n) {
    std::lock_guard<std::mutex> l(pm_);
    names_shown_.erase(std::remove(names_shown_.begin(), names_shown_.end(), n), names_shown_.end());
}
std::vector<std::string> Server::player_names() {
    std::lock_guard<std::mutex> l(pm_);
    return names_shown_;
}

bool Server::local_peer(const std::string& addr) const {
    size_t c = addr.rfind(':');
    std::string ip = c == std::string::npos ? addr : addr.substr(0, c);
    if (ip == "::1" || ip.rfind("127.", 0) == 0 || ip.rfind("10.", 0) == 0 || ip.rfind("192.168.", 0) == 0 || ip.rfind("fe80:", 0) == 0 ||
        ip.rfind("fd", 0) == 0)
        return true;
    if (ip.rfind("172.", 0) == 0) {
        int b = atoi(ip.c_str() + 4);
        return b >= 16 && b <= 31;
    }
    return false;
}

void Server::broadcast(const std::string& text) {
    std::lock_guard<std::mutex> l(wm_);
    for (auto& w : workers_) if (!w->finished) w->post_chat(text);
}

int Server::worker_count() {
    std::lock_guard<std::mutex> l(wm_);
    int n = 0;
    for (auto& w : workers_) if (!w->finished) n++;
    return n;
}

std::string Server::console_status() {
    std::lock_guard<std::mutex> l(wm_);
    std::ostringstream o;
    o << "players " << online.load() << "/" << cfg.max_players << ", workers";
    for (auto& w : workers_) o << " [#" << w->id() << " conns=" << w->nconn.load() << (w->finished ? " done" : "") << "]";
    o << ", memory " << (mem_used.load() >> 20) << "/" << (mem_cap >> 20) << " MiB, cores";
    for (int c : cores) o << " " << c;
    return o.str();
}

bool Server::spawn_worker(std::string& err) {
    int id = next_worker_id_++;
    int core = cores.empty() ? -1 : cores[id % cores.size()];
    auto w = std::make_unique<Worker>(*this, id, core, id == 0);
    if (!w->start(err)) return false;
    workers_.push_back(std::move(w));
    return true;
}

void Server::route_auth(const AuthRequest& r, AuthResult&& res) {
    std::lock_guard<std::mutex> l(wm_);
    for (auto& w : workers_) if (w->id() == r.worker_id) { w->post_auth(r.conn_id, std::move(res)); return; }
}

bool Server::start(std::string& err) {
    mem_cap = static_cast<int64_t>(cfg.memory_cap_mb) * 1024 * 1024;
    std::random_device rd;
    guid = (static_cast<uint64_t>(rd()) << 32) | rd();

    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    sched_getaffinity(0, sizeof allowed, &allowed);
    if (cfg.cores.empty()) {
        for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &allowed)) cores.push_back(i);
    } else {
        for (int c : cfg.cores) {
            if (!CPU_ISSET(c, &allowed)) { err = "core " + std::to_string(c) + " is not available to this process"; return false; }
            if (std::find(cores.begin(), cores.end(), c) == cores.end()) cores.push_back(c);
        }
        cpu_set_t want;
        CPU_ZERO(&want);
        for (int c : cores) CPU_SET(c, &want);
        sched_setaffinity(0, sizeof want, &want);
    }
    if (cores.empty()) { err = "no usable CPU cores"; return false; }
    if (cfg.workers_max <= 0 || cfg.workers_max > static_cast<int>(cores.size())) cfg.workers_max = static_cast<int>(cores.size());
    if (cfg.workers_min < 1) cfg.workers_min = 1;
    if (cfg.workers_min > cfg.workers_max) cfg.workers_min = cfg.workers_max;

    auto bins = list_vanilla_bins(cfg.data_dir, exe_dir);
    if (bins.empty()) { err = "no protocol data found (data/vanilla-*.bin). Run: python3 tools/fetch_vanilla_data.py"; return false; }
    for (auto& b : bins) {
        VanillaData v;
        std::string e;
        if (!load_vanilla_file(b, v, e)) { warn("skipping " + b + ": " + e); continue; }
        data.push_back(std::move(v));
    }
    if (data.empty()) { err = "none of the protocol data files could be loaded"; return false; }
    std::sort(data.begin(), data.end(), [](auto& a, auto& b) { return a.protocol < b.protocol; });
    {
        std::vector<std::pair<int, std::string>> files;
        for (auto it = data.rbegin(); it != data.rend(); ++it) files.emplace_back(it->protocol, it->blocks_path);  // newest first
        if (!Blocks::get().load(files, err)) { err = "block data: " + err + " (run tools/fetch_vanilla_data.py again)"; return false; }
        std::string gen = cfg.generator;
        if (gen == "auto") {
            if (cfg.level_type == "flat" || cfg.level_type == "superflat") gen = "flat";
            else if (cfg.level_type == "void") gen = "void";
            else if (cfg.level_type != "normal" && !cfg.level_type.empty()) warn("level-type=" + cfg.level_type + " needs terrain generation, which Painite does not have; using the flat generator for new chunks");
        }
        if (!world.open(cfg.level_name, gen, data.back(), err)) return false;
        if (cfg.bedrock) {
            std::string bb = find_data_file(cfg.data_dir, exe_dir, "blocks-bedrock.bin"), berr;
            if (bb.empty() || !Blocks::get().load_bedrock(bb, berr)) {
                warn("Bedrock world data missing" + (berr.empty() ? std::string(" (data/blocks-bedrock.bin)") : ": " + berr) + ": run tools/fetch_bedrock_blocks.py; Bedrock players will see wrong blocks");
            } else {
                std::string bio = find_data_file(cfg.data_dir, exe_dir, "biomes-bedrock.txt");
                bedrock_set_biomes(world.biome_names, bio);
            }
        }
    }

    if (cfg.online_mode) {
        if (!auth.init(cfg.session_host, cfg.session_ca_file, cfg.prevent_proxy, err)) return false;
        auth.set_xbox_host(cfg.xbox_host);
        auth.start(2, [this](const AuthRequest& r, AuthResult&& res) { route_auth(r, std::move(res)); });
    }
    lists.load_all();
    reload_icon(true);
    {
        std::lock_guard<std::mutex> l(wm_);
        for (int i = 0; i < cfg.workers_min; i++)
            if (!spawn_worker(err)) return false;
    }
    game.start();
    mon_ = std::thread([this] { monitor(); });
    return true;
}

void Server::reload_icon(bool log_missing) {
    struct stat st;
    int64_t mt = stat("server-icon.png", &st) == 0 ? static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec : 0;
    if (mt == icon_mtime_) return;
    icon_mtime_ = mt;
    std::shared_ptr<const std::string> uri;
    if (mt) {
        std::ifstream f("server-icon.png", std::ios::binary);
        std::string png((std::istreambuf_iterator<char>(f)), {});
        // vanilla only shows a 64x64 PNG; anything else would be dropped by the client
        bool ok = png.size() > 33 && png.size() < 100000 && memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
        uint32_t w = 0, h = 0;
        if (ok) {
            for (int i = 0; i < 4; i++) { w = w << 8 | static_cast<uint8_t>(png[16 + i]); h = h << 8 | static_cast<uint8_t>(png[20 + i]); }
            ok = w == 64 && h == 64;
        }
        if (ok) { uri = std::make_shared<const std::string>("data:image/png;base64," + base64(png)); if (!log_missing) info("server-icon.png reloaded"); }
        else warn("server-icon.png must be a 64x64 PNG under 100 KB; ignoring it");
    } else if (log_missing) {
        // no icon is fine
    }
    std::lock_guard<std::mutex> l(fav_mu_);
    favicon_ = uri;
}

void Server::monitor() {
    pthread_setname_np(pthread_self(), "pn-monitor");
    struct Snap { uint64_t busy = 0; int64_t t = 0; };
    std::unordered_map<int, Snap> snaps;
    while (!stopping) {
        for (int i = 0; i < 10 && !stopping; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stopping) break;
        std::lock_guard<std::mutex> l(wm_);
        int64_t now = now_ms();
        double sum = 0;
        int active = 0;
        for (auto it = workers_.begin(); it != workers_.end();) {
            Worker& w = **it;
            if (w.finished) {
                w.join();
                info("Worker #" + std::to_string(w.id()) + " retired");
                it = workers_.erase(it);
                continue;
            }
            Snap& sn = snaps[w.id()];
            uint64_t busy = w.busy_ns.load();
            if (sn.t) {
                double dt = static_cast<double>(now - sn.t) * 1e6;
                sum += dt > 0 ? static_cast<double>(busy - sn.busy) / dt : 0;
            }
            sn.busy = busy;
            sn.t = now;
            active++;
            ++it;
        }
        double avg = active ? sum / active : 0;
        if (avg * 100 >= cfg.spawn_busy_pct && active < cfg.workers_max && mem_used.load() < mem_cap * 9 / 10) {
            std::string e;
            if (spawn_worker(e)) info("Worker #" + std::to_string(next_worker_id_ - 1) + " started (load " + std::to_string(int(avg * 100)) + "%)");
            else warn("could not start worker: " + e);
        } else if (active > cfg.workers_min) {
            for (auto& w : workers_) {
                if (w->id() != 0 && !w->finished && w->nconn.load() == 0 && now - w->idle_since_ms.load() > static_cast<int64_t>(cfg.retire_idle_s) * 1000) {
                    w->drain();
                    break;
                }
            }
        }
    }
}

void Server::shutdown() {
    stopping = true;
    auth.stop();
    if (mon_.joinable()) mon_.join();
    std::vector<std::unique_ptr<Worker>> ws;
    {
        std::lock_guard<std::mutex> l(wm_);
        ws.swap(workers_);  // workers call back into the server while closing connections
    }
    for (auto& w : ws) w->stop();
    for (auto& w : ws) w->join();
    game.stop();
    game.save_everything(false);
}

}  // namespace pn
