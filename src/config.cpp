#include "config.hpp"
#include "log.hpp"
#include "util.hpp"
#include <cstdlib>
#include <fstream>
#include <sched.h>
#include <sstream>

namespace pn {

bool parse_cores(const std::string& spec, std::vector<int>& out) {
    out.clear();
    std::string s = lower(trim(spec));
    if (s.empty() || s == "all") return true;
    for (char& ch : s) if (ch == '[' || ch == ']' || ch == '"') ch = ' ';
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (tok.empty()) continue;
        size_t d = tok.find('-');
        char* end = nullptr;
        if (d == std::string::npos) {
            long v = strtol(tok.c_str(), &end, 10);
            if (*end || v < 0 || v >= CPU_SETSIZE) return false;
            out.push_back(static_cast<int>(v));
        } else {
            long a = strtol(tok.substr(0, d).c_str(), &end, 10);
            if (*end) return false;
            long b = strtol(tok.substr(d + 1).c_str(), &end, 10);
            if (*end || a < 0 || b < a || b >= CPU_SETSIZE) return false;
            for (long v = a; v <= b; v++) out.push_back(static_cast<int>(v));
        }
    }
    return true;
}

static const char* kDefaultProps =
    "#Minecraft server properties (Painite)\n"
    "server-port=25565\n"
    "server-ip=\n"
    "motd=A Painite Server\n"
    "max-players=20\n"
    "online-mode=false\n"
    "white-list=false\n"
    "enforce-whitelist=false\n"
    "view-distance=10\n"
    "gamemode=survival\n"
    "network-compression-threshold=256\n"
    "level-name=world\n";

static std::map<std::string, std::string> read_props(const std::string& path) {
    std::map<std::string, std::string> m;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        m[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return m;
}

static bool to_bool(const std::string& v, bool d) {
    std::string s = lower(v);
    if (s == "true") return true;
    if (s == "false") return false;
    return d;
}

static int to_int(const std::string& v, int d) {
    char* end = nullptr;
    long x = strtol(v.c_str(), &end, 10);
    return (end && *end == 0 && !v.empty()) ? static_cast<int>(x) : d;
}

// Minimal TOML: [section], key = value, # comments; values: "string", number, bool, [array].
static std::map<std::string, std::string> read_toml(const std::string& path, bool& found) {
    std::map<std::string, std::string> m;
    std::ifstream f(path);
    found = f.good();
    std::string line, section;
    while (std::getline(f, line)) {
        bool inq = false;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '"') inq = !inq;
            if (line[i] == '#' && !inq) { line.resize(i); break; }
        }
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '[') { section = trim(line.substr(1, line.size() - 2)); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        m[section + "." + trim(line.substr(0, eq))] = v;
    }
    return m;
}

bool load_config(Config& c, const std::string& toml_path, std::string& err) {
    {
        std::ifstream t("server.properties");
        if (!t.good()) {
            std::ofstream o("server.properties");
            o << kDefaultProps;
            info("Created default server.properties");
        }
    }
    auto p = read_props("server.properties");
    auto get = [&](const char* k) -> const std::string* { auto it = p.find(k); return it == p.end() ? nullptr : &it->second; };
    if (auto v = get("server-port")) c.java_port = to_int(*v, c.java_port);
    if (auto v = get("server-ip")) c.server_ip = *v;
    if (auto v = get("motd")) c.motd = *v;
    if (auto v = get("max-players")) c.max_players = to_int(*v, c.max_players);
    if (auto v = get("online-mode")) c.online_mode = to_bool(*v, c.online_mode);
    if (auto v = get("prevent-proxy-connections")) c.prevent_proxy = to_bool(*v, false);
    if (auto v = get("white-list")) c.white_list = to_bool(*v, false);
    if (auto v = get("view-distance")) c.view_distance = to_int(*v, c.view_distance);
    if (auto v = get("network-compression-threshold")) c.compression_threshold = to_int(*v, c.compression_threshold);
    if (auto v = get("level-name")) c.level_name = *v;
    if (auto v = get("gamemode")) {
        std::string g = lower(*v);
        c.gamemode = g == "creative" ? 1 : g == "adventure" ? 2 : g == "spectator" ? 3 : g == "survival" ? 0 : to_int(g, 0);
    }
    if (c.view_distance < 2) c.view_distance = 2;
    if (c.view_distance > 32) c.view_distance = 32;

    bool found = false;
    auto t = read_toml(toml_path, found);
    auto tget = [&](const char* k) -> const std::string* { auto it = t.find(k); return it == t.end() ? nullptr : &it->second; };
    if (auto v = tget("network.bedrock")) c.bedrock = to_bool(*v, c.bedrock);
    if (auto v = tget("network.bedrock_port")) c.bedrock_port = lower(*v) == "same" ? -1 : to_int(*v, -1);
    if (auto v = tget("network.bedrock_version")) c.bedrock_version = *v;
    if (auto v = tget("network.bedrock_encryption")) c.bedrock_encryption = to_bool(*v, c.bedrock_encryption);
    if (auto v = tget("network.bedrock_unauthenticated")) c.bedrock_unauthenticated = to_bool(*v, false);
    if (auto v = tget("network.bedrock_prefix")) c.bedrock_prefix = *v;
    if (auto v = tget("network.bedrock_protocol")) c.bedrock_protocol = to_int(*v, c.bedrock_protocol);
    if (auto v = tget("bandwidth.profile")) c.profile = lower(*v);
    if (auto v = tget("bandwidth.compression_level")) c.compression_level = to_int(*v, c.compression_level);
    if (auto v = tget("bandwidth.chunk_cache_mb")) c.chunk_cache_mb = to_int(*v, c.chunk_cache_mb);
    if (auto v = tget("bandwidth.backlog_cap_kb")) c.backlog_cap_kb = to_int(*v, c.backlog_cap_kb);
    if (auto v = tget("cpu.cores")) {
        if (!parse_cores(*v, c.cores)) { err = "painite.toml: bad cpu.cores value '" + *v + "'"; return false; }
    }
    if (auto v = tget("cpu.pin")) c.pin = to_bool(*v, c.pin);
    if (auto v = tget("workers.min")) c.workers_min = to_int(*v, c.workers_min);
    if (auto v = tget("workers.max")) c.workers_max = to_int(*v, c.workers_max);
    if (auto v = tget("workers.memory_cap_mb")) c.memory_cap_mb = to_int(*v, c.memory_cap_mb);
    if (auto v = tget("workers.spawn_busy_pct")) c.spawn_busy_pct = to_int(*v, c.spawn_busy_pct);
    if (auto v = tget("workers.retire_after_idle_s")) c.retire_idle_s = to_int(*v, c.retire_idle_s);
    if (auto v = tget("auth.session_host")) c.session_host = *v;
    if (auto v = tget("auth.xbox_host")) c.xbox_host = *v;
    if (auto v = tget("auth.session_ca_file")) c.session_ca_file = *v;
    if (auto v = tget("world.generator")) c.generator = lower(*v);
    if (auto v = tget("world.autosave_s")) c.autosave_s = to_int(*v, c.autosave_s);
    if (auto v = get("spawn-protection")) c.spawn_protection = to_int(*v, 0);
    if (auto v = tget("paths.data_dir")) c.data_dir = *v;
    if (c.profile == "lan") c.compression_threshold = -1;
    return true;
}

}  // namespace pn
