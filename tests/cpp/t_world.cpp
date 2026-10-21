// Edit a world with Painite, restart, then start Mojang's own server on the saved world: vanilla must read what Painite wrote.
#include "chunkparse.hpp"
#include "harness.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace pt {

namespace {

std::string find_java(const Env& e) {
    if (!std::filesystem::exists(e.test_world + "/jre")) return "java";
    for (auto& entry : std::filesystem::directory_iterator(e.test_world + "/jre")) {
        std::string j = entry.path().string() + "/bin/java";
        if (std::filesystem::exists(j)) return j;
    }
    return "java";
}

// joins as `name` and collects chunks until `want` arrived, `secs` passed, or `done` says so
std::map<std::pair<int, int>, ChunkData> collect_chunks(int port, const std::string& name, std::shared_ptr<Ids> ids, int secs, size_t want, JBot** keep = nullptr,
                                                       const std::function<bool(const std::map<std::pair<int, int>, ChunkData>&)>& done = nullptr) {
    auto bot = new JBot(port, 777, ids, name);
    std::map<std::pair<int, int>, ChunkData> got;
    bot->on_chunk = [&](const Bytes& d) { ChunkData c = parse_chunk(d); got[{c.x, c.z}] = std::move(c); };
    bot->until([&] { return got.size() >= want || (done && done(got)); }, secs * 1000);
    if (keep) *keep = bot; else delete bot;
    return got;
}

void roundtrip(Ctx& t, Env& e) {
    if (!std::filesystem::exists(e.test_world + "/server.jar")) { t.note("skipped: no Mojang server jar next to the reference world (rebuild it with tools/make_test_world.py)"); return; }
    auto ids = e.ids("26.3");
    auto names = load_block_names(e.data_dir + "/vanilla-26.3.blocks", true);
    auto at = [&](const ChunkData& c, int x, int y, int z) {
        size_t si = size_t((y + 64) >> 4);
        if (si >= c.sections.size()) return std::string("?");
        auto it = names.find(c.sections[si].blocks[size_t(((y + 64) & 15) * 256 + (z & 15) * 16 + (x & 15))]);
        return it == names.end() ? std::string("?") : it->second;
    };
    // Painite session: spawn, fill a gold cube and a column of mixed blocks, then stop (saves)
    JBot* ed = nullptr;
    auto got0 = collect_chunks(e.port(), "Editor", ids, 15, 60, &ed);
    std::unique_ptr<JBot> editor(ed);
    t.check(got0.size() >= 9, "Painite sent " + std::to_string(got0.size()) + " chunks to the editor");
    std::vector<int> xs, zs;
    for (auto& kv : got0) { xs.push_back(kv.first.first); zs.push_back(kv.first.second); }
    std::sort(xs.begin(), xs.end());
    std::sort(zs.begin(), zs.end());
    if (xs.empty()) return;
    int cx = xs[xs.size() / 2], cz = zs[zs.size() / 2];
    int bx = cx * 16 + 3, bz = cz * 16 + 5;
    auto s = [](int v) { return std::to_string(v); };
    std::vector<std::string> cmds = {"setblock " + s(bx) + " 120 " + s(bz) + " gold_block", "setblock " + s(bx + 1) + " 120 " + s(bz) + " oak_log[axis=x]",
                                     "setblock " + s(bx + 2) + " 120 " + s(bz) + " oak_stairs[facing=east,half=top]",
                                     "fill " + s(bx) + " 121 " + s(bz) + " " + s(bx + 3) + " 123 " + s(bz + 3) + " diamond_block", "setblock " + s(bx) + " 100 " + s(bz) + " water"};
    for (auto& c : cmds) { editor->command(c); editor->pump(120); }
    editor->settle(200, 1500);
    editor.reset();
    e.srv->stop();   // saves the world
    e.srv->restart();
    // Painite again, fresh process: do the edits survive a restart?
    auto got1 = collect_chunks(e.port(), "Checker", ids, 15, 60, nullptr, [&](auto& m) { return m.count({cx, cz}) > 0; });
    auto it = got1.find({cx, cz});
    t.check(it != got1.end(), "edited chunk is sent after the restart");
    if (it != got1.end()) {
        const ChunkData& ch = it->second;
        t.check(at(ch, bx, 120, bz) == "minecraft:gold_block", "gold block survived the restart: " + at(ch, bx, 120, bz));
        t.check(at(ch, bx + 1, 120, bz) == "minecraft:oak_log[axis=x]", "log axis survived: " + at(ch, bx + 1, 120, bz));
        std::string st = at(ch, bx + 2, 120, bz);
        t.check(st.find("oak_stairs[") != std::string::npos && st.find("facing=east") != std::string::npos && st.find("half=top") != std::string::npos, "stairs properties survived: " + st);
        t.check(at(ch, bx + 3, 123, bz + 3) == "minecraft:diamond_block", "fill survived");
        t.check(at(ch, bx, 100, bz).rfind("minecraft:water", 0) == 0, "water survived: " + at(ch, bx, 100, bz));
    }
    // now Mojang's server reads the world Painite saved
    e.srv->stop();
    const std::string run = e.srv->dir;
    int vport = Server::free_port();
    std::string painite_props;
    { std::ifstream f(run + "/server.properties"); painite_props.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
    { std::ofstream f(run + "/server.properties"); f << "server-port=" << vport << "\nonline-mode=false\nlevel-name=world\nwhite-list=false\nnetwork-compression-threshold=-1\nview-distance=8\n"; }
    { std::ofstream f(run + "/eula.txt"); f << "eula=true\n"; }
    Proc vanilla({find_java(e), "-Xmx1G", "-XX:TieredStopAtLevel=1", "-XX:+UseSerialGC", "-Xshare:auto", "-jar", e.test_world + "/server.jar", "nogui"}, run, run + "/vanilla.log");
    bool up = false;
    for (int i = 0; i < 1800 && !up; i++) {
        up = vanilla.log().find("Done") != std::string::npos;
        if (!up && !vanilla.alive()) break;
        if (!up) usleep(100000);
    }
    t.check(up, "Mojang's server started on the saved world");
    if (up) {
        auto got2 = collect_chunks(vport, "VanillaCheck", ids, 40, 100000, nullptr, [&](auto& m) { return m.count({cx, cz}) > 0; });
        auto v = got2.find({cx, cz});
        t.check(v != got2.end(), "vanilla serves the edited chunk");
        if (v != got2.end()) {
            const ChunkData& ch = v->second;
            t.check(at(ch, bx, 120, bz) == "minecraft:gold_block", "vanilla sees the gold block: " + at(ch, bx, 120, bz));
            t.check(at(ch, bx + 1, 120, bz) == "minecraft:oak_log[axis=x]", "vanilla sees the oak log axis: " + at(ch, bx + 1, 120, bz));
            std::string st = at(ch, bx + 2, 120, bz);
            t.check(st.find("facing=east") != std::string::npos && st.find("half=top") != std::string::npos, "vanilla sees the stairs: " + st);
            t.check(at(ch, bx + 3, 123, bz + 3) == "minecraft:diamond_block", "vanilla sees the filled diamond blocks");
        }
    }
    vanilla.terminate(60000);
    std::string vlog = vanilla.log();
    t.check(vlog.find("Exception") == std::string::npos && vlog.find("Failed") == std::string::npos, "Mojang's log mentions no exception or failure");
    { std::ofstream f(run + "/server.properties"); f << painite_props; }
    e.srv->restart();   // leave the fixture running for the harness check
}


// The layouts Painite writes (play Login, Login Success, ...) must still parse the vanilla samples recorded in data/vanilla-*.bin.
// The ported Mojang generator serves chunks: bedrock floor, stone and a surface of its own
void vanilla_terrain(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    auto names = load_block_names(e.data_dir + "/vanilla-26.3.blocks", true);
    auto got = collect_chunks(e.port(), "Walker", ids, 60, 40);
    t.check(got.size() >= 20, "vanilla generator sent " + std::to_string(got.size()) + " chunks");
    int bedrock = 0, stone = 0, other = 0;
    for (auto& kv : got) {
        const ChunkData& c = kv.second;
        if (c.sections.empty()) continue;
        for (int i = 0; i < 4096; i += 7) {
            auto it = names.find(c.sections[0].blocks[size_t(i)]);
            std::string n = it == names.end() ? "?" : it->second;
            if (n.find("bedrock") != std::string::npos) bedrock++; else if (n.find("stone") != std::string::npos || n.find("deepslate") != std::string::npos) stone++; else other++;
        }
    }
    t.check(bedrock > 0 && stone > 0, "lowest section holds bedrock and stone");
}

void layouts(Ctx& t, Env& e) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    for (auto& en : fs::directory_iterator(e.data_dir)) {
        std::string n = en.path().filename().string();
        if (n.rfind("vanilla-", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".bin") files.push_back(en.path().string());
    }
    std::sort(files.begin(), files.end());
    for (auto& path : files) {
        std::ifstream f(path, std::ios::binary);
        Bytes d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Rd r(d, 4);
        int proto = int(r.le32());
        uint32_t n = r.le32();
        for (uint32_t i = 0; i < n; i++) { uint32_t l = r.le32(); r.skip(l); }
        uint32_t l = r.le32();
        r.skip(l);
        uint32_t nb = r.le32();
        std::map<std::string, Bytes> blobs;
        for (uint32_t i = 0; i < nb && !r.bad; i++) {
            size_t nl = r.u8();
            Bytes nm = r.take(nl);
            uint32_t len = r.le32();
            blobs[std::string(nm.begin(), nm.end())] = r.take(len);
        }
        std::string base = fs::path(path).filename().string();
        auto have = [&](const char* k) { bool h = blobs.count(k) > 0; if (!h) t.note("skip " + base + " " + k + ": regenerate the data"); return h; };
        if (have("login_finished_sample")) {
            Rd q(blobs["login_finished_sample"]);
            q.skip(16);
            q.skip(q.varint());
            uint32_t np = q.varint();
            for (uint32_t i = 0; i < np && !q.bad; i++) { q.skip(q.varint()); q.skip(q.varint()); if (q.u8()) q.skip(q.varint()); }
            t.check(!q.bad && q.left() == (proto >= 776 ? 16u : 0u), base + " (protocol " + std::to_string(proto) + ") login_finished_sample");
        }
        if (have("play_login_sample")) {
            Rd q(blobs["play_login_sample"]);
            q.skip(5);
            uint32_t nd = q.varint();
            for (uint32_t i = 0; i < nd; i++) q.skip(q.varint());
            q.varint(); q.varint(); q.varint();
            q.skip(3);
            q.varint();
            q.skip(q.varint());
            q.skip(8);
            if (proto >= 777) { q.varint(); q.varint(); } else q.skip(2);
            q.skip(2);
            if (q.u8()) { q.skip(q.varint()); q.skip(8); }
            q.varint(); q.varint();
            q.skip(proto >= 776 ? 2 : 1);
            t.check(!q.bad && q.left() == 0, base + " (protocol " + std::to_string(proto) + ") play_login_sample");
        }
        static const std::pair<const char*, size_t> fixed[] = {{"play_player_position", 61}, {"play_game_event", 5}, {"play_player_abilities", 9}, {"play_set_default_spawn_position", 36}};
        for (auto& fx : fixed)
            if (have(fx.first)) t.check(blobs[fx.first].size() == fx.second, base + " " + fx.first + " is " + std::to_string(fx.second) + " bytes");
    }
}

}  // namespace

void reg_world(Registry& reg) {
    Group g;
    g.name = "roundtrip";
    g.opts = [](const Env& e) {
        ServerOpts o;
        o.world_src = e.test_world;
        o.properties = "gamemode=creative\n";
        o.ops = {"Editor"};
        o.bedrock = false;
        return o;
    };
    g.tests.push_back({"mojang-reads-painite-world", roundtrip});
    reg.push_back(std::move(g));
    Group v;
    v.name = "vanilla-gen";
    v.opts = [](const Env&) {
        ServerOpts o;
        o.properties = "gamemode=creative\nview-distance=4\n";
        o.toml = "[world]\ngenerator = \"vanilla\"\n";
        o.bedrock = false;
        return o;
    };
    v.tests.push_back({"vanilla-terrain", vanilla_terrain});
    reg.push_back(std::move(v));
    Group l;
    l.name = "layouts";
    l.needs_server = false;
    l.opts = [](const Env&) { return ServerOpts(); };
    l.tests.push_back({"vanilla-samples", layouts});
    reg.push_back(std::move(l));
}

}  // namespace pt
