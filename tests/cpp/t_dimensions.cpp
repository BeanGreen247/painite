// Dimensions over Java: /execute in <dim> run tp, respawn packet, chunks of the new dimension, /locate, nether portal, End portal.
#include "chunkparse.hpp"
#include "harness.hpp"
#include <cmath>

namespace pt {

namespace {

ServerOpts dim_opts(const Env&) {
    ServerOpts o;
    o.properties = "gamemode=creative\nlevel-seed=777\n";
    o.ops = {"Dimmy"};
    return o;
}

bool near(const double* p, double x, double y, double z, double tol = 0.6) { return std::fabs(p[0] - x) < tol && std::fabs(p[1] - y) < tol && std::fabs(p[2] - z) < tol; }

void java_dimensions(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    JBot B(e.port(), 777, ids, "Dimmy");
    B.until([&] { return B.chunks > 10; }, 8000);
    t.check(B.chunks > 10, "overworld chunks arrive (" + std::to_string(B.chunks) + ")");
    B.drop("respawn");
    struct Step { const char* dim; int y; const char* label; int sections; };
    for (Step s : {Step{"the_nether", 80, "the Nether", 16}, Step{"the_end", 70, "the End", 16}, Step{"overworld", 100, "the overworld", 24}}) {
        int before = 0;   // chunks are counted per world (a respawn resets the count)
        B.command(std::string("execute in minecraft:") + s.dim + " run tp @s 0 " + std::to_string(s.y) + " 0");
        B.until([&] { return B.count("respawn") >= 1 && B.chunks > 10 && near(B.pos, 0, s.y, 0, 0.01); }, 8000);
        t.check(B.take("respawn").size() == 1, std::string("respawn packet when entering ") + s.label);
        t.check(B.chunks - before > 10, std::string("chunks of ") + s.label + " arrive (" + std::to_string(B.chunks - before) + ")");
        t.check(std::fabs(B.pos[1] - s.y) < 0.01, "teleported to y " + std::to_string(s.y) + " in " + s.label);
        ChunkData c = parse_chunk(B.last_chunk);
        t.check(int(c.sections.size()) == s.sections, std::string("a chunk of ") + s.label + " has " + std::to_string(c.sections.size()) + " sections (expected " + std::to_string(s.sections) + ")");
        t.check(parse_tail(c.rest).exact, std::string("the light data of a ") + s.label + " chunk is well formed");
    }
    B.drop("system_chat");
    for (const char* kind : {"stronghold", "village"}) {
        B.command(std::string("locate structure ") + kind);
        bool got = B.until([&] { for (auto& en : B.log) if (en.first == "system_chat" && (contains(en.second, "nearest") || contains(en.second, "no generated structures"))) return true; return false; }, 3000);
        t.check(got, std::string("/locate answers for a ") + kind);
        B.drop("system_chat");
    }
    auto& items = ids->items;
    auto cmd = [&](const std::string& text, int ms = 120) { B.command(text); B.pump(ms); };
    auto tp_to = [&](double x, double y, double z) {
        char b[96];
        snprintf(b, sizeof b, "tp @s %g %g %g", x, y, z);
        B.command(b);
        B.until([&] { return near(B.pos, x, y, z, 0.01); }, 6000);
    };
    // a nether portal: an obsidian frame in the air, lit with flint and steel, then walked into and back out of
    tp_to(300, 150, 298);
    B.settle(150, 1500);
    B.drop("respawn");
    cmd("fill 296 149 296 306 149 306 stone");
    cmd("fill 300 150 300 303 150 300 obsidian");
    cmd("fill 300 154 300 303 154 300 obsidian");
    cmd("fill 300 151 300 300 153 300 obsidian");
    cmd("fill 303 151 300 303 153 300 obsidian");
    cmd("fill 301 151 300 302 153 300 air");
    tp_to(301.5, 150, 298.5);
    B.drop("block_update");
    B.send("set_creative_mode_slot", Wr().be16(36).varint(1).varint(uint32_t(items.at("flint_and_steel"))).varint(0).varint(0).b);
    B.pump(100);
    B.send("use_item_on", Wr().varint(0).be64(block_pos(301, 150, 300)).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).u8(0).u8(0).varint(1).b);
    B.until([&] { return B.count("block_update") >= 6; }, 3000);
    B.settle(100, 600);
    t.check(B.count("block_update") >= 6, "lighting the frame changes its 6 inside blocks (" + std::to_string(B.count("block_update")) + " block updates)");
    B.send("move_player_pos", Wr().f64(301.5).f64(151.0).f64(300.5).u8(1).b);
    B.until([&] { return B.count("respawn") >= 1; }, 15000);
    t.check(B.take("respawn").size() == 1, "walking into the portal enters the Nether");
    B.until([&] { return std::fabs(B.pos[0] - 301.5 / 8) < 24 && std::fabs(B.pos[2] - 300.5 / 8) < 24; }, 3000);
    t.check(std::fabs(B.pos[0] - 301.5 / 8) < 24 && std::fabs(B.pos[2] - 300.5 / 8) < 24, "arrived near the scaled position");
    double nx = B.pos[0], ny = B.pos[1], nz = B.pos[2];
    B.send("move_player_pos", Wr().f64(nx).f64(ny).f64(nz + 4.0).u8(1).b);
    B.pump(1400);
    B.send("move_player_pos", Wr().f64(nx).f64(ny).f64(nz).u8(1).b);
    B.until([&] { return B.count("respawn") >= 1; }, 15000);
    t.check(B.take("respawn").size() == 1, "walking into the Nether side leads back to the overworld");
    B.until([&] { return std::fabs(B.pos[0] - 301.5) < 40 && std::fabs(B.pos[2] - 300.5) < 40; }, 3000);
    t.check(std::fabs(B.pos[0] - 301.5) < 40 && std::fabs(B.pos[2] - 300.5) < 40, "arrived near the original portal");
    // an End portal: twelve frames in the air, eyes put in with the held eye of ender, then in and out of the End
    tp_to(400, 150, 395);
    B.settle(150, 1500);
    B.drop("respawn");
    cmd("fill 394 149 394 406 149 406 stone");
    for (int i = 0; i < 3; i++) {
        cmd("setblock " + std::to_string(399 + i) + " 150 398 end_portal_frame[eye=false,facing=south]", 40);
        cmd("setblock " + std::to_string(399 + i) + " 150 402 end_portal_frame[eye=false,facing=north]", 40);
        cmd("setblock 398 150 " + std::to_string(399 + i) + " end_portal_frame[eye=false,facing=east]", 40);
        cmd("setblock 402 150 " + std::to_string(399 + i) + " end_portal_frame[eye=false,facing=west]", 40);
    }
    tp_to(400.5, 150, 395.5);
    B.drop("block_update");
    B.send("set_creative_mode_slot", Wr().be16(36).varint(1).varint(uint32_t(items.at("ender_eye"))).varint(0).varint(0).b);
    B.pump(100);
    std::vector<std::pair<int, int>> frames;
    for (int i = 0; i < 3; i++) { frames.push_back({399 + i, 398}); frames.push_back({399 + i, 402}); frames.push_back({398, 399 + i}); frames.push_back({402, 399 + i}); }
    for (auto& f : frames) {
        B.send("use_item_on", Wr().varint(0).be64(block_pos(f.first, 150, f.second)).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).u8(0).u8(0).varint(1).b);
        B.pump(30);
    }
    B.until([&] { return B.count("block_update") >= 21; }, 3000);
    t.check(B.count("block_update") >= 21, "twelve eyes and nine portal blocks (" + std::to_string(B.count("block_update")) + " block updates)");
    B.send("move_player_pos", Wr().f64(400.5).f64(150.0).f64(400.5).u8(1).b);
    B.until([&] { return B.count("respawn") >= 1; }, 15000);
    t.check(B.take("respawn").size() == 1, "the End portal leads to the End");
    B.until([&] { return near(B.pos, 100.5, 49, 0.5, 1.0); }, 3000);
    t.check(near(B.pos, 100.5, 49, 0.5, 1.0), "arrived on the obsidian platform");
    B.pump(2200);   // the portal cooldown
    B.command("execute in minecraft:the_end run tp @s 1.5 66 0.5");
    B.until([&] { return B.count("respawn") >= 1; }, 15000);
    t.check(B.take("respawn").size() == 1, "the exit fountain leads back to the overworld");
}

}  // namespace

void reg_dimensions(Registry& reg) {
    Group g;
    g.name = "dimensions";
    g.opts = dim_opts;
    g.tests.push_back({"java", java_dimensions});
    reg.push_back(std::move(g));
}

}  // namespace pt
