// Console and player commands. Permission levels follow vanilla: 0 everyone, 2 gamemaster, 3 admin, 4 owner/console.
#include "game.hpp"
#include "cachestats.hpp"
#include "sim/entity_manager.hpp"
#include <algorithm>
#include <cstdio>
#include "dimension.hpp"
#include "structures.hpp"
#include "vg/gen.hpp"
#include "worldgen.hpp"
#include "hash.hpp"
#include "log.hpp"
#include "server.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace pn {

using Reply = std::function<void(const std::string&)>;

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> o;
    std::istringstream is(s);
    std::string t;
    while (is >> t) o.push_back(t);
    return o;
}

static std::string rest(const std::vector<std::string>& a, size_t from) {
    std::string o;
    for (size_t i = from; i < a.size(); i++) o += (i > from ? " " : "") + a[i];
    return o;
}

static bool parse_coord(const std::string& s, double cur, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    if (s[0] == '~') {
        double off = s.size() > 1 ? strtod(s.c_str() + 1, &end) : 0;
        if (s.size() > 1 && (!end || *end)) return false;
        out = cur + off;
        return true;
    }
    out = strtod(s.c_str(), &end);
    return end && !*end && std::isfinite(out);
}

static int parse_mode(const std::string& s) {
    std::string t = lower(s);
    if (t == "survival" || t == "s" || t == "0") return 0;
    if (t == "creative" || t == "c" || t == "1") return 1;
    if (t == "adventure" || t == "a" || t == "2") return 2;
    if (t == "spectator" || t == "sp" || t == "3") return 3;
    return -1;
}

static void set_property_line(const std::string& key, const std::string& val) {
    std::ifstream in("server.properties");
    std::vector<std::string> lines;
    std::string l;
    bool done = false;
    while (std::getline(in, l)) {
        if (l.rfind(key + "=", 0) == 0) { l = key + "=" + val; done = true; }
        lines.push_back(l);
    }
    in.close();
    if (!done) lines.push_back(key + "=" + val);
    std::ofstream out("server.properties", std::ios::trunc);
    for (auto& x : lines) out << x << "\n";
}

// "stone", "minecraft:oak_log[axis=x]" -> internal state id; false if the block does not exist
static bool parse_block(const std::string& spec, uint16_t& out) {
    Blocks& B = Blocks::get();
    std::string name = spec, props;
    size_t br = spec.find('[');
    if (br != std::string::npos && spec.back() == ']') {
        name = spec.substr(0, br);
        std::vector<std::pair<std::string, std::string>> kv;
        std::string inner = spec.substr(br + 1, spec.size() - br - 2);
        std::stringstream ss(inner);
        std::string t;
        while (std::getline(ss, t, ',')) {
            size_t eq = t.find('=');
            if (eq == std::string::npos) return false;
            kv.emplace_back(t.substr(0, eq), t.substr(eq + 1));
        }
        std::sort(kv.begin(), kv.end());
        for (auto& p : kv) props += (props.empty() ? "" : ",") + p.first + "=" + p.second;
    }
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    if (!B.known(name)) return false;
    out = B.find(name, props);
    if (!props.empty() && B.info(out).props != props) {
        // a partial property list is fine: fill in from the default state
        std::string base = B.info(B.default_state(name)).props;
        std::map<std::string, std::string> m;
        auto add = [&](const std::string& s) {
            size_t i = 0;
            while (i < s.size()) {
                size_t e = s.find(',', i);
                std::string t = s.substr(i, e == std::string::npos ? std::string::npos : e - i);
                size_t eq = t.find('=');
                m[t.substr(0, eq)] = t.substr(eq + 1);
                if (e == std::string::npos) break;
                i = e + 1;
            }
        };
        add(base);
        add(props);
        std::string merged;
        for (auto& p : m) merged += (merged.empty() ? "" : ",") + p.first + "=" + p.second;
        out = B.find(name, merged);
        if (B.info(out).props != merged) return false;
    }
    return true;
}

void Game::run_command(uint64_t gid, const std::string& line) {
    exec(gid, line, [this, gid](const std::string& m) {
        if (gid == 0) { info(m); return; }
        auto id = identity(gid);
        if (!id) return;
        s_.post_to_worker(id->worker, [conn = id->conn, m](Worker& w) { w.send_chat_to(conn, m); });
    });
}

void Game::announce(const std::string& text) {
    info(text);
    s_.broadcast(text);
}

void Game::exec(uint64_t gid, const std::string& raw, const Reply& reply) {
    std::string line = trim(raw);
    if (!line.empty() && line[0] == '/') line = line.substr(1);
    auto a = split(line);
    if (a.empty()) return;
    std::string cmd = lower(a[0]);
    if (cmd.rfind("minecraft:", 0) == 0) cmd = cmd.substr(10);
    bool list_commands = false;
    if (cmd == "painite" || cmd == "pn") {   // "painite <command> ..." runs any server command; "painite" alone, "painite help" or "painite commands" lists them
        if (a.size() == 1 || lower(a[1]) == "commands" || lower(a[1]) == "help" || lower(a[1]) == "?") { cmd = "help"; list_commands = true; }
        else { a.erase(a.begin()); cmd = lower(a[0]); }
    }
    int level = op_level(gid);
    std::string who = gid == 0 ? "Server" : (identity(gid) ? identity(gid)->name : "?");
    auto need = [&](int l) {
        if (level >= l) return true;
        reply("You do not have permission to use this command");
        return false;
    };
    auto find_target = [&](const std::string& t) -> std::shared_ptr<const GIdentity> {
        if ((t == "@s" || t == "@p") && gid) return identity(gid);
        return find_name(t);
    };
    auto missing = [&](const std::string& t) { reply("No player was found: " + t); };

    if (cmd == "help" || cmd == "?") {
        std::string o = "Commands: help, list, players, tps, me, tell/msg, version";
        if (level >= 2) o += ", say, tp, execute, locate, gamemode, time, setblock, fill, kill, summon, cache, perf, kick";
        if (level >= 3) o += ", ban, ban-ip, pardon, pardon-ip, banlist, whitelist, op, deop";
        if (level >= 4) o += ", save-all, save-on, save-off, stop";
        if (list_commands) o += ". Every command also works as 'painite <command>', e.g. 'painite tps'";
        reply(o);
    } else if (cmd == "list") {
        auto ids = identities();
        std::string o = "There are " + std::to_string(ids.size()) + " of a max of " + std::to_string(s_.cfg.max_players) + " players online:";
        for (size_t i = 0; i < ids.size(); i++) o += (i ? ", " : " ") + ids[i]->name;
        reply(o);
    } else if (cmd == "players" || cmd == "whois" || cmd == "playerinfo") {
        auto ids = identities();
        if (ids.empty()) { reply("Nobody is online"); return; }
        int64_t now = now_ms();
        char b[240];
        reply("Player            Edition  Ping     Coords (X, Y, Z)          Gamemode  Ticks Online");
        for (auto& id : ids) {
            GPos p = position(id->gid);
            int ping = id->ping_ms.load();
            char coords[64];
            snprintf(coords, sizeof coords, "%.1f, %.1f, %.1f", p.x, p.y, p.z);
            snprintf(b, sizeof b, "%-17s %-8s %4d ms  %-24s %-9d %lld", id->name.c_str(), id->bedrock ? "Bedrock" : "Java",
                     ping < 0 ? 0 : ping, coords, gamemode_of(id->gid), static_cast<long long>((now - id->joined_ms) / 50));
            reply(b);
        }
    } else if (cmd == "version" || cmd == "ver" || cmd == "about" || cmd == "info") {
        reply(s_.cfg.server_type + " " + s_.cfg.version_str + " (Minecraft Java + Bedrock native server)");
        reply("GitHub: " + s_.cfg.github_url);
    } else if (cmd == "plugins" || cmd == "pl") {
        reply("Plugins (0): Painite does not load Java plugins");
    } else if (cmd == "tps" || cmd == "status") {
        char b[200];
        snprintf(b, sizeof b, "TPS %.1f, tick %.2f ms, players %d, loaded chunks %d, block updates %lld (%lld changed, %zu queued, %d region threads), ", tps_, mspt_, players(), s_.world.loaded() + s_.other_dims[0]->loaded() + s_.other_dims[1]->loaded(),
                 static_cast<long long>(ticker_.processed.load()), static_cast<long long>(ticker_.changed.load()), ticker_.pending(), ticker_.threads() + 1);
        reply(std::string(b) + s_.console_status());
    } else if (cmd == "cache") {
        if (!need(2)) return;
        // every cache of the runtime: what is in it, what it takes, how often it answered (hits) and how often it had to do the work (misses)
        auto human = [](int64_t b) {
            char t[32];
            if (b < 1024) snprintf(t, sizeof t, "%lld B", (long long)b);
            else if (b < 1024 * 1024) snprintf(t, sizeof t, "%.1f KiB", double(b) / 1024.0);
            else snprintf(t, sizeof t, "%.1f MiB", double(b) / (1024.0 * 1024.0));
            return std::string(t);
        };
        for (const char* n : {"world chunks", "java chunk packets", "bedrock chunk tails", "path cells (per tick)", "tags", "loot tables", "biome spawn lists", "worldgen json"})
            cache_stat(n);   // listed even before they were used
        {   // caches that are measured on demand
            int64_t chunks = 0, bytes = 0;
            for (int d = 0; d < 3; d++) { int64_t c1, b1; s_.dim(d).memory_use(c1, b1); chunks += c1; bytes += b1; }
            CacheStat& wc = cache_stat("world chunks");
            wc.entries.store(chunks); wc.bytes.store(bytes);
            CacheStat& et = cache_stat("entity tracking");
            et.counts_lookups = false; et.entries.store(int64_t(entity_tracks_.size())); et.bytes.store(int64_t(entity_tracks_.size() * 240));
            CacheStat& bs = cache_stat("block collision shapes");
            bs.counts_lookups = false; bs.entries.store(int64_t(sim::BlockCollisionShapes::get().states())); bs.bytes.store(int64_t(sim::BlockCollisionShapes::get().memory_bytes()));
            CacheStat& bl = cache_stat("block state table");
            bl.counts_lookups = false; bl.entries.store(int64_t(Blocks::get().count())); bl.bytes.store(int64_t(Blocks::get().count() * sizeof(BlockInfo)));
            CacheStat& mp = cache_stat("memory pool (chunk packets, connections)");
            mp.counts_lookups = false; mp.bytes.store(s_.mem_used.load()); mp.limit_bytes = s_.mem_cap;
        }
        auto rows = cache_stats();
        std::sort(rows.begin(), rows.end(), [](CacheStat* a, CacheStat* b) { return a->name < b->name; });
        char line[256];
        snprintf(line, sizeof line, "%-40s %9s %11s %11s %11s %11s %6s", "cache", "entries", "size", "limit", "hits", "misses", "hit%");
        reply(line);
        int64_t total_bytes = 0;
        for (CacheStat* s : rows) {
            int64_t entries = s->entries.load(), bytes = s->bytes.load();
            if (s->sampler) s->sampler(entries, bytes);
            uint64_t h = s->hits.load(), m = s->misses.load();
            total_bytes += bytes;
            std::string size = human(bytes) + (s->bytes_are_source ? "*" : ""), limit = s->limit_bytes ? human(s->limit_bytes) : "-";
            char hitc[24] = "-", missc[24] = "-", pct[16] = "-";
            if (s->counts_lookups) {
                snprintf(hitc, sizeof hitc, "%llu", (unsigned long long)h);
                snprintf(missc, sizeof missc, "%llu", (unsigned long long)m);
                if (h + m) snprintf(pct, sizeof pct, "%.1f", 100.0 * double(h) / double(h + m));
            }
            snprintf(line, sizeof line, "%-40s %9lld %11s %11s %11s %11s %6s", s->name.c_str(), (long long)entries, size.c_str(), limit.c_str(), hitc, missc, pct);
            reply(line);
        }
        long rss_kb = 0;
        if (FILE* f = fopen("/proc/self/status", "r")) {
            char l2[128];
            while (fgets(l2, sizeof l2, f)) if (!strncmp(l2, "VmRSS:", 6)) rss_kb = atol(l2 + 6);
            fclose(f);
        }
        snprintf(line, sizeof line, "cached data listed: %s; process resident memory: %.1f MiB (* = size of the source files, the parsed form is larger)", human(total_bytes).c_str(), double(rss_kb) / 1024.0);
        reply(line);
    } else if (cmd == "perf") {
        for (auto& l : s_.perf_report()) reply(l);
    } else if (cmd == "me") {
        if (a.size() < 2) { reply("Usage: /me <action>"); return; }
        announce("* " + who + " " + rest(a, 1));
    } else if (cmd == "say") {
        if (!need(2)) return;
        if (a.size() < 2) { reply("Usage: /say <message>"); return; }
        announce("[" + who + "] " + rest(a, 1));
    } else if (cmd == "tell" || cmd == "msg" || cmd == "w") {
        if (a.size() < 3) { reply("Usage: /msg <player> <message>"); return; }
        auto t = find_target(a[1]);
        if (!t) { missing(a[1]); return; }
        std::string m = rest(a, 2);
        reply("You whisper to " + t->name + ": " + m);
        s_.post_to_worker(t->worker, [conn = t->conn, m, who](Worker& w) { w.send_chat_to(conn, who + " whispers to you: " + m); });
    } else if (cmd == "stop") {
        if (!need(4)) return;
        reply("Stopping the server");
        s_.stop_requested = true;
    } else if (cmd == "save-all") {
        if (!need(4)) return;
        reply("Saving the game (this may take a moment!)");
        save_everything(false);
        reply("Saved the game");
    } else if (cmd == "save-off") {
        if (!need(4)) return;
        saving_enabled = false;
        reply("Automatic saving is now disabled");
    } else if (cmd == "save-on") {
        if (!need(4)) return;
        saving_enabled = true;
        reply("Automatic saving is now enabled");
    } else if (cmd == "kick") {
        if (!need(3)) return;
        if (a.size() < 2) { reply("Usage: /kick <player> [reason]"); return; }
        std::string reason = a.size() > 2 ? rest(a, 2) : "Kicked by an operator";
        if (!s_.kick_player(a[1], reason)) { missing(a[1]); return; }
        reply("Kicked " + a[1] + ": " + reason);
    } else if (cmd == "ban") {
        if (!need(3)) return;
        if (a.size() < 2) { reply("Usage: /ban <player> [reason]"); return; }
        std::string reason = a.size() > 2 ? rest(a, 2) : "Banned by an operator.";
        auto t = find_name(a[1]);
        uint8_t u[16];
        if (t) memcpy(u, t->uuid, 16); else offline_uuid(a[1], u);
        if (!s_.lists.ban_add(t ? t->name : a[1], uuid_string(u), who, reason)) { reply("Nothing changed. That player is already banned"); return; }
        reply("Banned " + (t ? t->name : a[1]) + ": " + reason);
        if (t) s_.kick_player(t->name, "You are banned from this server.\nReason: " + reason);
    } else if (cmd == "ban-ip") {
        if (!need(3)) return;
        if (a.size() < 2) { reply("Usage: /ban-ip <address|player> [reason]"); return; }
        std::string ip = a[1], reason = a.size() > 2 ? rest(a, 2) : "Banned by an operator.";
        auto t = find_name(a[1]);
        if (t) ip = t->addr.substr(0, t->addr.rfind(':'));
        if (!s_.lists.ban_ip_add(ip, who, reason)) { reply("Nothing changed. That IP is already banned"); return; }
        reply("Banned IP " + ip + ": " + reason);
        for (auto& id : identities())
            if (id->addr.substr(0, id->addr.rfind(':')) == ip) s_.kick_player(id->name, "You have been IP banned from this server.\nReason: " + reason);
    } else if (cmd == "pardon") {
        if (!need(3)) return;
        if (a.size() < 2) { reply("Usage: /pardon <player>"); return; }
        reply(s_.lists.pardon(a[1]) ? "Unbanned " + a[1] : "Nothing changed. The player is not banned");
    } else if (cmd == "pardon-ip") {
        if (!need(3)) return;
        if (a.size() < 2) { reply("Usage: /pardon-ip <address>"); return; }
        reply(s_.lists.pardon_ip(a[1]) ? "Unbanned IP " + a[1] : "Nothing changed. That IP is not banned");
    } else if (cmd == "banlist") {
        if (!need(3)) return;
        bool ips = a.size() > 1 && lower(a[1]) == "ips";
        std::string o;
        int n = 0;
        if (ips) for (auto& e : s_.lists.ip_bans()) { o += (n++ ? ", " : " ") + e.ip; }
        else for (auto& e : s_.lists.bans()) { o += (n++ ? ", " : " ") + e.name; }
        reply("There are " + std::to_string(n) + " bans:" + o);
    } else if (cmd == "whitelist") {
        if (!need(3)) return;
        std::string sub = a.size() > 1 ? lower(a[1]) : "";
        auto enforce = [&] {   // enforce-whitelist: whoever is online and no longer listed goes
            if (!s_.cfg.white_list || !s_.cfg.enforce_whitelist) return;
            for (auto& id : identities()) {
                std::string u = uuid_string(id->uuid);
                if (!s_.whitelisted(id->name, u)) s_.kick_player(id->name, "You are not white-listed on this server!");
            }
        };
        if (sub == "on") { s_.cfg.white_list = true; set_property_line("white-list", "true"); reply("Whitelist is now turned on"); enforce(); }
        else if (sub == "off") { s_.cfg.white_list = false; set_property_line("white-list", "false"); reply("Whitelist is now turned off"); }
        else if (sub == "list") {
            auto n = s_.lists.whitelist_names();
            std::string o;
            for (size_t i = 0; i < n.size(); i++) o += (i ? ", " : " ") + n[i];
            reply("There are " + std::to_string(n.size()) + " whitelisted players:" + o);
        } else if (sub == "reload") { s_.lists.load_all(); reply("Reloaded the whitelist"); enforce(); }
        else if ((sub == "add" || sub == "remove") && a.size() > 2) {
            if (sub == "add") {
                auto t = find_name(a[2]);
                uint8_t u[16];
                if (t) memcpy(u, t->uuid, 16); else offline_uuid(a[2], u);
                reply(s_.lists.whitelist_add(t ? t->name : a[2], uuid_string(u)) ? "Added " + a[2] + " to the whitelist" : "Player is already whitelisted");
            } else { reply(s_.lists.whitelist_remove(a[2]) ? "Removed " + a[2] + " from the whitelist" : "Player is not whitelisted"); enforce(); }
        } else reply("Usage: /whitelist (on|off|list|reload|add <player>|remove <player>)");
    } else if (cmd == "op") {
        if (!need(3)) return;
        if (a.size() < 2) { reply("Usage: /op <player>"); return; }
        auto t = find_name(a[1]);
        uint8_t u[16];
        if (t) memcpy(u, t->uuid, 16); else offline_uuid(a[1], u);
        reply(s_.lists.op_add(t ? t->name : a[1], uuid_string(u), 4) ? "Made " + a[1] + " a server operator" : "Nothing changed. The player already is an operator");
    } else if (cmd == "deop") {
        if (!need(3)) return;
        if (a.size() < 2) { reply("Usage: /deop <player>"); return; }
        reply(s_.lists.op_remove(a[1]) ? "Made " + a[1] + " no longer a server operator" : "Nothing changed. The player is not an operator");
    } else if (cmd == "gamemode") {
        if (!need(2)) return;
        if (a.size() < 2) { reply("Usage: /gamemode <mode> [player]"); return; }
        int m = parse_mode(a[1]);
        if (m < 0) { reply("Unknown game mode: " + a[1]); return; }
        auto t = a.size() > 2 ? find_target(a[2]) : (gid ? identity(gid) : nullptr);
        if (!t) { if (a.size() > 2) missing(a[2]); else reply("Usage: /gamemode <mode> <player>"); return; }
        set_gamemode(t->gid, m);
        static const char* names[4] = {"Survival Mode", "Creative Mode", "Adventure Mode", "Spectator Mode"};
        reply("Set " + t->name + "'s game mode to " + names[m]);
    } else if (cmd == "time") {
        if (!need(2)) return;
        std::string sub = a.size() > 1 ? lower(a[1]) : "";
        if (sub == "query") {
            std::string w = a.size() > 2 ? lower(a[2]) : "daytime";
            if (w == "gametime") reply("The time is " + std::to_string(game_time()));
            else if (w == "day") reply("The time is " + std::to_string(day_time() / 24000));
            else reply("The time is " + std::to_string(day_time() % 24000));
        } else if ((sub == "set" || sub == "add") && a.size() > 2) {
            std::string v = lower(a[2]);
            int64_t t = 0;
            if (v == "day") t = 1000; else if (v == "noon") t = 6000; else if (v == "night") t = 13000; else if (v == "midnight") t = 18000;
            else { char* e = nullptr; t = strtoll(v.c_str(), &e, 10); if (!e || *e) { reply("Invalid time: " + a[2]); return; } }
            if (sub == "set") { int64_t base = day_time() - day_time() % 24000; set_day_time(base + t); }
            else set_day_time(day_time() + t);
            reply("Set the time to " + std::to_string(day_time()));
        } else reply("Usage: /time (set <value>|add <value>|query <daytime|gametime|day>)");
    } else if (cmd == "tp" || cmd == "teleport") {
        if (!need(2)) return;
        std::shared_ptr<const GIdentity> subject = gid ? identity(gid) : nullptr;
        size_t k = 1;
        GPos dest = subject ? position(subject->gid) : GPos{};
        auto is_num = [](const std::string& s) { return !s.empty() && (s[0] == '~' || s[0] == '-' || s[0] == '.' || isdigit(static_cast<unsigned char>(s[0]))); };
        if (a.size() == 2 && !is_num(a[1])) {   // /tp <player>  (move myself to them)
            auto t = find_target(a[1]);
            if (!t) { missing(a[1]); return; }
            if (!subject) { reply("Only a player can teleport to a player"); return; }
            dest = position(t->gid);
        } else {
            if (a.size() >= 2 && !is_num(a[1])) {
                subject = find_target(a[1]);
                if (!subject) { missing(a[1]); return; }
                dest = position(subject->gid);
                k = 2;
            }
            if (!subject) { reply("Usage: /tp <x> <y> <z> | /tp <player> [<x> <y> <z> | <player>]"); return; }
            if (a.size() == k + 1 && !is_num(a[k])) {
                auto t = find_target(a[k]);
                if (!t) { missing(a[k]); return; }
                dest = position(t->gid);
            } else if (a.size() >= k + 3) {
                GPos cur = position(subject->gid);
                if (!parse_coord(a[k], cur.x, dest.x) || !parse_coord(a[k + 1], cur.y, dest.y) || !parse_coord(a[k + 2], cur.z, dest.z)) { reply("Invalid coordinates"); return; }
                if (a.size() >= k + 5) { double yw = dest.yaw, pt = dest.pitch; if (parse_coord(a[k + 3], cur.yaw, yw)) dest.yaw = static_cast<float>(yw); if (parse_coord(a[k + 4], cur.pitch, pt)) dest.pitch = static_cast<float>(pt); }
            } else { reply("Usage: /tp <x> <y> <z> | /tp <player> [<x> <y> <z> | <player>]"); return; }
        }
        dest.head = dest.yaw;
        if (exec_dim_ >= 0) dest.dim = exec_dim_;
        teleport(subject->gid, dest);
        char b[160];
        snprintf(b, sizeof b, "Teleported %s to %.2f, %.2f, %.2f", subject->name.c_str(), dest.x, dest.y, dest.z);
        reply(b);
    } else if (cmd == "locate") {
        if (!need(2)) return;
        std::string name = a.size() >= 3 ? a[2] : a.size() == 2 ? a[1] : "";
        GPos cur = gid ? position(gid) : GPos{};
        int d = exec_dim_ >= 0 ? exec_dim_ : cur.dim;
        int ox, oz;
        if (World& w = s_.dim(d); w.vgen) {   // Mojang's generator: structures by id ("village_plains", "minecraft:monument", "#minecraft:village")
            std::string id = lower(name);
            if (id.rfind("structure ", 0) == 0) id = id.substr(10);
            if (id.empty() || !w.vgen->locate({id}, static_cast<int>(std::floor(cur.x)), static_cast<int>(std::floor(cur.z)), ox, oz)) {
                reply("Could not find a structure of that type nearby");
                return;
            }
        } else {
            const StructureGen* sg = w.gen ? w.gen->structures() : nullptr;
            if (!sg) { reply("This world has no generated structures"); return; }
            if (name.empty() || !sg->locate(lower(name), static_cast<int>(std::floor(cur.x)), static_cast<int>(std::floor(cur.z)), ox, oz)) {
                std::string o = "Could not find that structure nearby. Here: ";
                for (auto& k : sg->kinds()) o += k + " ";
                reply(o);
                return;
            }
        }
        double dist = std::sqrt((ox - cur.x) * (ox - cur.x) + (oz - cur.z) * (oz - cur.z));
        char b[200];
        snprintf(b, sizeof b, "The nearest %s is at [%d, ~, %d] (%.0f blocks away)", name.c_str(), ox, oz, dist);
        reply(b);
    } else if (cmd == "execute") {
        if (!need(2)) return;
        size_t i = 1;
        int saved = exec_dim_;
        while (i < a.size() && lower(a[i]) != "run") {
            if (lower(a[i]) == "in" && i + 1 < a.size()) { exec_dim_ = dim_from_name(lower(a[i + 1])); i += 2; }
            else { reply("Usage: /execute in <overworld|the_nether|the_end> run <command>"); exec_dim_ = saved; return; }
        }
        if (i + 1 >= a.size()) { reply("Usage: /execute in <overworld|the_nether|the_end> run <command>"); exec_dim_ = saved; return; }
        std::string rest;
        for (size_t k = i + 1; k < a.size(); k++) rest += (rest.empty() ? "" : " ") + a[k];
        exec(gid, rest, reply);
        exec_dim_ = saved;
    } else if (cmd == "setblock") {
        if (!need(2)) return;
        if (a.size() < 5) { reply("Usage: /setblock <x> <y> <z> <block>"); return; }
        GPos cur = gid ? position(gid) : GPos{};
        double x, y, z;
        uint16_t id;
        if (!parse_coord(a[1], cur.x, x) || !parse_coord(a[2], cur.y, y) || !parse_coord(a[3], cur.z, z)) { reply("Invalid coordinates"); return; }
        if (!parse_block(a[4], id)) { reply("Unknown block type: " + a[4]); return; }
        int bx = static_cast<int>(std::floor(x)), by = static_cast<int>(std::floor(y)), bz = static_cast<int>(std::floor(z));
        int d = exec_dim_ >= 0 ? exec_dim_ : cur.dim;
        const DimInfo& di = dim_info(d);
        if (by < di.min_y || by >= di.min_y + di.height) { reply("That position is out of the world"); return; }
        if (!s_.dim(d).set_block(bx, by, bz, id)) { reply("Could not set the block"); return; }
        block_changed(d, bx, by, bz, id);
        reply("Changed the block at " + std::to_string(bx) + ", " + std::to_string(by) + ", " + std::to_string(bz));
    } else if (cmd == "fill") {
        if (!need(2)) return;
        if (a.size() < 8) { reply("Usage: /fill <x1> <y1> <z1> <x2> <y2> <z2> <block>"); return; }
        GPos cur = gid ? position(gid) : GPos{};
        double v[6];
        for (int i = 0; i < 6; i++) if (!parse_coord(a[static_cast<size_t>(i) + 1], i % 3 == 0 ? cur.x : i % 3 == 1 ? cur.y : cur.z, v[i])) { reply("Invalid coordinates"); return; }
        uint16_t id;
        if (!parse_block(a[7], id)) { reply("Unknown block type: " + a[7]); return; }
        int x1 = static_cast<int>(std::floor(std::min(v[0], v[3]))), x2 = static_cast<int>(std::floor(std::max(v[0], v[3])));
        int d = exec_dim_ >= 0 ? exec_dim_ : cur.dim;
        const DimInfo& di = dim_info(d);
        int y1 = std::max(di.min_y, static_cast<int>(std::floor(std::min(v[1], v[4])))), y2 = std::min(di.min_y + di.height - 1, static_cast<int>(std::floor(std::max(v[1], v[4]))));
        int z1 = static_cast<int>(std::floor(std::min(v[2], v[5]))), z2 = static_cast<int>(std::floor(std::max(v[2], v[5])));
        long total = static_cast<long>(x2 - x1 + 1) * (y2 - y1 + 1) * (z2 - z1 + 1);
        if (total > 32768) { reply("Too many blocks in the specified area (maximum 32768, specified " + std::to_string(total) + ")"); return; }
        long n = 0;
        for (int x = x1; x <= x2; x++) for (int y = y1; y <= y2; y++) for (int z = z1; z <= z2; z++)
            if (s_.dim(d).set_block(x, y, z, id)) { block_changed(d, x, y, z, id); n++; }
        reply("Successfully filled " + std::to_string(n) + " block(s)");
    } else if (cmd == "kill") {
        if (!need(2)) return;
        auto t = a.size() > 1 ? find_target(a[1]) : (gid ? identity(gid) : nullptr);
        if (!t) { if (a.size() > 1) missing(a[1]); else reply("A player is required to run this command here"); return; }
        kill_player(t->gid);
        reply("Killed " + t->name);
    } else if (cmd == "summon") {
        if (!need(2)) return;
        if (a.size() < 2) { reply("Usage: /summon <entity> [x y z]"); return; }
        GPos cur = gid ? position(gid) : GPos();
        double x = cur.x, y = cur.y, z = cur.z;
        if (a.size() >= 5 && (!parse_coord(a[2], cur.x, x) || !parse_coord(a[3], cur.y, y) || !parse_coord(a[4], cur.z, z))) { reply("Invalid coordinates"); return; }
        if (summon(a[1], cur.dim, x, y, z)) reply("Summoned new " + a[1]);
        else reply("Unable to summon entity " + a[1] + " (Painite only knows living mobs)");
    } else if (cmd == "give" || cmd == "weather" || cmd == "difficulty" || cmd == "effect" || cmd == "xp" || cmd == "enchant") {
        reply("/" + cmd + " is not implemented in Painite yet (no inventory, mobs or weather)");
    } else {
        reply("Unknown or incomplete command, see below for error");
    }
}

}  // namespace pn
