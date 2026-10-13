#include "data.hpp"
#include "util.hpp"
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <sstream>

namespace pn {

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

std::vector<std::string> list_vanilla_bins(const std::string& cfg_dir, const std::string& exe_dir) {
    std::vector<std::string> out;
    for (auto& d : {cfg_dir, std::string("data"), exe_dir + "/data", exe_dir + "/../data"}) {
        if (d.empty()) continue;
        DIR* dp = opendir(d.c_str());
        if (!dp) continue;
        while (auto* e = readdir(dp)) {
            std::string n = e->d_name;
            if (n.rfind("vanilla-", 0) == 0 && n.size() > 12 && n.substr(n.size() - 4) == ".bin") out.push_back(d + "/" + n);
        }
        closedir(dp);
        if (!out.empty()) break;
    }
    return out;
}

bool load_vanilla_file(const std::string& bin, VanillaData& v, std::string& err) {
    std::string ids = bin.substr(0, bin.size() - 4) + ".ids";

    std::vector<uint8_t> raw;
    if (!read_file(bin, raw) || raw.size() < 12 || std::memcmp(raw.data(), "PNTE", 4) != 0) {
        err = "bad data file " + bin;
        return false;
    }
    size_t pos = 4;
    auto u32 = [&](uint32_t& o) { if (pos + 4 > raw.size()) return false; std::memcpy(&o, &raw[pos], 4); pos += 4; return true; };
    uint32_t proto = 0, ncfg = 0;
    if (!u32(proto) || !u32(ncfg)) { err = "truncated data file"; return false; }
    v.protocol = static_cast<int>(proto);
    for (uint32_t i = 0; i < ncfg; i++) {
        uint32_t n;
        if (!u32(n) || pos + n > raw.size()) { err = "truncated data file"; return false; }
        v.config_packets.emplace_back(raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t n;
    if (!u32(n) || pos + n > raw.size()) { err = "truncated data file"; return false; }
    v.chunk_template.assign(raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    uint32_t nb = 0;
    if (!u32(nb)) { err = "data file lacks blobs; regenerate with tools/fetch_vanilla_data.py"; return false; }
    for (uint32_t i = 0; i < nb; i++) {
        if (pos >= raw.size()) { err = "truncated data file"; return false; }
        size_t nl = raw[pos++];
        std::string name(reinterpret_cast<char*>(&raw[pos]), nl);
        pos += nl;
        uint32_t len;
        if (!u32(len) || pos + len > raw.size()) { err = "truncated data file"; return false; }
        if (name == "set_time") v.set_time.assign(raw.begin() + pos, raw.begin() + pos + len);
        pos += len;
    }

    v.blocks_path = bin.substr(0, bin.size() - 4) + ".blocks";
    std::ifstream f(ids);
    if (!f) { err = "missing " + ids; return false; }
    std::map<std::string, int> tbl;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        if (kind == "version") ls >> v.version;
        else if (kind == "const") {
            std::string k; int val;
            ls >> k >> val;
            if (k == "dim_type_overworld") v.dim_type_overworld = val;
            else if (k == "biome_plains") v.biome_plains = val;
            else if (k == "data_version") v.data_version = val;
        } else if (kind == "reg") {
            std::string r, name; int num;
            ls >> r >> name >> num;
            v.reg[r + "/" + name] = num;
            if (r == "item") { if (v.items.size() <= static_cast<size_t>(num)) v.items.resize(static_cast<size_t>(num) + 1); v.items[static_cast<size_t>(num)] = "minecraft:" + name; }
            if (r == "biome") { if (v.biomes.size() <= static_cast<size_t>(num)) v.biomes.resize(static_cast<size_t>(num) + 1); v.biomes[static_cast<size_t>(num)] = "minecraft:" + name; }
        } else if (kind == "id") {
            std::string st, dir, name; int num;
            ls >> st >> dir >> name >> num;
            tbl[st + "/" + dir + "/" + name] = num;
            v.pid[st + "/" + dir + "/" + name] = num;
        }
    }
    bool ok = true;
    auto g = [&](const char* key) {
        auto it = tbl.find(key);
        if (it == tbl.end()) { err = std::string("packet id missing in ") + ids + ": " + key; ok = false; return 0; }
        return it->second;
    };
    auto& i = v.id;
    i.st_status_response = g("status/c/status_response"); i.st_pong_response = g("status/c/pong_response");
    i.st_status_request = g("status/s/status_request"); i.st_ping_request = g("status/s/ping_request");
    i.lg_disconnect = g("login/c/login_disconnect"); i.lg_finished = g("login/c/login_finished");
    i.lg_compression = g("login/c/login_compression"); i.lg_hello = g("login/s/hello");
    i.lg_acknowledged = g("login/s/login_acknowledged"); i.lg_encrypt_req = g("login/c/hello"); i.lg_encrypt_resp = g("login/s/key");
    i.cf_custom_payload = g("configuration/c/custom_payload"); i.cf_disconnect = g("configuration/c/disconnect");
    i.cf_finish = g("configuration/c/finish_configuration"); i.cf_keep_alive = g("configuration/c/keep_alive");
    i.cf_known_packs = g("configuration/c/select_known_packs"); i.cf_registry_data = g("configuration/c/registry_data");
    i.cfs_client_information = g("configuration/s/client_information"); i.cfs_finish = g("configuration/s/finish_configuration");
    i.cfs_keep_alive = g("configuration/s/keep_alive"); i.cfs_known_packs = g("configuration/s/select_known_packs");
    i.cfs_pong = g("configuration/s/pong");
    i.pl_login = g("play/c/login"); i.pl_abilities = g("play/c/player_abilities"); i.pl_commands = g("play/c/commands");
    i.pl_position = g("play/c/player_position"); i.pl_spawn = g("play/c/set_default_spawn_position");
    i.pl_game_event = g("play/c/game_event"); i.pl_center = g("play/c/set_chunk_cache_center"); i.pl_time = g("play/c/set_time");
    i.pl_batch_start = g("play/c/chunk_batch_start"); i.pl_batch_finish = g("play/c/chunk_batch_finished");
    i.pl_chunk = g("play/c/level_chunk_with_light"); i.pl_forget = g("play/c/forget_level_chunk");
    i.pl_keep_alive = g("play/c/keep_alive"); i.pl_disconnect = g("play/c/disconnect");
    i.pl_system_chat = g("play/c/system_chat"); i.pl_player_info = g("play/c/player_info_update");
    i.pls_teleport_confirm = g("play/s/accept_teleportation"); i.pls_chat = g("play/s/chat");
    i.pls_chat_command = g("play/s/chat_command"); i.pls_client_information = g("play/s/client_information");
    i.pls_keep_alive = g("play/s/keep_alive"); i.pls_move_pos = g("play/s/move_player_pos");
    i.pls_move_pos_rot = g("play/s/move_player_pos_rot"); i.pls_move_rot = g("play/s/move_player_rot");
    i.pls_move_status = g("play/s/move_player_status_only"); i.pls_batch_received = g("play/s/chunk_batch_received");
    i.pls_pong = g("play/s/pong");
    if (!ok) return false;

    // config packets we generate ourselves (brand) are dropped from the replay; remember the known-packs split
    std::vector<std::vector<uint8_t>> kept;
    for (auto& p : v.config_packets) {
        int32_t pid;
        read_varint(p.data(), p.size(), pid);
        if (pid == i.cf_custom_payload) continue;
        if (pid == i.cf_known_packs) v.index_known_packs = kept.size();
        kept.push_back(std::move(p));
    }
    v.config_packets = std::move(kept);
    if (v.set_time.empty()) { err = "data file has no set_time blob; regenerate with tools/fetch_vanilla_data.py"; return false; }
    return true;
}

}  // namespace pn
