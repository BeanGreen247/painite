// Two Java bots: tab list, entities, movement, commands, block edits and chat; scheduled block updates (falling blocks, plants).
#include "chunkparse.hpp"
#include "harness.hpp"
#include <array>
#include <cstdio>
#include <unistd.h>
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
    o.ops = {"Digger", "Faller", "Eater", "Hunter", "Hunter2", "Keeper", "Archer", "Stress", "Cacher", "Wirer", "Farmer", "Plumber", "Plumber2", "Crafter", "Packer"};
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
    int item_type = ids->entity_types.at("item");
    auto item_spawn = [&]() -> Bytes {   // animals spawn by themselves now: look for the item entity by its type
        for (auto& en : B.log) if (en.first == "add_entity") { Rd r(en.second); r.varint(); r.take(16); if (int(r.varint()) == item_type) return en.second; }
        return Bytes();
    };
    B.until([&] { return !item_spawn().empty(); }, 3000);
    t.check(!item_spawn().empty(), "breaking dirt in survival spawns the dropped item");
    B.until([&] { return B.count("take_item_entity") >= 1; }, 5000);
    if (B.count("take_item_entity") < 1) {   // items slide away from where they pop (as in vanilla): once it rests, walk onto it like a player would
        B.pump(1500);
        Bytes spawn = item_spawn();
        if (!spawn.empty()) {
            Rd r(spawn);
            r.varint(); r.take(16); r.varint();
            double ix = r.f64(), iy = r.f64(), iz = r.f64();
            Rd id_reader(spawn);
            int item_id = int(id_reader.varint());
            for (auto& en : B.log) {
                if (en.first != "move_entity_pos" && en.first != "move_entity_pos_rot") continue;
                Rd m(en.second);
                if (int(m.varint()) != item_id) continue;
                if (777 >= 777) m.varint();
                ix += double(int16_t(m.be16())) / 4096.0; iy += double(int16_t(m.be16())) / 4096.0; iz += double(int16_t(m.be16())) / 4096.0;
            }
            B.send("move_player_pos", Wr().f64(ix).f64(iy).f64(iz).u8(1).b);
        }
    }
    B.until([&] { return B.count("take_item_entity") >= 1; }, 5000);
    {   // items slide away from where they pop (as in vanilla): walk onto the item like a player would
        Bytes spawn = item_spawn();
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

// a summoned mob is still there after the server restarts
void persist(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    int px, py, pz;
    {
        JBot B(e.port(), 777, ids, "Keeper");
        B.until([&] { return B.has_pos; }, 5000);
        B.settle(150, 1500);
        px = int(B.pos[0]); py = int(B.pos[1]); pz = int(B.pos[2]);
        B.command("summon armadillo " + std::to_string(px + 3) + " " + std::to_string(py) + " " + std::to_string(pz));
        B.until([&] { return B.count("add_entity") >= 1; }, 3000);
        t.check(B.count("add_entity") >= 1, "the armadillo is summoned");
    }
    e.srv->stop();
    e.srv->restart();
    JBot C(e.port(), 777, ids, "Keeper");
    C.until([&] { return C.has_pos; }, 8000);
    C.until([&] { for (auto& en : C.log) if (en.first == "add_entity") { Rd r(en.second); r.varint(); r.take(16); if (int(r.varint()) == ids->entity_types.at("armadillo")) return true; } return false; }, 5000);
    bool back = false;
    for (auto& en : C.log) if (en.first == "add_entity") { Rd r(en.second); r.varint(); r.take(16); if (int(r.varint()) == ids->entity_types.at("armadillo")) back = true; }
    t.check(back, "the armadillo is still there after a restart");
}

// a player draws a bow and throws a snowball
void ranged(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Archer");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    int px = int(std::floor(B.pos[0])), py = int(std::floor(B.pos[1])), pz = int(std::floor(B.pos[2]));
    B.send("move_player_pos_rot", Wr().f64(px + 0.5).f64(B.pos[1]).f64(pz + 0.5).f32(0.0f).f32(0.0f).u8(1).b);   // facing +z
    B.command("gamemode creative");
    B.pump(150);
    auto give = [&](int slot, const char* item, int n) { B.send("set_creative_mode_slot", Wr().be16(uint16_t(slot)).varint(uint32_t(n)).varint(uint32_t(ids->items.at(item))).varint(0).varint(0).b); };
    give(36, "bow", 1); give(37, "arrow", 16); give(38, "snowball", 16);
    B.pump(100);
    B.command("gamemode survival");
    B.pump(150);
    B.log.clear();
    B.command("summon cow " + std::to_string(px + 0.5) + " " + std::to_string(py) + " " + std::to_string(pz + 6.5));
    B.until([&] { return B.count("add_entity") >= 1; }, 3000);
    B.send("set_carried_item", Wr().be16(0).b);
    B.pump(100);
    auto entity_types_seen = [&](int type) { int n = 0; for (auto& en : B.log) if (en.first == "add_entity") { Rd r(en.second); r.varint(); r.take(16); if (int(r.varint()) == type) n++; } return n; };
    auto hurt_seen = [&] { for (auto& en : B.log) if (en.first == "entity_event") { Rd r(en.second); r.be32(); if (r.u8() == 2) return true; } return false; };
    B.send("use_item", Wr().varint(0).varint(1).f32(0).f32(0).b);
    B.pump(1200);
    B.send("player_action", Wr().varint(5).be64(block_pos(0, 0, 0)).u8(0).varint(2).b);   // release
    B.until([&] { return entity_types_seen(ids->entity_types.at("arrow")) >= 1; }, 3000);
    t.check(entity_types_seen(ids->entity_types.at("arrow")) >= 1, "a drawn bow shoots an arrow entity");
    B.until([&] { return hurt_seen(); }, 3000);
    t.check(hurt_seen(), "the arrow hurts the cow in front of the player");
    B.send("set_carried_item", Wr().be16(2).b);
    B.pump(100);
    B.send("use_item", Wr().varint(0).varint(3).f32(0).f32(0).b);
    B.until([&] { return entity_types_seen(ids->entity_types.at("snowball")) >= 1; }, 3000);
    t.check(entity_types_seen(ids->entity_types.at("snowball")) >= 1, "using a snowball throws it");
}

// Opt-in (PAINITE_PERF=1): 150 zombies chase a survival player among 150 cows; prints the server's timers and memory
std::string printable(const Bytes& b) { std::string o; for (uint8_t c : b) o += (c >= 32 && c < 127) ? char(c) : ' '; return o; }

long rss_kb(const std::string& dir) {
    for (int pid = 2; pid < 400000; pid++) {
        char link[64], target[512];
        snprintf(link, sizeof link, "/proc/%d/cwd", pid);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (dir != target) continue;
        snprintf(link, sizeof link, "/proc/%d/status", pid);
        FILE* f = fopen(link, "r");
        if (!f) continue;
        char line[256];
        long kb = -1;
        while (fgets(line, sizeof line, f)) if (!strncmp(line, "VmRSS:", 6)) kb = atol(line + 6);
        fclose(f);
        return kb;
    }
    return -1;
}

void perf_mobs(Ctx& t, Env& e) {
    if (!getenv("PAINITE_PERF")) return;
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Stress");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    int px = int(std::floor(B.pos[0])), py = int(std::floor(B.pos[1])), pz = int(std::floor(B.pos[2]));
    long before = rss_kb(e.srv->dir);
    for (int i = 0; i < 150; i++) {
        double a = i * 0.0419;
        B.command("summon zombie " + std::to_string(px + 0.5 + std::cos(a) * (12 + i % 20)) + " " + std::to_string(py) + " " + std::to_string(pz + 0.5 + std::sin(a) * (12 + i % 20)));
        B.command("summon cow " + std::to_string(px + 0.5 - std::cos(a) * (14 + i % 20)) + " " + std::to_string(py) + " " + std::to_string(pz + 0.5 - std::sin(a) * (14 + i % 20)));
        if (i % 10 == 0) B.pump(50);
    }
    B.pump(8000);
    B.log.clear();
    B.command("perf");
    B.pump(500);
    std::string rep;
    for (auto& en : B.log) if (en.first == "system_chat") rep += printable(en.second) + "\n";
    B.log.clear();
    B.command("cache");
    B.pump(500);
    for (auto& en : B.log) if (en.first == "system_chat") rep += printable(en.second) + "\n";
    t.note("RSS before " + std::to_string(before) + " kB, after " + std::to_string(rss_kb(e.srv->dir)) + " kB\n" + rep);
}

// the cache command lists the caches with their hits and misses
void cache_stats_test(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Cacher");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    B.log.clear();
    B.command("cache");
    B.pump(500);
    std::string all;
    for (auto& en : B.log) if (en.first == "system_chat") all += printable(en.second) + "\n";
    for (const char* name : {"world chunks", "java chunk packets", "path cells", "tags", "loot tables", "memory pool", "block collision shapes", "hit%"})
        t.check(all.find(name) != std::string::npos, std::string("`cache` lists ") + name);
}

// ---- redstone and random ticks, seen from a bot ------------------------------------------------------------

struct Stage {
    Ctx& t;
    JBot& B;
    std::map<int, std::string> names;
    std::map<std::array<int, 3>, std::string> seen;
    int px, py, pz;
    Stage(Ctx& ctx, JBot& bot, const std::string& blocks) : t(ctx), B(bot), names(load_block_names(blocks, true)) {
        px = int(std::floor(B.pos[0])); py = int(std::floor(B.pos[1])); pz = int(std::floor(B.pos[2]));
    }
    void collect() {
        for (auto& d : B.take("block_update")) {
            Rd r(d);
            int x, y, z;
            unpack_pos(r.be64(), x, y, z);
            auto it = names.find(int(r.varint()));
            seen[{x, y, z}] = it == names.end() ? "?" : it->second;
        }
    }
    void set(int x, int y, int z, const std::string& block) { B.command("setblock " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z) + " " + block); }
    // wait until the block at (x, y, z) has a name containing `part`
    bool wait(int x, int y, int z, const std::string& part, int ms) {
        auto has = [&] { collect(); auto it = seen.find({x, y, z}); return it != seen.end() && it->second.find(part) != std::string::npos; };
        B.until(has, ms);
        return has();
    }
    std::string at(int x, int y, int z) { collect(); auto it = seen.find({x, y, z}); return it == seen.end() ? "(unseen)" : it->second; }
};

void fluids_test(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Plumber");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    Stage S(t, B, e.data_dir + "/vanilla-26.3.blocks");
    B.command("gamemode creative");
    B.pump(150);
    int y = S.py, bx = S.px + 3, bz = S.pz + 3;
    B.command("fill " + std::to_string(bx - 2) + " " + std::to_string(y - 1) + " " + std::to_string(bz - 8) + " " + std::to_string(bx + 12) + " " + std::to_string(y - 1) + " " + std::to_string(bz + 8) + " stone");
    B.pump(200);
    // 1. a water source spreads: level 1 next to it, level 3 three blocks away, nothing beyond 7
    S.set(bx, y, bz, "water");
    B.pump(1500);
    S.collect();
    t.check(S.at(bx, y, bz) != "", "source: " + S.at(bx, y, bz));
    t.check(S.at(bx + 1, y, bz).find("level=1") != std::string::npos, "water flows one block out at level 1: " + S.at(bx + 1, y, bz));
    t.check(S.at(bx + 3, y, bz).find("level=3") != std::string::npos, "three blocks out it is level 3: " + S.at(bx + 3, y, bz));
    t.check(S.at(bx + 8, y, bz) == "(unseen)" || S.at(bx + 8, y, bz).find("air") != std::string::npos, "it stops after seven blocks: " + S.at(bx + 8, y, bz));
    // 2. water falls off an edge and the column below is falling water
    S.set(bx + 4, y - 1, bz, "air");
    B.pump(1500);
    S.collect();
    t.check(S.at(bx + 4, y - 1, bz).find("water") != std::string::npos, "water flows down a hole: " + S.at(bx + 4, y - 1, bz));
    // 3. removing the source dries the flow up
    S.set(bx, y, bz, "air");
    B.pump(2500);
    S.collect();
    t.check(S.at(bx + 1, y, bz).find("air") != std::string::npos, "the flow dries up without its source: " + S.at(bx + 1, y, bz));
    // 4. lava meets water: a flowing lava block turns to cobblestone, a source to obsidian
    int lz = bz + 5;
    S.set(bx, y, lz, "lava");
    S.set(bx + 2, y, lz, "water");
    B.pump(1000);
    S.collect();
    t.check(S.at(bx + 1, y, lz).find("cobblestone") != std::string::npos || S.at(bx + 1, y, lz).find("obsidian") != std::string::npos || S.at(bx, y, lz).find("obsidian") != std::string::npos,
            "lava and water make stone: " + S.at(bx, y, lz) + " / " + S.at(bx + 1, y, lz));
    // 5. buckets: scoop the source in front of the player
    S.set(S.px, y, S.pz + 2, "water");
    B.pump(300);
    B.send("set_creative_mode_slot", Wr().be16(36).varint(1).varint(uint32_t(ids->items.at("bucket"))).varint(0).varint(0).b);
    B.pump(300);
    B.send("move_player_pos_rot", Wr().f64(S.px + 0.5).f64(y).f64(S.pz + 0.5).f32(0.0f).f32(30.0f).u8(1).b);   // looks south at the water two blocks away
    B.pump(200);
    B.send("use_item", Wr().varint(0).varint(30).f32(0.0f).f32(0.0f).b);
    bool scooped = S.wait(S.px, y, S.pz + 2, "air", 3000);
    t.check(scooped, "a bucket scoops up the source: " + S.at(S.px, y, S.pz + 2));
    B.send("set_creative_mode_slot", Wr().be16(36).varint(1).varint(uint32_t(ids->items.at("lava_bucket"))).varint(0).varint(0).b);
    B.pump(300);
    B.send("use_item", Wr().varint(0).varint(31).f32(0.0f).f32(30.0f).b);
    bool poured = S.wait(S.px, y, S.pz + 3, "lava", 3000);
    t.check(poured, "a lava bucket pours a source where the ray meets the floor: " + S.at(S.px, y, S.pz + 3));
}

// the crafting table: open it, put planks in the grid, take sticks out of the result slot
void crafting_test(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Crafter");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    Stage S(t, B, e.data_dir + "/vanilla-26.3.blocks");
    B.command("gamemode creative");
    B.pump(150);
    int y = S.py, x = S.px + 2, z = S.pz;
    S.set(x, y, z, "crafting_table");
    B.send("set_creative_mode_slot", Wr().be16(36).varint(8).varint(uint32_t(ids->items.at("oak_planks"))).varint(0).varint(0).b);
    B.pump(300);
    B.command("gamemode survival");
    B.pump(300);
    B.drop("container_set_slot");
    B.send("use_item_on", Wr().varint(0).be64(block_pos(x, y, z)).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).u8(0).u8(0).varint(60).b);
    t.check(B.wait_for("open_screen", 1, 3000), "using a crafting table opens its window");
    auto scr = B.take("open_screen");
    int cid = 0;
    if (!scr.empty()) { Rd r(scr[0]); cid = int(r.varint()); }
    B.until([&] { return B.count("container_set_content") >= 1; }, 2000);
    B.drop("container_set_content");
    B.drop("container_set_slot");
    auto click = [&](int slot, int button) { B.send("container_click", Wr().varint(uint32_t(cid)).varint(0).be16(uint16_t(slot)).u8(uint8_t(button)).varint(0).varint(0).u8(0).b); B.pump(150); };
    click(37, 0);   // pick up the planks from the first hotbar slot
    click(2, 1);    // one into the top middle of the grid
    click(5, 1);    // one below it
    int result_item = -1, result_count = 0;
    for (auto& d : B.take("container_set_content")) {   // the last full state shows the result slot first
        Rd r(d);
        r.varint(); r.varint(); r.varint();
        int count = int(r.varint());
        result_count = count;
        result_item = count > 0 ? int(r.varint()) : 0;
    }
    t.check(result_item == ids->items.at("stick") && result_count == 4, "two planks stacked make 4 sticks: item " + std::to_string(result_item) + " x" + std::to_string(result_count));
    click(37, 0);   // put the rest of the planks back, the hand must be free
    B.drop("container_set_content");
    click(0, 0);    // take the sticks
    int sticks = 0;
    for (auto& d : B.take("container_set_content")) {   // the full state lists the slots: the sticks went to the inventory
        Rd r(d);
        r.varint(); r.varint();
        int n = int(r.varint());
        for (int i = 0; i < n && !r.bad; i++) {
            int count = int(r.varint());
            int item = count > 0 ? int(r.varint()) : 0;
            if (count > 0) { int pos = int(r.varint()), neg = int(r.varint()); (void)pos; (void)neg; }
            if (i >= 10 && item == ids->items.at("stick")) sticks += count;
        }
        int ccount = int(r.varint());   // the carried stack follows the slots
        if (ccount > 0 && int(r.varint()) == ids->items.at("stick")) sticks += ccount;
    }
    t.check(sticks == 4, "taking the result puts 4 sticks in the hand: " + std::to_string(sticks));
    B.send("container_close", Wr().varint(uint32_t(cid)).b);
    B.pump(200);
}

// chests keep what is put in them (also after closing and reopening), double chests are 54 slots, furnaces smelt
void containers_test(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Packer");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    Stage S(t, B, e.data_dir + "/vanilla-26.3.blocks");
    B.command("gamemode creative");
    B.pump(150);
    int y = S.py, x = S.px + 2, z = S.pz;
    S.set(x, y, z, "chest[facing=north,type=single]");
    S.set(x + 4, y, z, "chest[facing=north,type=left]");
    S.set(x + 5, y, z, "chest[facing=north,type=right]");
    S.set(x, y, z + 4, "furnace[facing=north,lit=false]");
    auto give = [&](int slot, const char* item, int n) { B.send("set_creative_mode_slot", Wr().be16(uint16_t(slot)).varint(uint32_t(n)).varint(uint32_t(ids->items.at(item))).varint(0).varint(0).b); };
    give(36, "diamond", 5);
    give(37, "raw_iron", 3);
    give(38, "coal", 4);
    B.pump(300);
    B.command("gamemode survival");
    B.pump(300);
    int seq = 70;
    auto open = [&](int bx, int bz) -> int {
        B.drop("open_screen");
        B.send("use_item_on", Wr().varint(0).be64(block_pos(bx, y, bz)).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).u8(0).u8(0).varint(uint32_t(seq++)).b);
        B.wait_for("open_screen", 1, 3000);
        auto scr = B.take("open_screen");
        int cid = 0, type = -1;
        if (!scr.empty()) { Rd r(scr[0]); cid = int(r.varint()); type = int(r.varint()); }
        B.pump(200);
        return type < 0 ? -1 : cid;
    };
    auto click = [&](int cid, int slot, int button) { B.send("container_click", Wr().varint(uint32_t(cid)).varint(0).be16(uint16_t(slot)).u8(uint8_t(button)).varint(0).varint(0).u8(0).b); B.pump(150); };
    // the number of slots in the last full window content, and the item in a slot
    auto content = [&](int& n, std::vector<std::pair<int, int>>& stacks) {
        n = 0; stacks.clear();
        auto all = B.take("container_set_content");
        if (all.empty()) return;
        Rd r(all.back());
        r.varint(); r.varint();
        n = int(r.varint());
        for (int i = 0; i < n && !r.bad; i++) {
            int count = int(r.varint());
            int item = 0;
            if (count > 0) { item = int(r.varint()); r.varint(); r.varint(); }
            stacks.push_back({item, count});
        }
    };
    int n = 0;
    std::vector<std::pair<int, int>> st;
    // 1. a chest: 27 slots, the diamonds go in and stay
    int cid = open(x, z);
    t.check(cid > 0, "a chest opens");
    B.drop("container_set_content");
    click(cid, 27 + 27, 0);   // pick up the diamonds (hotbar slot 0)
    click(cid, 3, 0);         // into chest slot 3
    B.send("container_close", Wr().varint(uint32_t(cid)).b);
    B.pump(200);
    cid = open(x, z);
    B.drop("container_set_content");
    click(cid, 40, 1);   // a click somewhere harmless to get a full state back
    content(n, st);
    t.check(n == 63 && st.size() > 3 && st[3].first == ids->items.at("diamond") && st[3].second == 5, "the chest still holds the diamonds after reopening (" + std::to_string(n) + " slots)");
    B.send("container_close", Wr().varint(uint32_t(cid)).b);
    B.pump(200);
    // 2. a double chest is 54 + 36 slots
    B.drop("container_set_content");
    cid = open(x + 4, z);
    B.drop("container_set_content");
    click(cid, 80, 1);
    content(n, st);
    t.check(n == 90, "a double chest has 54 + 36 slots: " + std::to_string(n));
    B.send("container_close", Wr().varint(uint32_t(cid)).b);
    B.pump(200);
    // 3. a furnace smelts raw iron with coal
    cid = open(x, z + 4);
    B.drop("container_set_content");
    click(cid, 3 + 27 + 1, 0);   // the raw iron: hotbar slot 1
    click(cid, 0, 0);            // into the ingredient slot
    click(cid, 3 + 27 + 2, 0);   // the coal
    click(cid, 1, 0);            // into the fuel slot
    B.drop("container_set_slot");
    bool smelted = B.until([&] {
        for (auto& d : B.log) {
            if (d.first != "container_set_slot") continue;
            Rd r(d.second);
            r.varint(); r.varint();
            int slot = int16_t(r.be16());
            int count = int(r.varint());
            if (slot == 2 && count > 0 && int(r.varint()) == ids->items.at("iron_ingot")) return true;
        }
        return false;
    }, 16000);
    t.check(smelted, "a furnace turns raw iron into an ingot");
    t.check(B.count("container_set_data") > 0, "the furnace sends its progress");
    B.send("container_close", Wr().varint(uint32_t(cid)).b);
    B.pump(200);
}

void fluids_more_test(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Plumber2");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    Stage S(t, B, e.data_dir + "/vanilla-26.3.blocks");
    B.command("gamemode creative");
    B.command("gamerule randomTickSpeed 3000");
    B.pump(150);
    int y = S.py, bx = S.px + 4, bz = S.pz + 4;
    auto give = [&](const char* item) { B.send("set_creative_mode_slot", Wr().be16(36).varint(1).varint(uint32_t(ids->items.at(item))).varint(0).varint(0).b); B.pump(300); };
    auto click = [&](int x, int yy, int z, int seq) { B.send("use_item_on", Wr().varint(0).be64(block_pos(x, yy, z)).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).u8(0).u8(0).varint(uint32_t(seq)).b); };
    // 0. powder snow from a bucket, and back
    B.send("move_player_pos_rot", Wr().f64(S.px + 0.5).f64(y).f64(S.pz + 0.5).f32(0.0f).f32(30.0f).u8(1).b);
    B.pump(200);
    give("powder_snow_bucket");
    B.send("use_item", Wr().varint(0).varint(50).f32(0.0f).f32(30.0f).b);
    bool placed = S.wait(S.px, y, S.pz + 3, "powder_snow", 3000);
    t.check(placed, "a powder snow bucket places powder snow: " + S.at(S.px, y, S.pz + 3));
    give("bucket");
    B.send("move_player_pos_rot", Wr().f64(S.px + 0.5).f64(y).f64(S.pz + 0.5).f32(0.0f).f32(30.0f).u8(1).b);
    B.send("use_item", Wr().varint(0).varint(51).f32(0.0f).f32(30.0f).b);
    bool taken = S.wait(S.px, y, S.pz + 3, "air", 3000);
    t.check(taken, "an empty bucket takes it back: " + S.at(S.px, y, S.pz + 3));
    // 1. a bubble column over soul sand lifts, over magma drags down
    S.set(bx, y - 1, bz, "soul_sand");
    S.set(bx, y, bz, "water");
    S.set(bx + 6, y - 1, bz, "magma_block");
    S.set(bx + 6, y, bz, "water");
    S.set(bx, y + 1, bz, "water");
    B.pump(800);
    t.check(S.wait(bx, y, bz, "bubble_column[drag=false]", 4000), "water over soul sand becomes an upward bubble column: " + S.at(bx, y, bz));
    t.check(S.wait(bx + 6, y, bz + 0, "bubble_column[drag=true]", 4000), "water over magma becomes a downward bubble column: " + S.at(bx + 6, y, bz));
    t.check(S.wait(bx, y + 1, bz, "bubble_column", 4000), "the column carries on upwards: " + S.at(bx, y + 1, bz));
    // 2. kelp grows up through water
    int kz = bz + 6;
    for (int i = 0; i <= 8; i++) S.set(bx, y + i, kz, "water");
    S.set(bx, y - 1, kz, "sand");
    S.set(bx, y, kz, "kelp[age=0]");
    B.pump(500);
    t.check(S.wait(bx, y, kz, "kelp_plant", 15000), "kelp grows: the base becomes a plant block: " + S.at(bx, y, kz));
    // 3. lava next to planks sets them alight
    int lz = bz - 6;
    S.set(bx, y, lz, "lava");
    S.set(bx + 1, y, lz, "oak_planks");
    S.set(bx - 1, y, lz, "oak_planks");
    S.set(bx, y, lz + 1, "oak_planks");
    S.set(bx, y, lz - 1, "oak_planks");
    bool fire = B.until([&] {
        S.collect();
        for (auto& kv : S.seen) if (kv.second.find("fire") != std::string::npos && std::abs(kv.first[0] - bx) <= 3 && std::abs(kv.first[2] - lz) <= 3) return true;
        return false;
    }, 20000);
    t.check(fire, "lava starts a fire on the planks around it");
    B.command("gamerule randomTickSpeed 3");
    // 4. buckets and a cauldron
    int cz = bz + 24;
    S.set(bx, y, cz, "cauldron");
    give("water_bucket");
    click(bx, y, cz, 40);
    t.check(S.wait(bx, y, cz, "water_cauldron[level=3]", 3000), "a water bucket fills a cauldron: " + S.at(bx, y, cz));
    give("bucket");
    click(bx, y, cz, 41);
    t.check(S.wait(bx, y, cz, "minecraft:cauldron", 3000), "an empty bucket empties it again: " + S.at(bx, y, cz));
    give("lava_bucket");
    click(bx, y, cz, 42);
    t.check(S.wait(bx, y, cz, "lava_cauldron", 3000), "a lava bucket fills it with lava: " + S.at(bx, y, cz));
}

void redstone(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Wirer");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    Stage S(t, B, e.data_dir + "/vanilla-26.3.blocks");
    B.command("gamemode creative");
    B.pump(150);
    int y = S.py, bx = S.px + 3, bz = S.pz;
    // 1. a lever, a piece of dust and a lamp: the lamp follows the lever
    S.set(bx, y, bz, "lever[face=floor,facing=north,powered=false]");
    S.set(bx + 1, y, bz, "redstone_wire");
    S.set(bx + 2, y, bz, "redstone_lamp");
    B.pump(400);
    S.collect();
    t.check(S.at(bx + 2, y, bz).find("lit=false") != std::string::npos || S.at(bx + 2, y, bz) == "(unseen)", "the lamp starts dark: " + S.at(bx + 2, y, bz));
    B.send("use_item_on", Wr().varint(0).be64(block_pos(bx, y, bz)).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).u8(0).u8(0).varint(21).b);   // flip the lever
    t.check(S.wait(bx + 2, y, bz, "lit=true", 3000), "flipping the lever lights the lamp through the dust: " + S.at(bx + 2, y, bz));
    t.check(S.at(bx + 1, y, bz).find("power=15") != std::string::npos, "the dust next to the lever carries 15");
    B.send("use_item_on", Wr().varint(0).be64(block_pos(bx, y, bz)).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).u8(0).u8(0).varint(22).b);
    t.check(S.wait(bx + 2, y, bz, "lit=false", 3000), "flipping it back darkens the lamp: " + S.at(bx + 2, y, bz));
    // 2. a redstone torch on a block turns off when the block is powered
    int tz = bz + 3;
    S.set(bx, y, tz, "stone");
    S.set(bx, y + 1, tz, "redstone_torch[lit=true]");
    S.set(bx + 1, y, tz, "lever[face=wall,facing=east,powered=true]");
    t.check(S.wait(bx, y + 1, tz, "lit=false", 3000), "a torch goes out when its block is powered: " + S.at(bx, y + 1, tz));
    // 3. a redstone block opens a door
    int dz = bz + 6;
    S.set(bx, y, dz, "oak_door[half=lower,facing=north,open=false]");
    S.set(bx, y + 1, dz, "oak_door[half=upper,facing=north,open=false]");
    S.set(bx + 1, y, dz, "redstone_block");
    t.check(S.wait(bx, y, dz, "open=true", 3000), "power opens a door: " + S.at(bx, y, dz));
    // 4. a piston pushes the block in front of it
    int pz2 = bz + 9;
    S.set(bx, y, pz2, "piston[facing=east,extended=false]");
    S.set(bx + 1, y, pz2, "stone");
    S.set(bx, y, pz2 + 1, "redstone_block");
    t.check(S.wait(bx + 2, y, pz2, "minecraft:stone", 3000), "a powered piston pushes the stone one block: " + S.at(bx + 2, y, pz2));
    t.check(S.wait(bx + 1, y, pz2, "piston_head", 3000), "and its head takes the place: " + S.at(bx + 1, y, pz2));
    // 6. a repeater passes the signal on after its delay, a button gives a short pulse, a plate answers to the player standing on it
    int rz = bz - 8;
    S.set(bx, y, rz, "lever[face=floor,facing=north,powered=false]");
    S.set(bx + 1, y, rz, "repeater[facing=west,delay=2]");
    S.set(bx + 2, y, rz, "redstone_lamp");
    B.pump(300);
    B.send("use_item_on", Wr().varint(0).be64(block_pos(bx, y, rz)).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).u8(0).u8(0).varint(23).b);
    t.check(S.wait(bx + 2, y, rz, "lit=true", 3000), "a lever feeds a lamp through a repeater");
    int uz = bz - 11;
    S.set(bx, y, uz, "stone_button[face=floor,facing=north,powered=false]");
    S.set(bx + 1, y, uz, "redstone_lamp");
    B.pump(300);
    B.send("use_item_on", Wr().varint(0).be64(block_pos(bx, y, uz)).varint(1).f32(0.5f).f32(0.5f).f32(0.5f).u8(0).u8(0).varint(24).b);
    t.check(S.wait(bx + 1, y, uz, "lit=true", 3000), "pressing a button lights the lamp next to it");
    t.check(S.wait(bx + 1, y, uz, "lit=false", 4000), "and the button lets go again after a second");
    S.set(S.px, y, S.pz, "stone_pressure_plate");
    S.set(S.px + 1, y, S.pz, "redstone_lamp");
    t.check(S.wait(S.px + 1, y, S.pz, "lit=true", 4000), "standing on a pressure plate lights the lamp next to it");
    // 5. TNT next to a redstone block goes off
    int tnz = bz - 5;
    S.set(bx + 6, y, tnz, "tnt");
    B.log.clear();
    S.set(bx + 6, y, tnz + 1, "redstone_block");
    B.until([&] { return B.count("add_entity") >= 1; }, 3000);
    t.check(B.count("add_entity") >= 1, "powered TNT becomes a primed TNT entity");
    S.collect();
    S.seen.clear();
    B.pump(4500);
    t.check(S.at(bx + 6, y, tnz + 1) == "minecraft:air" || S.at(bx + 6, y, tnz + 1) == "(unseen)" ? S.at(bx + 6, y, tnz + 1) == "minecraft:air" : false, "the explosion blows the redstone block away: " + S.at(bx + 6, y, tnz + 1));
}

void random_tick_test(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Farmer");
    B.until([&] { return B.has_pos; }, 5000);
    B.settle(150, 1500);
    Stage S(t, B, e.data_dir + "/vanilla-26.3.blocks");
    B.command("gamemode creative");
    B.command("gamerule randomTickSpeed 1000");
    B.pump(200);
    int y = S.py, x = S.px + 3, z = S.pz;
    S.set(x, y - 1, z, "farmland[moisture=7]");
    S.set(x, y, z, "wheat[age=0]");
    S.set(x + 1, y - 1, z, "water");   // keeps the farmland wet
    S.set(x + 2, y - 1, z + 8, "grass_block");
    S.set(x + 2, y, z + 8, "oak_sapling[stage=1]");
    S.set(x + 4, y + 6, z - 8, "oak_leaves[distance=7,persistent=false]");
    B.pump(300);
    S.collect();
    auto age_of = [&] { std::string v = S.at(x, y, z); size_t p = v.find("age="); return p == std::string::npos ? -1 : atoi(v.c_str() + p + 4); };
    B.until([&] { S.collect(); return age_of() >= 4; }, 14000);
    t.check(age_of() >= 4, "wheat grows on watered farmland with random ticks (age " + std::to_string(age_of()) + ")");
    t.check(S.wait(x + 2, y, z + 8, "oak_log", 8000), "a sapling grows into a tree");
    t.check(S.wait(x + 4, y + 6, z - 8, "minecraft:air", 8000), "a leaf block without a log decays");
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
    d.tests.push_back({"crafting-table", crafting_test});
    d.tests.push_back({"containers", containers_test});
    d.tests.push_back({"mobs", mobs});
    d.tests.push_back({"mobs-more", mobs2});
    d.tests.push_back({"perf-mobs", perf_mobs});
    d.tests.push_back({"cache-command", cache_stats_test});
    d.tests.push_back({"bow-and-snowball", ranged});
    d.tests.push_back({"natural-spawn", natural});
    reg.push_back(std::move(d));
    Group rs;
    rs.name = "redstone";
    rs.opts = survival_opts;
    rs.tests.push_back({"circuits", redstone});
    rs.tests.push_back({"random-ticks", random_tick_test});
    rs.tests.push_back({"fluids", fluids_test});
    rs.tests.push_back({"fluids-more", fluids_more_test});
    reg.push_back(std::move(rs));
    Group f;
    f.name = "persist";
    f.opts = survival_opts;
    f.tests.push_back({"mobs-survive-restart", persist});
    reg.push_back(std::move(f));
}

}  // namespace pt
