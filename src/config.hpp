// server.properties (vanilla layout) + painite.toml (extras), with CLI overrides on top.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace pn {

struct Config {
    // vanilla server.properties
    int java_port = 25565;
    std::string server_ip;
    std::string motd = "A Painite Server";
    int max_players = 20;
    bool online_mode = false;
    bool white_list = false;
    bool enforce_whitelist = false;   // kick online players who are not on the whitelist when it changes (vanilla enforce-whitelist)
    bool prevent_proxy = false;  // prevent-proxy-connections: pass the client IP to the session server
    std::string session_host = "sessionserver.mojang.com";  // host or host:port
    std::string xbox_host = "authorization.franchise.minecraft-services.net";  // Bedrock login token issuer and key server
    std::string session_ca_file;  // optional CA bundle for private auth servers
    int files_poll_ms = 3000;     // how often ops, bans, whitelist and server-icon.png are checked for edits
    int view_distance = 10;
    int gamemode = 0;  // 0 survival, 1 creative, 2 adventure, 3 spectator
    int compression_threshold = 256;
    std::string level_name = "world";
    std::string level_type = "normal";   // normal | flat | void | large_biomes | amplified
    std::string level_seed;
    int difficulty = -1;                  // -1 = whatever level.dat says
    bool hardcore = false;
    int simulation_distance = 10;

    // painite.toml
    bool bedrock = true;
    int bedrock_port = -1;  // -1 = "same" as java_port
    std::string bedrock_version = "1.26.52";
    bool bedrock_encryption = true;       // ServerToClientHandshake + AES-256 like vanilla Bedrock servers
    bool bedrock_unauthenticated = false; // allow Bedrock logins while Java online-mode=true (no Xbox verification yet)
    std::string bedrock_prefix = ".";     // prepended to Bedrock names, like Floodgate
    int bedrock_protocol = 2193;
    std::string profile = "auto";  // auto | lan | wan | lossy
    int compression_level = 3;
    int chunk_cache_mb = 256;
    int backlog_cap_kb = 256;  // per-connection outbound backlog before chunk sending pauses
    std::vector<int> cores;    // empty = every core this process may use
    bool pin = true;
    int workers_min = 1;
    int workers_max = 0;       // 0 = number of cores
    int memory_cap_mb = 1024;  // total pool cap (connection buffers + chunk cache)
    int spawn_busy_pct = 70;
    int retire_idle_s = 60;
    std::string generator = "auto";  // chunks that are not on disk: auto (terrain for a new world, void for an existing one) | terrain | flat | void | vanilla
    int autosave_s = 300;
    int tick_threads = 0;   // threads that tick regions in parallel (0 = automatic)
    int spawn_protection = 0;
    std::string data_dir;      // where vanilla-<ver>.bin/.ids live; empty = search

    // Server Info metadata
    std::string server_type = "Painite";
    std::string version_str = "0.1.0";
    std::string github_url = "https://github.com/BeanGreen247/painite";
    std::vector<std::string> startup_commands;

    int bedrock_port_effective() const { return bedrock_port < 0 ? java_port : bedrock_port; }
};

bool parse_cores(const std::string& spec, std::vector<int>& out);
// Loads server.properties (creating it with defaults if missing) and painite.toml if present.
bool load_config(Config& c, const std::string& toml_path, std::string& err);

}  // namespace pn
