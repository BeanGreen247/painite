// Builds a reference world with Mojang's own server so Painite's world generation can be compared with it.
//   painite_refgen --version 26.3 --seed 12345 --out DIR [--area X1 Z1 X2 Z2] [--dim overworld|the_nether|the_end] [--structures] [--type default|large_biomes|amplified|flat]
// The area is in blocks; it is loaded in 16x16 chunk batches with /forceload, then the world is saved and the server stopped. DIR/world is the result.
#include "jbot.hpp"
#include "server.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace pt;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::string version = "26.3", out, dim = "overworld", type = "default", seed = "12345";
    int x1 = -128, z1 = -128, x2 = 127, z2 = 127;
    bool structures = false;
    std::string data = "data";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--version") version = val();
        else if (a == "--seed") seed = val();
        else if (a == "--out") out = val();
        else if (a == "--dim") dim = val();
        else if (a == "--type") type = val();
        else if (a == "--data") data = val();
        else if (a == "--structures") structures = true;
        else if (a == "--area") { x1 = std::stoi(val()); z1 = std::stoi(val()); x2 = std::stoi(val()); z2 = std::stoi(val()); }
    }
    if (out.empty()) { fprintf(stderr, "usage: painite_refgen --out DIR [--version 26.3] [--seed N] [--area X1 Z1 X2 Z2] [--dim overworld|the_nether|the_end] [--structures] [--type default|large_biomes|amplified|flat]\n"); return 2; }
    out = fs::absolute(out).string();
    std::string jar = "build/mojang/server-" + version + ".jar";
    if (!fs::exists(jar)) { fprintf(stderr, "missing %s (run tools/fetch_worldgen.sh %s)\n", jar.c_str(), version.c_str()); return 1; }
    jar = fs::absolute(jar).string();
    fs::remove_all(out);
    fs::create_directories(out + "/run");
    std::string run = out + "/run";
    int port = Server::free_port();
    {
        std::ofstream f(run + "/server.properties");
        f << "server-port=" << port << "\nonline-mode=false\nlevel-name=world\nlevel-seed=" << seed << "\nlevel-type=minecraft\\:" << (type == "default" ? "normal" : type)
          << "\ngenerate-structures=" << (structures ? "true" : "false") << "\nview-distance=4\nsimulation-distance=2\nnetwork-compression-threshold=-1\nspawn-monsters=false\nspawn-animals=false\nspawn-npcs=false\n"
          << "max-tick-time=-1\nspawn-protection=0\ngamemode=creative\n";
    }
    { std::ofstream f(run + "/eula.txt"); f << "eula=true\n"; }
    {
        Bytes u = offline_uuid("Ref");
        char s[40];
        snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        std::ofstream f(run + "/ops.json");
        f << "[{\"uuid\":\"" << s << "\",\"name\":\"Ref\",\"level\":4,\"bypassesPlayerLimit\":false}]";
    }
    std::string java = "java";
    Proc vanilla({java, "-Xmx2G", "-XX:+UseSerialGC", "-jar", jar, "nogui"}, run, run + "/vanilla.log");
    for (int i = 0; i < 3000; i++) {
        if (vanilla.log().find("Done") != std::string::npos) break;
        if (!vanilla.alive()) { fprintf(stderr, "Mojang's server exited:\n%s\n", vanilla.log().c_str()); return 1; }
        usleep(100000);
    }
    std::string idsfile = data + "/vanilla-" + version + ".ids";
    auto ids = Ids::load(idsfile);
    int proto = version == "26.2" ? 776 : 777;
    {
        JBot bot(port, proto, ids, "Ref");
        bot.pump(1500);
        int total = 0;
        std::string prefix = dim == "overworld" ? "" : "execute in minecraft:" + dim + " run ";
        // 16x16 chunk batches
        int bx1 = (x1 >> 4), bz1 = (z1 >> 4), bx2 = (x2 >> 4), bz2 = (z2 >> 4);
        for (int cz = bz1; cz <= bz2; cz += 16)
            for (int cx = bx1; cx <= bx2; cx += 16) {
                int ex = std::min(cx + 15, bx2), ez = std::min(cz + 15, bz2);
                char b[160];
                snprintf(b, sizeof b, "%sforceload add %d %d %d %d", prefix.c_str(), cx * 16, cz * 16, ex * 16 + 15, ez * 16 + 15);
                bot.drop("system_chat");
                bot.command(b);
                bot.until([&] { return bot.count("system_chat") >= 1; }, 120000);
                total += (ex - cx + 1) * (ez - cz + 1);
                fprintf(stderr, "forceloaded %d chunks\n", total);
                bot.settle(200, 2000);
            }
        // give the generator time to finish everything that was requested
        for (int i = 0; i < 600; i++) {
            bot.pump(500);
            std::string log = vanilla.log();
            if (i > 6 && log.find("Can't keep up") == std::string::npos) break;
        }
        bot.command("save-all flush");
        bot.until([&] { for (auto& e : bot.log) if (e.first == "system_chat" && contains(e.second, "Saved")) return true; return false; }, 120000);
    }
    vanilla.terminate(60000);
    fprintf(stderr, "world written to %s/world\n", run.c_str());
    fs::rename(run + "/world", out + "/world");
    return 0;
}
