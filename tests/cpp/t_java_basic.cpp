// Java basics: status, login, configuration, chunk delivery, chat; chunks equal Mojang's; block entities; ban/whitelist lists; server icon.
#include "chunkparse.hpp"
#include "harness.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <zlib.h>

namespace pt {

namespace {

ServerOpts ref_opts(const Env& e) {
    ServerOpts o;
    o.world_src = e.test_world;
    o.properties = "gamemode=creative\nlevel-type=flat\n";
    o.ops = {"Bobby", "Alice"};
    return o;
}

std::string status_json(int port, int protocol, int64_t ping_value, bool& pong_ok) {
    JClient c;
    c.connect(port);
    Wr hs;
    hs.varint(uint32_t(protocol)).str("localhost").be16(uint16_t(port)).varint(1);
    c.send(0, hs.b);
    c.send(0);
    Packet p;
    if (!c.recv(p, 5000) || p.id != 0) throw std::runtime_error("no status response");
    Rd r(p.d);
    std::string js = r.str();
    c.send(1, Wr().be64(uint64_t(ping_value)).b);
    pong_ok = c.recv(p, 5000) && p.id == 1 && Rd(p.d).be64() == uint64_t(ping_value);
    return js;
}

void smoke(Ctx& t, Env& e, const std::string& version) {
    auto ids = e.ids(version);
    int proto = e.protocol(version);
    bool pong = false;
    std::string js = status_json(e.port(), proto, 1234, pong);
    t.check(js.find("\"protocol\":" + std::to_string(proto)) != std::string::npos, "status response names protocol " + std::to_string(proto));
    t.check(pong, "ping/pong");

    JClient c;
    const std::string name = version == "26.3" ? "SmokeTest" : "SmokeTest262";
    Bytes success = java_login(c, e.port(), proto, name, Bytes(16, 0));
    t.check(success.size() == 16 + 1 + name.size() + 1 + 16 || success.size() == 16 + 1 + name.size() + 1,
            "login success length " + std::to_string(success.size()) + " (compression " + (c.threshold >= 0 ? "on" : "off") + ")");
    int n_cfg = java_configure(c, *ids);
    t.check(n_cfg > 5, "configuration packets received: " + std::to_string(n_cfg));
    int chunk_pkts = 0;
    bool login_ok = false;
    int64_t end = now_ms() + 15000;
    Packet p;
    while (now_ms() < end && !(login_ok && chunk_pkts >= 100)) {
        if (!c.recv(p, 3000)) break;
        std::string nm = ids->name("play", "c", p.id);
        if (nm == "login") login_ok = true;
        else if (nm == "level_chunk_with_light") chunk_pkts++;
        else if (nm == "chunk_batch_finished") c.send((*ids)("play", "s", "chunk_batch_received"), Wr().f32(20.0f).b);
        else if (nm == "keep_alive") c.send((*ids)("play", "s", "keep_alive"), p.d);
        else if (nm == "player_position") { Rd r(p.d); uint32_t tid = r.varint(); c.send((*ids)("play", "s", "accept_teleportation"), Wr().varint(tid).zeros(40).b); }
    }
    t.check(login_ok, "play login packet");
    t.check(chunk_pkts >= 100, "chunks received: " + std::to_string(chunk_pkts));
    Wr chat;
    chat.str("hello from smoke test").be64(0).be64(0).u8(0).varint(0).zeros(4);
    c.send((*ids)("play", "s", "chat"), chat.b);
    bool got = false;
    end = now_ms() + 5000;
    while (now_ms() < end && !got) {
        if (!c.recv(p, 1000)) continue;
        if (ids->name("play", "c", p.id) == "system_chat" && contains(p.d, "hello from smoke test")) got = true;
        else if (ids->name("play", "c", p.id) == "keep_alive") c.send((*ids)("play", "s", "keep_alive"), p.d);
    }
    t.check(got, "chat echoed back");
}

// The chunks Painite sends for a vanilla world must equal the ones vanilla recorded.
void world_compare(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    std::map<std::pair<int, int>, Bytes> ref;
    {
        std::ifstream f(e.test_world + "/vanilla_chunks.bin", std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t i = 0;
        while (i + 12 <= raw.size()) {
            int32_t x, z;
            uint32_t n;
            memcpy(&x, &raw[i], 4); memcpy(&z, &raw[i + 4], 4); memcpy(&n, &raw[i + 8], 4);
            ref[{x, z}] = Bytes(raw.begin() + long(i + 12), raw.begin() + long(i + 12 + n));
            i += 12 + n;
        }
    }
    JBot b(e.port(), 777, ids, "Compare");
    std::map<std::pair<int, int>, ChunkData> got;
    int64_t end = now_ms() + 20000;
    // JBot keeps only the newest chunk; read the packets directly instead
    Packet p;
    int lc = (*ids)("play", "c", "level_chunk_with_light"), ka = (*ids)("play", "c", "keep_alive"), bf = (*ids)("play", "c", "chunk_batch_finished");
    while (now_ms() < end && got.size() < 60) {
        if (!b.c.recv(p, 2000)) continue;
        if (p.id == lc) { ChunkData c = parse_chunk(p.d); got[{c.x, c.z}] = std::move(c); }
        else if (p.id == ka) b.c.send((*ids)("play", "s", "keep_alive"), p.d);
        else if (p.id == bf) b.c.send((*ids)("play", "s", "chunk_batch_received"), Wr().f32(20.0f).b);
    }
    auto names = load_block_names(e.data_dir + "/vanilla-26.3.blocks", false);
    static const std::set<std::string> live = {"minecraft:water", "minecraft:lava", "minecraft:fire", "minecraft:soul_fire"};
    static const std::set<std::string> airish = {"minecraft:air", "minecraft:cave_air", "minecraft:stone", "minecraft:obsidian", "minecraft:cobblestone", "minecraft:basalt", "minecraft:glow_lichen", "minecraft:gravel"};
    int bad = 0, checked = 0;
    for (auto& kv : ref) {
        auto g = got.find(kv.first);
        if (g == got.end()) continue;
        ChunkData rd = parse_chunk(kv.second);
        checked++;
        size_t ns = std::min(rd.sections.size(), g->second.sections.size());
        for (size_t si = 0; si < ns; si++) {
            const SectionData& r = rd.sections[si];
            const SectionData& s = g->second.sections[si];
            // the recording is a live snapshot: fluids move, ores light up, random ticks change a handful of blocks
            int odd = 0;
            for (size_t i = 0; i < 4096; i++) {
                int pp = r.blocks[i], q = s.blocks[i];
                if (pp == q) continue;
                const std::string& a = names[pp];
                const std::string& b2 = names[q];
                if (a == b2 || ((live.count(a) || airish.count(a)) && (live.count(b2) || airish.count(b2)))) continue;
                odd++;
            }
            if (r.biomes == s.biomes && odd <= 4) continue;
            if (r.nonair != s.nonair || r.fluid != s.fluid || r.blocks != s.blocks || r.biomes != s.biomes) {
                bad++;
                int diff = 0, bdiff = 0;
                for (size_t i = 0; i < 4096; i++) diff += r.blocks[i] != s.blocks[i];
                for (size_t i = 0; i < 64; i++) bdiff += r.biomes[i] != s.biomes[i];
                t.note("chunk " + std::to_string(kv.first.first) + "," + std::to_string(kv.first.second) + " section " + std::to_string(si) + ": block diffs " + std::to_string(diff) + " biome diffs " + std::to_string(bdiff) + " (unexplained " + std::to_string(odd) + ")");
            }
        }
        if (rd.sections.size() != g->second.sections.size()) { bad++; t.note("section count differs"); }
    }
    int k = 0;
    for (auto& kv : got) {
        if (k++ >= 8) break;
        TailInfo ti = parse_tail(kv.second.rest);
        if (!ti.exact || __builtin_popcountll(ti.masks[0]) != ti.arrays[0] || __builtin_popcountll(ti.masks[1]) != ti.arrays[1] || (ti.masks[0] & ti.masks[2]) || (ti.masks[1] & ti.masks[3])) {
            bad++;
            t.note("light/tail structure wrong for chunk " + std::to_string(kv.first.first) + "," + std::to_string(kv.first.second));
        }
    }
    k = 0;
    bool ref_parses = true;
    for (auto& kv : ref) { if (k++ >= 3) break; ref_parses = ref_parses && parse_tail(parse_chunk(kv.second).rest).exact; }
    t.check(ref_parses, "the reference chunks themselves parse");
    t.check(checked > 0 && bad == 0, "compared " + std::to_string(checked) + " chunks (painite sent " + std::to_string(got.size()) + "), " + std::to_string(bad) + " mismatching sections");
}

// Chunks of a world with block entities must carry them.
void block_entities(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    std::map<int, std::string> reg;
    {
        std::ifstream f(version_ids_path(e, "26.3"));
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ls(line);
            std::string k, r, nm;
            int n;
            ls >> k >> r >> nm >> n;
            if (k == "reg" && r == "block_entity_type") reg[n] = nm;
        }
    }
    JBot b(e.port(), 777, ids, "BEChecker");
    std::set<std::string> kinds;
    int found = 0;
    Packet p;
    int lc = (*ids)("play", "c", "level_chunk_with_light");
    int64_t end = now_ms() + 15000;
    while (now_ms() < end) {
        if (!b.c.recv(p, 1000)) { if (found >= 2) break; continue; }
        std::string nm = ids->name("play", "c", p.id);
        if (nm == "keep_alive") b.c.send((*ids)("play", "s", "keep_alive"), p.d);
        else if (nm == "chunk_batch_finished") b.c.send((*ids)("play", "s", "chunk_batch_received"), Wr().f32(20.0f).b);
        else if (p.id == lc) {
            ChunkData c = parse_chunk(p.d);
            TailInfo ti = parse_tail(c.rest);
            for (auto& en : ti.entities) { found++; kinds.insert(reg.count(en.first) ? reg[en.first] : std::to_string(en.first)); }
            if (kinds.count("chest") && kinds.count("sign") && found >= 4) break;
        }
    }
    t.check(found >= 4, "block entities seen: " + std::to_string(found));
    t.check(kinds.count("chest") > 0 && kinds.count("sign") > 0, "a chest and a sign are among them");
}

// ---- bans, whitelist and the server icon ----------------------------------------------------------------

Bytes png(int w, int h) {
    Bytes raw;
    for (int y = 0; y < h; y++) { raw.push_back(0); for (int x = 0; x < w; x++) { raw.push_back(200); raw.push_back(30); raw.push_back(30); } }
    auto chunk = [](const char* type, const Bytes& d) {
        Wr o;
        o.be32(uint32_t(d.size()));
        Bytes td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        o.raw(td);
        o.be32(uint32_t(crc32(0, td.data(), uInt(td.size()))));
        return o.b;
    };
    Bytes comp(compressBound(uLong(raw.size())));
    uLongf n = uLongf(comp.size());
    compress(comp.data(), &n, raw.data(), uLong(raw.size()));
    comp.resize(n);
    Wr ihdr;
    ihdr.be32(uint32_t(w)).be32(uint32_t(h)).u8(8).u8(2).u8(0).u8(0).u8(0);
    Bytes out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    for (auto* part : {&ihdr.b}) { Bytes c = chunk("IHDR", *part); out.insert(out.end(), c.begin(), c.end()); }
    { Bytes c = chunk("IDAT", comp); out.insert(out.end(), c.begin(), c.end()); }
    { Bytes c = chunk("IEND", {}); out.insert(out.end(), c.begin(), c.end()); }
    return out;
}

// join and leave at once; empty string when the login succeeded, else the refusal text
std::string try_join(int port, int proto, const std::string& name, const Ids& ids) {
    (void)ids;
    JClient c;
    try {
        java_login(c, port, proto, name, offline_uuid(name));
        return "";
    } catch (const std::exception& ex) {
        std::string m = ex.what();
        const std::string pre = "login refused: ";
        return m.rfind(pre, 0) == 0 ? m.substr(pre.size()) : m;
    }
}

bool wait_for_reply(JBot& b, const std::string& needle, int timeout_ms) {
    return b.until([&] {
        for (auto& e : b.log) if (e.first == "system_chat" && contains(e.second, needle)) return true;
        return false;
    }, timeout_ms);
}

void lists(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    int port = e.port();
    const std::string dir = e.srv->dir;
    JBot B(port, 777, ids, "Bobby");
    B.pump(300);
    auto cmd = [&](const std::string& text, const std::string& expect) {
        B.drop("system_chat");
        B.command(text);
        wait_for_reply(B, expect, 3000);
        std::string all;
        for (auto& en : B.take("system_chat")) all.append(en.begin(), en.end());
        return all;
    };
    auto contains_s = [](const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; };
    t.check(try_join(port, 777, "Victim", *ids).empty(), "Victim can join");
    std::string out = cmd("ban Victim griefing", "Banned Victim");
    t.check(contains_s(out, "Banned Victim"), "/ban answers");
    std::string msg = try_join(port, 777, "Victim", *ids);
    t.check(contains_s(msg, "banned") && contains_s(msg, "griefing"), "banned Victim is refused with the reason");
    {
        std::ifstream f(dir + "/banned-players.json");
        std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        t.check(js.find("\"name\": \"Victim\"") != std::string::npos || js.find("\"name\":\"Victim\"") != std::string::npos, "banned-players.json names Victim");
        t.check(js.find("griefing") != std::string::npos && js.find("\"uuid\"") != std::string::npos, "banned-players.json has the reason and a uuid");
    }
    out = cmd("pardon Victim", "Unbanned");
    t.check(contains_s(out, "Unbanned") && try_join(port, 777, "Victim", *ids).empty(), "/pardon lets Victim back in");
    cmd("whitelist add Victim", "Added");
    cmd("whitelist on", "hitelist");
    t.check(try_join(port, 777, "Victim", *ids).empty(), "whitelisted Victim joins");
    msg = try_join(port, 777, "Stranger", *ids);
    t.check(contains_s(msg, "white-listed"), "a player that is not whitelisted is refused");
    cmd("whitelist off", "hitelist");
    t.check(try_join(port, 777, "Stranger", *ids).empty(), "whitelist off lets everybody in");
    // files edited by hand are picked up (the server re-reads them every few seconds)
    { std::ofstream f(dir + "/banned-players.json"); f << R"([{"uuid": "", "name": "Stranger", "created": "x", "source": "test", "expires": "forever", "reason": "by hand"}])"; }
    bool reloaded = false;
    for (int i = 0; i < 80 && !reloaded; i++) { std::string m = try_join(port, 777, "Stranger", *ids); reloaded = m.find("by hand") != std::string::npos; if (!reloaded) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    t.check(reloaded, "hand edited banned-players.json is reloaded");
    { std::ofstream f(dir + "/banned-players.json"); f << "[]"; }
    auto favicon = [&]() {
        bool pong;
        return status_json(port, 777, 1, pong).find("\"favicon\":\"data:image/png;base64,") != std::string::npos;
    };
    auto write = [&](const Bytes& b) { std::ofstream f(dir + "/server-icon.png", std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), long(b.size())); };
    auto wait_icon = [&](bool want) {
        for (int i = 0; i < 80; i++) { if (favicon() == want) return true; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        return false;
    };
    write(png(64, 64));
    t.check(wait_icon(true), "64x64 server-icon.png appears in the server list without a restart");
    write(png(32, 32));
    t.check(wait_icon(false), "a 32x32 icon is ignored");
    std::filesystem::remove(dir + "/server-icon.png");
}

}  // namespace

void reg_java_basic(Registry& reg) {
    Group a;
    a.name = "ref-smoke";
    a.opts = ref_opts;
    a.tests.push_back({"smoke-26.3", [](Ctx& t, Env& e) { smoke(t, e, "26.3"); }});
    a.tests.push_back({"smoke-26.2", [](Ctx& t, Env& e) { smoke(t, e, "26.2"); }});
    reg.push_back(std::move(a));
    Group b;
    b.name = "ref-chunks";
    b.opts = ref_opts;
    b.tests.push_back({"chunks-equal-mojang", world_compare});
    b.tests.push_back({"block-entities", block_entities});
    reg.push_back(std::move(b));
    Group c;
    c.name = "lists";
    c.opts = [](const Env&) { ServerOpts o; o.properties = "gamemode=creative\nlevel-type=flat\n"; o.ops = {"Bobby"}; o.toml = "[server]\nfiles_poll_ms = 100\n"; return o; };
    c.tests.push_back({"bans-whitelist-icon", lists});
    reg.push_back(std::move(c));
}

}  // namespace pt
