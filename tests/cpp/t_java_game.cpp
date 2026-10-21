// Two Java bots: tab list, entities, movement, commands, block edits and chat; scheduled block updates (falling blocks, plants).
#include "chunkparse.hpp"
#include "harness.hpp"
#include <array>
#include <cmath>
#include <map>

namespace pt {

namespace {

ServerOpts game_opts(const Env&) {
    ServerOpts o;
    o.properties = "gamemode=creative\nlevel-type=flat\n";
    o.ops = {"Bobby", "Bobbyb", "Bobbyc", "Rob"};
    return o;
}

// total of the x deltas (in blocks) carried by the movement packets A has queued for entity-of-B
double movement_total(JBot& a, int protocol) {
    double tot = 0;
    for (auto& e : a.log) {
        if (e.first != "move_entity_pos_rot" && e.first != "move_entity_pos") continue;
        Rd r(e.second);
        r.varint();
        if (protocol >= 777) r.varint();
        tot += double(int16_t(r.be16())) / 4096.0;
    }
    return tot;
}

void game(Ctx& t, Env& e, const std::string& version, const std::string& an, const std::string& bn, const char* platform) {
    auto ids = e.ids(version);
    int proto = e.protocol(version);
    int port = e.port();
    JBot A(port, proto, ids, an);
    A.until([&] { return A.has_pos; }, 5000);
    JBot B(port, proto, ids, bn);
    B.until([&] { return B.has_pos; }, 5000);
    Bytes ub = B.uuid;
    A.wait_for("player_info_update", 1, 3000);
    A.wait_for("add_entity", 1, 3000);
    bool tab = false;
    for (auto& en : A.log) if (en.first == "player_info_update" && std::search(en.second.begin(), en.second.end(), ub.begin(), ub.end()) != en.second.end()) tab = true;
    t.check(tab, an + " sees " + bn + " in the tab list");
    {
        Bytes spawn;
        for (auto& en : A.log) if (en.first == "add_entity") { spawn = en.second; break; }
        t.check(!spawn.empty(), an + " gets add_entity for " + bn);
        if (!spawn.empty()) {
            Rd r(spawn);
            r.varint();
            Bytes u = r.take(16);
            t.check(u == ub, "add_entity carries " + bn + "'s uuid");
            int type = int(r.varint());
            t.check(type == ids->entity_types.at("player"), "entity type is player (" + std::to_string(type) + ")");
            double x = r.f64(), y = r.f64(), z = r.f64();
            (void)y;
            t.check(std::fabs(x - B.pos[0]) < 0.01 && std::fabs(z - B.pos[2]) < 0.01, "spawned at " + bn + "'s position");
        }
    }
    // movement
    double bx = B.pos[0], by = B.pos[1], bz = B.pos[2];
    for (int i = 1; i <= 5; i++) {
        B.send("move_player_pos_rot", Wr().f64(bx + i * 0.2).f64(by).f64(bz).f32(90.0f).f32(10.0f).u8(1).b);
        B.pump(55);
    }
    A.until([&] { return std::fabs(movement_total(A, proto) - 1.0) < 0.01; }, 3000);
    double tot = movement_total(A, proto);
    t.check(std::fabs(tot - 1.0) < 0.01, "summed movement deltas = " + std::to_string(tot) + " blocks (expected 1.0)");
    A.log.clear();
    // operator commands: a platform under Bobby, seen by Alicia
    B.drop("system_chat");
    B.command(std::string("fill ~ ~-1 ~ ~6 ~-1 ~2 ") + platform);
    bool filled = B.until([&] { for (auto& en : B.log) if (en.first == "system_chat" && contains(en.second, "filled")) return true; return false; }, 3000);
    t.check(filled, "/fill answers for an operator");
    A.until([&] { return A.count("block_update") >= 15; }, 3000);
    t.check(A.count("block_update") >= 15, an + " gets block updates for the platform (" + std::to_string(A.count("block_update")) + ")");
    A.drop("block_update");
    // block placement: stone from the creative inventory onto the platform
    auto& items = ids->items;
    B.send("set_creative_mode_slot", Wr().be16(36).varint(64).varint(uint32_t(items.at("stone"))).varint(0).varint(0).b);
    B.send("set_carried_item", Wr().be16(0).b);
    double cur_x = bx + 1.0;
    B.send("move_player_pos", Wr().f64(cur_x).f64(by).f64(bz).u8(1).b);
    B.pump(150);
    int tx = int(std::floor(cur_x)) + 2, tz = int(std::floor(bz)), ty = int(std::floor(by)) - 1;
    B.drop("block_changed_ack");
    B.send("use_item_on", Wr().varint(0).be64(block_pos(tx, ty, tz)).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).u8(0).u8(0).varint(7).b);
    B.wait_for("block_changed_ack", 1, 3000);
    auto ack = B.take("block_changed_ack");
    t.check(ack.size() == 1 && Rd(ack[0]).varint() == 7, bn + " gets block_changed_ack for sequence 7");
    uint64_t want = block_pos(tx, ty + 1, tz);
    auto find_update = [&](uint64_t pos, int* state) {
        for (auto& en : A.log)
            if (en.first == "block_update") { Rd r(en.second); if (r.be64() == pos) { *state = int(r.varint()); return true; } }
        return false;
    };
    int st = -1;
    A.until([&] { return find_update(want, &st); }, 3000);
    t.check(find_update(want, &st), an + " gets block_update for the placed block");
    if (st < 0) {
        std::string seen;
        int ex, ey, ez;
        unpack_pos(want, ex, ey, ez);
        seen = "expected " + std::to_string(ex) + "," + std::to_string(ey) + "," + std::to_string(ez) + "; received:";
        for (auto& en : A.log) if (en.first == "block_update") { Rd r(en.second); int x, y, z; unpack_pos(r.be64(), x, y, z); seen += " (" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ")=" + std::to_string(r.varint()); }
        seen += " | B got:";
        for (auto& en : B.log) if (en.first == "block_update") { Rd r(en.second); int x, y, z; unpack_pos(r.be64(), x, y, z); seen += " (" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ")=" + std::to_string(r.varint()); }
        t.note(seen);
    }
    t.check(st == 1, "placed block state is stone (state " + std::to_string(st) + ")");
    // break it again
    A.drop("block_update");
    B.send("player_action", Wr().varint(0).be64(block_pos(tx, ty + 1, tz)).u8(1).varint(8).b);
    A.until([&] { st = -1; return find_update(want, &st) && st == 0; }, 3000);
    t.check(st == 0, an + " gets block_update (air) when " + bn + " breaks the block");
    // swing
    A.drop(proto >= 777 ? "swing_animation" : "animate");
    if (ids->has("play/s/punch")) B.send("punch");
    else B.send("swing", Wr().varint(0).b);
    A.wait_for(proto >= 777 ? "swing_animation" : "animate", 1, 3000);
    t.check(A.count(proto >= 777 ? "swing_animation" : "animate") == 1, an + " sees " + bn + " swing");
    // commands
    B.drop("system_chat");
    B.command("list");
    t.check(B.until([&] { for (auto& en : B.log) if (en.first == "system_chat" && contains(en.second, "players online")) return true; return false; }, 3000), "/list answers");
    A.drop("system_chat");
    A.command("tp 1 100 1");
    t.check(A.until([&] { for (auto& en : A.log) if (en.first == "system_chat" && contains(en.second, "permission")) return true; return false; }, 3000), "/tp refused for a non-operator");
    B.command("tp " + bn + " ~ ~10 ~");
    B.pump(300);
    // chat reaches both
    A.drop("system_chat");
    B.send("chat", Wr().str("hi there").be64(uint64_t(unix_ms())).be64(0).u8(0).varint(0).zeros(3).u8(0).b);
    t.check(A.until([&] { for (auto& en : A.log) if (en.first == "system_chat" && contains(en.second, "hi there")) return true; return false; }, 3000), "chat reaches " + an);
    // leave
    A.drop("remove_entities");
    B.c.close();
    A.wait_for("remove_entities", 1, 5000);
    t.check(A.count("remove_entities") >= 1, an + " gets remove_entities when " + bn + " leaves");
    A.wait_for("player_info_remove", 1, 2000);
    bool rem = false;
    for (auto& en : A.log) if (en.first == "player_info_remove" && std::search(en.second.begin(), en.second.end(), ub.begin(), ub.end()) != en.second.end()) rem = true;
    t.check(rem, an + " gets player_info_remove");
}

// ---- scheduled block updates ---------------------------------------------------------------------------

void region(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    auto names = load_block_names(e.data_dir + "/vanilla-26.3.blocks", true);
    JBot B(e.port(), 777, ids, "Rob");
    JBot W(e.port(), 777, ids, "Watcher");
    B.until([&] { return B.has_pos; }, 5000);
    W.until([&] { return W.has_pos; }, 5000);
    int x = int(std::floor(B.pos[0])) + 4, z = int(std::floor(B.pos[2])) + 1, y0 = int(std::floor(B.pos[1])) - 1;
    std::map<std::array<int, 3>, std::string> seen;
    auto collect = [&]() {
        for (auto& d : W.take("block_update")) {
            Rd r(d);
            int px, py, pz;
            unpack_pos(r.be64(), px, py, pz);
            auto it = names.find(int(r.varint()));
            seen[{px, py, pz}] = it == names.end() ? "?" : it->second.substr(it->second.rfind(':') + 1);
        }
    };
    auto cmd = [&](const std::string& text) { B.command(text); B.pump(100); W.pump(100); };
    auto wait_block = [&](int py, const std::string& want, int ms) {
        W.until([&] { collect(); auto it = seen.find({x, py, z}); return it != seen.end() && it->second == want; }, ms);
        collect();
        auto it = seen.find({x, py, z});
        return it != seen.end() ? it->second : std::string("none");
    };
    auto fmt = [](int a, int b, int c, int d, int f, int g) { return std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c) + " " + std::to_string(d) + " " + std::to_string(f) + " " + std::to_string(g); };
    cmd("fill " + fmt(x - 1, y0, z - 1, x + 1, y0, z + 1) + " stone");   // own floor
    cmd("fill " + fmt(x, y0 + 1, z, x, y0 + 12, z) + " air");
    W.settle(150, 1000);
    collect();
    seen.clear();
    auto at = [&](int py) { return std::to_string(x) + " " + std::to_string(py) + " " + std::to_string(z); };
    // 1. sand dropped in the air lands on the ground
    cmd("setblock " + at(y0 + 8) + " sand");
    std::string landed = wait_block(y0 + 1, "sand", 4000);
    std::string left = wait_block(y0 + 8, "air", 500);
    t.check(left == "air", "sand leaves its starting block: " + left);
    t.check(landed == "sand", "sand lands on the ground at y=" + std::to_string(y0 + 1) + ": " + landed);
    // 2. a flower on the sand pops when the sand goes
    cmd("setblock " + at(y0 + 2) + " dandelion");
    seen.clear();
    cmd("setblock " + at(y0 + 1) + " air");
    std::string fl = wait_block(y0 + 2, "air", 3000);
    t.check(fl == "air", "the flower is removed with its ground: " + fl);
    // 3. tall grass: the top goes when the bottom is removed
    cmd("setblock " + at(y0 + 1) + " tall_grass[half=lower]");
    cmd("setblock " + at(y0 + 2) + " tall_grass[half=upper]");
    W.settle(150, 1500);
    seen.clear();
    cmd("setblock " + at(y0 + 1) + " air");
    std::string tg = wait_block(y0 + 2, "air", 3000);
    t.check(tg == "air", "the upper half of a tall plant goes with the lower one: " + tg);
    // 4. gravel stack falls together
    seen.clear();
    cmd("setblock " + at(y0 + 5) + " gravel");
    cmd("setblock " + at(y0 + 6) + " gravel");
    std::string g1 = wait_block(y0 + 1, "gravel", 5000);
    std::string g2 = wait_block(y0 + 2, "gravel", 3000);
    t.check(g1 == "gravel" && g2 == "gravel", "two gravel blocks fall and stack: " + g1 + " " + g2);
    cmd("fill " + fmt(x, y0 + 1, z, x, y0 + 12, z) + " air");
}

// ---- survival: breaking a block drops its item, the player picks it up ------------------------------------

ServerOpts survival_opts(const Env&) {
    ServerOpts o;
    o.properties = "gamemode=survival\nlevel-type=flat\n";
    o.ops = {"Digger", "Faller", "Eater", "Hunter", "Hunter2"};
    return o;
}

void survival(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Digger");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    int x = int(std::floor(B.pos[0])) + 1, y = int(std::floor(B.pos[1])), z = int(std::floor(B.pos[2]));
    B.send("move_player_pos", Wr().f64(x - 0.5).f64(B.pos[1]).f64(z + 0.5).u8(1).b);   // stand in the middle of the block next to the dirt, so the drop is always in reach
    B.pump(150);
    B.command("setblock " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z) + " dirt");
    B.pump(200);
    B.drop("add_entity");
    B.send("player_action", Wr().varint(0).be64(block_pos(x, y, z)).u8(1).varint(1).b);
    B.send("player_action", Wr().varint(2).be64(block_pos(x, y, z)).u8(1).varint(2).b);
    B.until([&] { return B.count("add_entity") >= 1; }, 3000);
    t.check(B.count("add_entity") >= 1, "breaking dirt in survival spawns the dropped item");
    B.until([&] { return B.count("take_item_entity") >= 1; }, 5000);
    if (B.count("take_item_entity") < 1) {   // items slide away from where they pop (as in vanilla): once it rests, walk onto it like a player would
        B.pump(1500);
        Bytes spawn;
        for (auto& en : B.log) if (en.first == "add_entity") { spawn = en.second; break; }
        if (!spawn.empty()) {
            Rd r(spawn);
            r.varint(); r.take(16); r.varint();
            double ix = r.f64(), iy = r.f64(), iz = r.f64();
            for (auto& en : B.log) {
                if (en.first != "move_entity_pos" && en.first != "move_entity_pos_rot") continue;
                Rd m(en.second);
                m.varint();
                if (777 >= 777) m.varint();
                ix += double(int16_t(m.be16())) / 4096.0; iy += double(int16_t(m.be16())) / 4096.0; iz += double(int16_t(m.be16())) / 4096.0;
            }
            B.send("move_player_pos", Wr().f64(ix).f64(iy).f64(iz).u8(1).b);
        }
    }
    B.until([&] { return B.count("take_item_entity") >= 1; }, 5000);
    {   // items slide away from where they pop (as in vanilla): walk onto the item like a player would
        Bytes spawn;
        for (auto& en : B.log) if (en.first == "add_entity") { spawn = en.second; break; }
        if (!spawn.empty()) {
            Rd r(spawn);
            r.varint(); r.take(16); r.varint();
            double ix = r.f64(), iy = r.f64(), iz = r.f64();
            B.send("move_player_pos", Wr().f64(ix).f64(iy).f64(iz).u8(1).b);
        }
    }
    t.check(B.count("take_item_entity") >= 1, "the player picks the item up (take_item_entity)");
}

// ---- health: fall damage, /kill, the death screen and the respawn --------------------------------------------

float last_health(JBot& b) {
    float h = -1;
    for (auto& en : b.log) if (en.first == "set_health") { Rd r(en.second); h = r.f32(); }
    return h;
}

void vitals(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Faller");
    B.until([&] { return B.has_pos; }, 5000);
    B.until([&] { return B.count("set_health") >= 1; }, 3000);
    t.check(last_health(B) == 20.0f, "a new player starts with 20 health");
    B.settle(150, 1500);
    double x = B.pos[0], y = B.pos[1], z = B.pos[2];
    // a fall of 20 blocks onto the ground costs ceil(20 - 3) = 17 health
    B.log.clear();
    for (double dy = 20; dy > 0; dy -= 1.0) { B.send("move_player_pos", Wr().f64(x).f64(y + dy).f64(z).u8(0).b); B.pump(5); }
    B.send("move_player_pos", Wr().f64(x).f64(y).f64(z).u8(1).b);
    B.until([&] { return last_health(B) >= 0 && last_health(B) < 20.0f; }, 3000);
    t.check(last_health(B) == 3.0f, "a 20 block fall leaves 3 health (" + std::to_string(last_health(B)) + ")");
    t.check(B.count("hurt_animation") >= 1, "the fall plays the hurt animation");
    // /kill: death screen, then respawn gives the health back
    B.log.clear();
    B.command("kill Faller");
    B.until([&] { return B.count("player_combat_kill") >= 1; }, 3000);
    t.check(B.count("player_combat_kill") >= 1, "dying sends the death screen (player_combat_kill)");
    B.log.clear();
    B.send("client_command", Wr().varint(0).b);
    B.until([&] { return B.count("respawn") >= 1 && last_health(B) == 20.0f; }, 3000);
    t.check(B.count("respawn") >= 1, "pressing Respawn answers with a respawn packet");
    t.check(last_health(B) == 20.0f, "the respawned player has 20 health");
}

void eating(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Eater");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    B.command("gamemode creative");
    B.pump(200);
    B.send("set_creative_mode_slot", Wr().be16(36).varint(1).varint(uint32_t(ids->items.at("golden_apple"))).varint(0).varint(0).b);
    B.send("set_carried_item", Wr().be16(0).b);
    B.pump(100);
    B.command("gamemode survival");
    B.pump(200);
    B.log.clear();
    B.send("use_item", Wr().varint(0).varint(1).f32(0).f32(0).b);
    B.until([&] { return B.count("set_health") >= 1; }, 4000);
    float sat = -1;
    for (auto& en : B.log) if (en.first == "set_health") { Rd r(en.second); r.f32(); r.varint(); sat = r.f32(); }
    t.check(sat > 10.0f, "eating a golden apple raises the saturation (" + std::to_string(sat) + ")");
}

// a summoned zombie walks to the player, takes sword hits and dies
void mobs(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Hunter");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    B.command("gamemode creative");
    B.pump(200);
    B.send("set_creative_mode_slot", Wr().be16(36).varint(1).varint(uint32_t(ids->items.at("netherite_sword"))).varint(0).varint(0).b);
    B.send("set_carried_item", Wr().be16(0).b);
    B.pump(100);
    B.command("gamemode survival");
    B.pump(150);
    B.log.clear();
    B.command("summon zombie " + std::to_string(int(B.pos[0]) + 4) + " " + std::to_string(int(B.pos[1])) + " " + std::to_string(int(B.pos[2])));
    B.until([&] { return B.count("add_entity") >= 1; }, 3000);
    int zombie = -1;
    for (auto& en : B.log) if (en.first == "add_entity") {
        Rd r(en.second);
        zombie = int(r.varint());
        r.take(16);
        t.check(int(r.varint()) == ids->entity_types.at("zombie"), "a zombie is added to the world");
        break;
    }
    t.check(zombie >= 0, "/summon zombie spawns an entity");
    B.until([&] { return B.count("move_entity_pos") + B.count("move_entity_pos_rot") >= 3; }, 4000);
    t.check(B.count("move_entity_pos") + B.count("move_entity_pos_rot") >= 3, "the zombie moves");
    B.log.clear();
    auto zombie_died = [&] { for (auto& en : B.log) if (en.first == "entity_event") { Rd r(en.second); r.be32(); if (r.u8() == 3) return true; } return false; };
    for (int i = 0; i < 8 && !zombie_died(); i++) {   // hit, wait out the hurt time, hit again
        B.send("interact", Wr().varint(uint32_t(zombie)).varint(1).u8(0).b);
        B.pump(700);
    }
    bool hurt = false, died = false;
    for (auto& en : B.log) if (en.first == "entity_event") { Rd r(en.second); r.be32(); int st = r.u8(); if (st == 2) hurt = true; if (st == 3) died = true; }
    t.check(hurt, "hitting the zombie plays its hurt animation");
    t.check(died, "three netherite sword hits kill the zombie (death animation)");
    B.until([&] { return B.count("remove_entities") >= 1; }, 3000);
    t.check(B.count("remove_entities") >= 1, "the dead zombie is removed");
}

// animals appear around a player on grass by themselves
void natural(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Walker");
    B.until([&] { return B.has_pos; }, 5000);
    B.log.clear();
    B.until([&] { return B.count("add_entity") >= 1; }, 20000);
    t.check(B.count("add_entity") >= 1, "animals spawn around a player (" + std::to_string(B.count("add_entity")) + " seen)");
}

// creeper blast, skeleton arrows, zombies burning in the sun
void mobs2(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Hunter2");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    int px = int(B.pos[0]), py = int(B.pos[1]), pz = int(B.pos[2]);
    auto at = [&](int dx, int dz) { return std::to_string(px + dx) + " " + std::to_string(py) + " " + std::to_string(pz + dz); };
    // zombies burn in daylight: a hurt animation without anybody hitting it (the player is in creative, so it is no target)
    B.command("gamemode creative");
    B.pump(150);
    B.log.clear();
    B.command("summon zombie " + at(10, 0));
    B.until([&] { for (auto& en : B.log) if (en.first == "entity_event") { Rd r(en.second); r.be32(); if (r.u8() == 2) return true; } return false; }, 8000);
    bool burnt = false;
    for (auto& en : B.log) if (en.first == "entity_event") { Rd r(en.second); r.be32(); if (r.u8() == 2) burnt = true; }
    t.check(burnt, "a zombie in the sun catches fire and gets hurt");
    // a skeleton shoots: the arrow shows up as an entity and the skeleton holds a bow
    B.command("gamemode survival");
    B.pump(150);
    B.log.clear();
    B.command("summon skeleton " + at(0, 9));
    B.until([&] { for (auto& en : B.log) if (en.first == "add_entity") { Rd r(en.second); r.varint(); r.take(16); if (int(r.varint()) == ids->entity_types.at("arrow")) return true; } return false; }, 8000);
    bool arrow = false;
    for (auto& en : B.log) if (en.first == "add_entity") { Rd r(en.second); r.varint(); r.take(16); if (int(r.varint()) == ids->entity_types.at("arrow")) arrow = true; }
    t.check(arrow, "a skeleton shoots arrows");
    t.check(B.count("set_equipment") >= 1, "the skeleton is shown holding a bow");
    // a creeper next to a survival player blows up: damage and blocks gone
    B.command("kill @e");
    B.log.clear();
    B.command("summon creeper " + at(-3, 0));
    B.until([&] { return B.count("player_combat_kill") >= 1 || (B.count("set_health") >= 1 && B.count("block_update") >= 1); }, 6000);
    t.check(B.count("block_update") >= 1, "the creeper explosion removes blocks");
    float lowest = 20;
    for (auto& en : B.log) if (en.first == "set_health") { Rd r(en.second); lowest = std::min(lowest, r.f32()); }
    t.check(lowest < 20.0f || B.count("player_combat_kill") >= 1, "the creeper explosion hurts the player");
}

}  // namespace

void reg_java_game(Registry& reg) {
    Group a;
    a.name = "game-26.3";
    a.opts = game_opts;
    a.tests.push_back({"two-bots", [](Ctx& t, Env& e) { game(t, e, "26.3", "Alicia", "Bobby", "gold_block"); }});
    reg.push_back(std::move(a));
    Group b;
    b.name = "game-26.2";
    b.opts = game_opts;
    b.tests.push_back({"two-bots", [](Ctx& t, Env& e) { game(t, e, "26.2", "Aliciab", "Bobbyb", "iron_block"); }});
    reg.push_back(std::move(b));
    Group b1;
    b1.name = "game-26.1";
    b1.opts = game_opts;
    b1.tests.push_back({"two-bots", [](Ctx& t, Env& e) { game(t, e, "26.1", "Aliciac", "Bobbyc", "copper_block"); }});
    reg.push_back(std::move(b1));
    Group c;
    c.name = "region";
    c.opts = game_opts;
    c.tests.push_back({"block-updates", region});
    reg.push_back(std::move(c));
    Group d;
    d.name = "survival";
    d.opts = survival_opts;
    d.tests.push_back({"dig-and-pickup", survival});
    d.tests.push_back({"health-and-death", vitals});
    d.tests.push_back({"eating", eating});
    d.tests.push_back({"mobs", mobs});
    d.tests.push_back({"mobs-more", mobs2});
    d.tests.push_back({"natural-spawn", natural});
    reg.push_back(std::move(d));
}

}  // namespace pt
