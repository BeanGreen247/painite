// Bedrock Edition: join handshake and chunks, sub chunk requests, chat, dimensions, and Java + Bedrock players together.
#include "bclient.hpp"
#include "chunkparse.hpp"
#include "harness.hpp"
#include "lossyproxy.hpp"
#include <cmath>
#include <fstream>

namespace pt {

namespace {

ServerOpts bed_opts(const Env&) {
    ServerOpts o;
    o.properties = "gamemode=creative\nlevel-type=flat\n";
    o.ops = {"Alice", ".BedDim", ".Bedi"};
    return o;
}

bool has_text(const std::vector<Bytes>& v, const std::string& s) {
    for (auto& d : v) if (contains(d, s)) return true;
    return false;
}

void bedrock_smoke(Ctx& t, Env& e) {
    BedBot b(e.port(), "BedSmoke");
    std::map<int, int> seen;
    bool asked = false, chat_sent = false, chat_ok = false;
    Bytes sub;
    b.on_packet = [&](const BPacket& p) {
        seen[p.id]++;
        if (p.id == 58 && !asked) {
            asked = true;
            Rd r(p.d);
            uint64_t zx = r.varuint(), zz = r.varuint();
            int cx = int(zx >> 1) ^ -int(zx & 1), cz = int(zz >> 1) ^ -int(zz & 1);
            Wr w;
            w.varuint(0).varuint(24);
            for (int k = 0; k < 24; k++) { w.u8(0); w.u8(uint8_t(k)); w.u8(0); }
            w.le32(uint32_t(cx)).le32(uint32_t(int32_t(-4))).le32(uint32_t(cz));
            b.send(175, w.b);
        } else if (p.id == 174) sub = p.d;
        else if (p.id == 9 && contains(p.d, "hello from bedrock")) chat_ok = true;
        if (b.spawned && !chat_sent) { chat_sent = true; b.chat("hello from bedrock"); }
    };
    b.until([&] { return b.chunks > 0 && b.spawned && !sub.empty() && chat_ok; }, 40000);
    t.check(seen.count(11) > 0, "StartGame replay received");
    t.check(seen[63] >= 1, "player list received (patched with our name)");
    t.check(b.chunks >= 40, "LevelChunk packets: " + std::to_string(b.chunks));
    t.check(sub.size() > 1000, "SubChunk response: " + std::to_string(sub.size()) + " bytes");
    if (!sub.empty()) {
        Rd r(sub);
        r.u8();
        r.varuint();
        r.skip(12);
        uint64_t n = r.varuint();
        int good = 0, air = 0, bad = 0;
        for (uint64_t k = 0; k < n && !r.bad; k++) {
            r.skip(3);
            int res = r.u8(), pres = r.u8();
            if (pres) {
                uint64_t ln = r.varuint();
                Bytes data = r.take(size_t(ln));
                if (res == 1 && data.size() > 1 && data[0] == 9 && (data[1] == 1 || data[1] == 2)) good++; else bad++;
            } else air++;
            int ht = r.u8();
            if (ht == 1) { r.u8(); for (int row = 0; row < 16; row++) { uint64_t ln = r.varuint(); r.skip(size_t(ln)); } }
            else r.u8();
            r.u8();   // render height map type
            r.u8();
            r.u8();
        }
        t.check(!r.bad && r.left() == 0 && bad == 0 && good > 0, "SubChunk entries parse: " + std::to_string(good) + " with blocks, " + std::to_string(air) + " empty, " + std::to_string(bad) + " bad, " + std::to_string(r.left()) + " bytes left");
    }
    t.check(b.spawned, "PlayStatus player_spawn");
    t.check(chat_ok, "chat echoed back to the Bedrock client");
}

void bedrock_dimensions(Ctx& t, Env& e) {
    BedBot b(e.port(), "BedDim");
    t.check(b.until([&] { return b.spawned; }, 30000), "joined");
    struct Step { const char* name; int dim; };
    for (Step s : {Step{"the_nether", 1}, Step{"the_end", 2}, Step{"overworld", 0}}) {
        int before = b.chunk_dims[s.dim];
        b.command(std::string("/execute in minecraft:") + s.name + " run tp @s 0 80 0");
        b.until([&] { return b.dim_changes[s.dim] >= 1 && b.chunk_dims[s.dim] - before >= 10; }, 15000);
        t.check(b.dim_changes[s.dim] >= 1, std::string("ChangeDimension to ") + s.name);
        t.check(b.chunk_dims[s.dim] - before >= 10, std::string("LevelChunk packets for ") + s.name + ": " + std::to_string(b.chunk_dims[s.dim] - before));
    }
}

// the Bedrock runtime ids for every Java state id
std::vector<int32_t> bedrock_table(const std::string& data_dir) {
    std::ifstream f(data_dir + "/blocks-bedrock.bin", std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() < 8) return {};
    uint32_t n;
    memcpy(&n, &raw[4], 4);
    std::vector<int32_t> t(n);
    memcpy(t.data(), &raw[8], size_t(n) * 4);
    return t;
}

void crossplay(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot J(e.port(), 777, ids, "Alice");
    J.until([&] { return J.has_pos; }, 5000);
    std::string bname = "Bed" + std::to_string(unix_ms() % 1000);
    BedBot B(e.port(), bname);
    B.until([&] { return B.spawned; }, 30000);
    B.until([&] { return B.count(63) >= 1 && B.count(12) >= 1; }, 5000);
    t.check(B.spawned && B.has_pos, "Bedrock bot spawned");
    J.until([&] { return J.count("add_entity") >= 1 && J.count("player_info_update") >= 1; }, 5000);
    bool tab = false;
    for (auto& en : J.log) if (en.first == "player_info_update" && contains(en.second, "." + bname)) tab = true;
    t.check(tab, "Java: tab list has the Bedrock player");
    t.check(J.count("add_entity") >= 1, "Java: add_entity for the Bedrock player");
    auto pl_list = B.take(63);
    t.check(has_text(pl_list, "Alice"), "Bedrock: PlayerList has Alice");
    bool skin = false;
    for (auto& p : pl_list) if (contains(p, "Alice") && contains(p, "painite-") && contains(p, "geometry.humanoid.custom")) skin = true;
    t.check(skin, "Bedrock: Alice has a classic default skin (64x64, standard geometry)");
    auto ap = B.take(12);
    t.check(!ap.empty() && contains(ap[0], "Alice"), "Bedrock: AddPlayer for Alice");
    // Java moves, Bedrock follows
    double jx = J.pos[0], jy = J.pos[1], jz = J.pos[2];
    for (int k = 1; k <= 5; k++) { J.send("move_player_pos_rot", Wr().f64(jx + k * 0.25).f64(jy).f64(jz).f32(45.0f).f32(5.0f).u8(1).b); J.pump(60); }
    auto last_move = [&](float& x, float& y) {
        bool any = false;
        for (auto& p : B.log) if (p.id == 19) { Rd r(p.d); r.varuint(); x = r.lef32(); y = r.lef32(); any = true; }
        return any;
    };
    float mx = 0, my = 0;
    B.until([&] { return last_move(mx, my) && std::fabs(mx - (jx + 1.25)) < 0.05; }, 4000);
    bool got_move = last_move(mx, my);
    t.check(got_move, "Bedrock: MovePlayer packets for Alice");
    if (got_move) t.check(std::fabs(mx - (jx + 1.25)) < 0.05 && std::fabs(my - (jy + 1.62)) < 0.05, "Bedrock sees Alice at the Java position plus eye height");
    // Bedrock moves, Java follows
    float bx = B.pos[0], by = B.pos[1], bz = B.pos[2];
    J.log.clear();
    for (int k = 1; k <= 5; k++) { B.auth_input(bx + k * 0.2f, by, bz, 90.0f, 10.0f, uint64_t(k)); B.pump(60); }
    J.until([&] { return J.count("move_entity_pos_rot") + J.count("move_entity_pos") + J.count("move_entity_rot") >= 1; }, 4000);
    t.check(J.count("move_entity_pos_rot") + J.count("move_entity_pos") + J.count("move_entity_rot") >= 1, "Java: movement packets for the Bedrock player");
    // block edits across editions
    B.take(21);
    const char* platform = "emerald_block";
    J.command(std::string("setblock ~ ~3 ~ ") + platform);
    B.until([&] { return B.count(21) >= 1; }, 3000);
    auto ub = B.take(21);
    t.check(!ub.empty(), "Bedrock: UpdateBlock after a Java /setblock");
    if (!ub.empty()) {
        Rd r(ub[0]);
        r.varuint(); r.varuint(); r.varuint();
        int32_t rid = int32_t(uint32_t(r.varuint()));
        auto table = bedrock_table(e.data_dir);
        auto names = load_block_names(e.data_dir + "/vanilla-26.3.blocks", false);
        std::string match;
        for (size_t k = 0; k < table.size(); k++) if (table[k] == rid && names.count(int(k))) { match = names[int(k)]; if (match == std::string("minecraft:") + platform) break; }
        t.check(match == std::string("minecraft:") + platform, "UpdateBlock runtime id " + std::to_string(rid) + " maps back to " + match);
    }
    // chat both ways
    J.drop("system_chat");
    B.chat("hi from bedrock");
    J.until([&] { for (auto& en : J.log) if (en.first == "system_chat" && contains(en.second, "hi from bedrock")) return true; return false; }, 3000);
    bool jc = false;
    for (auto& en : J.log) if (en.first == "system_chat" && contains(en.second, "hi from bedrock")) jc = true;
    t.check(jc, "Java sees the Bedrock player's chat");
    J.send("chat", Wr().str("hi from java").be64(uint64_t(unix_ms())).be64(0).u8(0).varint(0).zeros(3).u8(0).b);
    B.until([&] { for (auto& p : B.log) if (p.id == 9 && contains(p.d, "hi from java")) return true; return false; }, 3000);
    bool bc = false;
    for (auto& p : B.log) if (p.id == 9 && contains(p.d, "hi from java")) bc = true;
    t.check(bc, "Bedrock sees the Java player's chat");
    // leave
    J.drop("remove_entities");
    B.c.send_frame(Bytes{0x15}, 3);   // RakNet disconnect notification
    J.wait_for("remove_entities", 1, 8000);
    t.check(J.count("remove_entities") >= 1, "Java: remove_entities when the Bedrock player leaves");
}

// Bedrock over a link that loses 10% of the datagrams and delays the rest: RakNet resends must still deliver the join.
void bedrock_lossy(Ctx& t, Env& e) {
    LossyProxy proxy(0, e.port(), 10.0, 30);
    BedBot b(proxy.port, "BedLossy");
    b.until([&] { return b.spawned && b.chunks >= 40; }, 60000);
    t.check(b.spawned, "spawned through the lossy link (" + std::to_string(proxy.dropped()) + " datagrams dropped)");
    t.check(b.chunks >= 40, "chunks arrived despite the loss: " + std::to_string(b.chunks));
    t.check(proxy.dropped() > 5, "the proxy did drop datagrams");
}

}  // namespace

void reg_bedrock(Registry& reg) {
    Group a;
    a.name = "bedrock";
    a.opts = bed_opts;
    a.tests.push_back({"smoke", bedrock_smoke});
    a.tests.push_back({"dimensions", bedrock_dimensions});
    reg.push_back(std::move(a));
    Group b;
    b.name = "crossplay";
    b.opts = bed_opts;
    b.tests.push_back({"java-and-bedrock", crossplay});
    reg.push_back(std::move(b));
    Group c;
    c.name = "bedrock-lossy";
    c.opts = bed_opts;
    c.tests.push_back({"lossy-link", bedrock_lossy});
    reg.push_back(std::move(c));
}

}  // namespace pt
