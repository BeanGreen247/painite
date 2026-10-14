// Load test: many Java bots join, wander, build and chat; reports the server's CPU, memory and tick time.
//   painite_load --port P --pid SERVER_PID [--bots 40] [--secs 20] [--protocol 777] [--ids data/vanilla-26.3.ids]   (offline mode; "Bobby" in ops.json for the /tps readout)
#include "jbot.hpp"
#include <atomic>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>

using namespace pt;

static double cpu_secs(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t p = s.rfind(')');
    std::istringstream is(s.substr(p + 2));
    std::string tok;
    long ut = 0, st = 0;
    std::vector<std::string> v;   // the fields after ")" start at the state; utime and stime are the 12th and 13th of them
    while (is >> tok) v.push_back(tok);
    if (v.size() > 12) { ut = std::stol(v[11]); st = std::stol(v[12]); }
    return double(ut + st) / double(sysconf(_SC_CLK_TCK));
}

static std::map<std::string, std::pair<std::string, double>> thread_cpu(int pid) {
    std::map<std::string, std::pair<std::string, double>> out;
    std::string dir = "/proc/" + std::to_string(pid) + "/task";
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::ifstream f(dir + "/" + e->d_name + "/stat");
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            size_t a = s.find('('), b = s.rfind(')');
            if (a == std::string::npos || b == std::string::npos) continue;
            std::istringstream is(s.substr(b + 2));
            std::vector<std::string> v;
            std::string tok;
            while (is >> tok) v.push_back(tok);
            if (v.size() > 12) out[e->d_name] = {s.substr(a + 1, b - a - 1), double(std::stol(v[11]) + std::stol(v[12])) / double(sysconf(_SC_CLK_TCK))};
        }
        closedir(d);
    }
    return out;
}

static double rss_mb(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line)) if (line.rfind("VmRSS", 0) == 0) return std::stod(line.substr(6)) / 1024.0;
    return 0;
}

int main(int argc, char** argv) {
    int port = 0, pid = 0, bots = 40, secs = 20, protocol = 777;
    std::string idsfile = "data/vanilla-26.3.ids";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--port") port = std::stoi(val());
        else if (a == "--pid") pid = std::stoi(val());
        else if (a == "--bots") bots = std::stoi(val());
        else if (a == "--secs") secs = std::stoi(val());
        else if (a == "--protocol") protocol = std::stoi(val());
        else if (a == "--ids") idsfile = val();
    }
    if (!port || !pid) { fprintf(stderr, "usage: painite_load --port P --pid SERVER_PID [--bots 40] [--secs 20]\n"); return 2; }
    auto ids = Ids::load(idsfile);
    std::atomic<int> ok{0};
    std::mutex em;
    std::vector<std::string> errs;
    double c0 = cpu_secs(pid);
    int64_t t0 = now_ms();
    auto bot = [&](int idx) {
        char name[32];
        snprintf(name, sizeof name, "Load%03d", idx);
        try {
            JBot b(port, protocol, ids, name);
            b.pump(1500);
            std::mt19937 rng(unsigned(idx) * 7919u + 1);
            std::uniform_real_distribution<double> d01(0, 1), dd(-0.3, 0.3);
            double x = b.pos[0], y = b.pos[1], z = b.pos[2];
            double ang = d01(rng) * 6.28;
            b.send("set_creative_mode_slot", Wr().be16(36).varint(64).varint(uint32_t(ids->items.at("stone"))).varint(0).varint(0).b);
            int64_t end = now_ms() + secs * 1000;
            int n = 0;
            while (now_ms() < end) {
                ang += dd(rng);
                x += std::cos(ang) * 0.25;
                z += std::sin(ang) * 0.25;
                b.send("move_player_pos_rot", Wr().f64(x).f64(y).f64(z).f32(float(ang * 57.29578)).f32(0.0f).u8(1).b);
                b.send("client_tick_end");
                n++;
                if (n % 40 == 0) b.send("use_item_on", Wr().varint(0).be64(block_pos(int(x) + 1, int(y) - 1, int(z))).varint(1).f32(0.5f).f32(1.0f).f32(0.5f).u8(0).u8(0).varint(uint32_t(n)).b);
                if (n % 100 == 0) b.send("chat", Wr().str(std::string("hello ") + name).be64(uint64_t(unix_ms())).be64(0).u8(0).varint(0).zeros(3).u8(0).b);
                b.pump(50);
                b.log.clear();
            }
            ok++;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> l(em);
            errs.push_back(e.what());
        }
    };
    std::vector<std::thread> th;
    for (int i = 0; i < bots; i++) { th.emplace_back(bot, i); usleep(150000); }
    sleep(3);
    double cj = cpu_secs(pid);
    int64_t tj = now_ms();
    auto thj = thread_cpu(pid);
    printf("join phase: %.1fs CPU in %.1fs\n", cj - c0, double(tj - t0) / 1000.0);
    std::string tps;
    try {
        JBot op(port, protocol, ids, "Bobby");
        op.pump(int(secs * 600));
        op.command("tps");
        op.until([&] { for (auto& e : op.log) if (e.first == "system_chat" && contains(e.second, "TPS")) return true; return false; }, 3000);
        for (auto& e : op.log) if (e.first == "system_chat" && contains(e.second, "TPS")) { std::string s(e.second.begin(), e.second.end()); size_t p = s.find("TPS"); tps = s.substr(p, 100); }
    } catch (const std::exception& e) { tps = std::string("(no readout: ") + e.what() + ")"; }
    for (auto& t : th) t.join();
    double c1 = cpu_secs(pid);
    int64_t t1 = now_ms();
    auto the = thread_cpu(pid);
    printf("per thread in the steady phase:");
    for (auto& kv : the) {
        double before = thj.count(kv.first) ? thj[kv.first].second : 0;
        if (kv.second.second - before > 0.05) printf(" %s#%s=%.1fs,", kv.second.first.c_str(), kv.first.c_str(), kv.second.second - before);
    }
    printf("\nsteady phase: %.1fs CPU in %.1fs = %.0f%% of one core\n", c1 - cj, double(t1 - tj) / 1000.0, 100 * (c1 - cj) / (double(t1 - tj) / 1000.0));
    printf("%d bots, %ds of movement: %d finished, %zu errors %s\n", bots, secs, ok.load(), errs.size(), errs.empty() ? "" : errs[0].c_str());
    printf("server CPU %.1fs over %.1fs = %.0f%% of one core, RSS %.0f MB\n", c1 - c0, double(t1 - t0) / 1000.0, 100 * (c1 - c0) / (double(t1 - t0) / 1000.0), rss_mb(pid));
    if (!tps.empty()) printf("tps readout: %s\n", tps.c_str());
    return ok == bots ? 0 : 1;
}
