#include "config.hpp"
#include "log.hpp"
#include "server.hpp"
#include "util.hpp"
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace pn;

static const char* kVersion = "0.1.0";
static Server* g_server = nullptr;
static volatile sig_atomic_t g_sig = 0;

static void on_signal(int) { g_sig = 1; }

static void usage() {
    puts("Painite " "0.1.0" " - fast Minecraft server for Linux\n"
         "usage: painite [options]\n"
         "  --config FILE        extra settings (default painite.toml)\n"
         "  --port N             Java port (overrides server.properties)\n"
         "  --bedrock-port N|same\n"
         "  --cores LIST         e.g. 0-3 or 0,2,4 (overrides cpu.cores)\n"
         "  --max-workers N      cap on autoscaled workers\n"
         "  --memory-cap MB      total pool memory cap\n"
         "  --online | --offline  override online-mode (offline accepts cracked clients)\n"
         "  --no-bedrock         disable the built-in Bedrock listener\n"
         "  nogui, --nogui       accepted and ignored (vanilla compatibility)\n"
         "  --version, --help");
}

static std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p = buf;
    size_t s = p.rfind('/');
    return s == std::string::npos ? "." : p.substr(0, s);
}

static void console(Server& s) {
    std::string line;
    while (!g_sig && !s.stopping && std::getline(std::cin, line)) {
        line = trim(line);
        if (!line.empty() && line[0] == '/') line = line.substr(1);
        if (line.empty()) continue;
        std::string cmd = lower(line.substr(0, line.find(' ')));
        std::string arg = line.find(' ') == std::string::npos ? "" : trim(line.substr(line.find(' ') + 1));
        if (cmd == "stop") { info("Stopping the server"); g_sig = 1; return; }
        else if (cmd == "help" || cmd == "?") info("Commands: help, list, say <msg>, status, stop");
        else if (cmd == "list") {
            auto n = s.player_names();
            std::string o = "There are " + std::to_string(n.size()) + " of a max of " + std::to_string(s.cfg.max_players) + " players online:";
            for (size_t i = 0; i < n.size(); i++) o += (i ? ", " : " ") + n[i];
            info(o);
        } else if (cmd == "say") {
            info("[Server] " + arg);
            s.broadcast("[Server] " + arg);
        } else if (cmd == "status") info(s.console_status());
        else info("Unknown or incomplete command, see below for error");
    }
    // stdin closed (e.g. started detached): keep running until signalled
    while (!g_sig && !s.stopping) usleep(200000);
}

int main(int argc, char** argv) {
    std::string toml = "painite.toml";
    std::string cores_arg, port_arg, bport_arg;
    int max_workers = -1, mem_cap = -1;
    bool no_bedrock = false;
    int online_flag = -1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--version" || a == "-v") { printf("Painite %s\n", kVersion); return 0; }
        else if (a == "--config") toml = val();
        else if (a == "--port") port_arg = val();
        else if (a == "--bedrock-port") bport_arg = val();
        else if (a == "--cores") cores_arg = val();
        else if (a == "--max-workers") max_workers = atoi(val().c_str());
        else if (a == "--memory-cap") mem_cap = atoi(val().c_str());
        else if (a == "--no-bedrock") no_bedrock = true;
        else if (a == "--online") online_flag = 1;
        else if (a == "--offline") online_flag = 0;
        else if (a == "nogui" || a == "--nogui") {}
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    mkdir("logs", 0755);
    log_open("logs/latest.log");
    Server server;
    g_server = &server;
    server.exe_dir = exe_dir();
    std::string err;
    info(std::string("Starting Painite ") + kVersion);
    info("Loading properties");
    if (!load_config(server.cfg, toml, err)) { error(err); return 1; }
    if (!port_arg.empty()) server.cfg.java_port = atoi(port_arg.c_str());
    if (!bport_arg.empty()) server.cfg.bedrock_port = bport_arg == "same" ? -1 : atoi(bport_arg.c_str());
    if (!cores_arg.empty() && !parse_cores(cores_arg, server.cfg.cores)) { error("bad --cores value"); return 1; }
    if (max_workers >= 0) server.cfg.workers_max = max_workers;
    if (mem_cap > 0) server.cfg.memory_cap_mb = mem_cap;
    if (no_bedrock) server.cfg.bedrock = false;

    mkdir(server.cfg.level_name.c_str(), 0755);
    if (DIR* d = opendir("plugins")) {
        int jars = 0;
        while (auto* e = readdir(d)) { std::string n = e->d_name; if (n.size() > 4 && n.substr(n.size() - 4) == ".jar") jars++; }
        closedir(d);
        if (jars) warn("plugins/ contains " + std::to_string(jars) + " jar(s); Painite cannot load Java plugins and ignores them");
    }
    if (online_flag >= 0) server.cfg.online_mode = online_flag == 1;
    if (!server.cfg.online_mode) warn("online-mode=false: offline/cracked clients can join and players are not verified with Mojang");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int64_t t0 = now_ms();
    if (!server.start(err)) { error(err); return 1; }
    std::string ip = server.cfg.server_ip.empty() ? "*" : server.cfg.server_ip;
    info("Starting Minecraft server on " + ip + ":" + std::to_string(server.cfg.java_port));
    if (server.cfg.bedrock) info("Bedrock (RakNet) listening on UDP port " + std::to_string(server.cfg.bedrock_port_effective()));
    std::string vers;
    for (auto& d : server.data) vers += (vers.empty() ? "" : ", ") + d.version;
    info("Supported Java versions: " + vers + "; " + std::to_string(server.cores.size()) + " core(s), memory cap " + std::to_string(server.cfg.memory_cap_mb) + " MiB");
    char done[96];
    snprintf(done, sizeof done, "Done (%.3fs)! For help, type \"help\"", (now_ms() - t0) / 1000.0);
    info(done);

    console(server);
    info("Stopping server");
    server.shutdown();
    info("Server stopped");
    return 0;
}
