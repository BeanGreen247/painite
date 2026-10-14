// painite_tests: the whole end-to-end suite in one binary. Groups (one server each) run in parallel threads.
//   painite_tests [--binary build/painite] [--data data] [--world DIR] [--filter text] [--serial] [--verbose]
#include "harness.hpp"
#include "lossyproxy.hpp"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <thread>
#include <unistd.h>

namespace pt {

std::shared_ptr<Ids> Env::ids(const std::string& version) const {
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<Ids>> cache;
    std::lock_guard<std::mutex> l(m);
    auto& slot = cache[version];
    if (!slot) slot = Ids::load(data_dir + "/vanilla-" + version + ".ids");
    return slot;
}

std::string version_ids_path(const Env& e, const std::string& version) { return e.data_dir + "/vanilla-" + version + ".ids"; }

}  // namespace pt

using namespace pt;

struct Outcome {
    std::string name;
    bool ok = true;
    double secs = 0;
    std::string text;
};

int main(int argc, char** argv) {
    if (argc == 6 && std::string(argv[1]) == "proxy") {   // painite_tests proxy LISTEN_PORT TARGET_PORT LOSS_PERCENT DELAY_MS
        pt::LossyProxy p(atoi(argv[2]), atoi(argv[3]), atof(argv[4]), atoi(argv[5]));
        printf("relaying UDP 127.0.0.1:%d -> %s, loss %s%%, delay %s ms\n", p.port, argv[3], argv[4], argv[5]);
        for (;;) pause();
    }
    std::string self = argv[0];
    std::string dir = std::filesystem::absolute(self).parent_path().string();
    Env base;
    base.binary = dir + "/painite";
    base.data_dir = "data";
    std::string filter;
    bool serial = false, verbose = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--binary") base.binary = std::filesystem::absolute(val()).string();
        else if (a == "--data") base.data_dir = val();
        else if (a == "--world") base.test_world = std::filesystem::absolute(val()).string();
        else if (a == "--filter") filter = val();
        else if (a == "--serial") serial = true;
        else if (a == "--verbose") verbose = true;
    }
    base.data_dir = std::filesystem::absolute(base.data_dir).string();
    Registry reg;
    reg_java_basic(reg);
    reg_java_game(reg);
    reg_dimensions(reg);
    reg_bedrock(reg);
    reg_online(reg);
    reg_world(reg);

    std::vector<std::vector<Outcome>> results(reg.size());
    std::atomic<size_t> next{0};
    auto run_group = [&](size_t gi) {
        Group& g = reg[gi];
        std::vector<const TestDef*> todo;
        for (auto& t : g.tests) if (filter.empty() || (g.name + "/" + t.name).find(filter) != std::string::npos) todo.push_back(&t);
        if (todo.empty()) return;
        Env env = base;
        std::unique_ptr<Server> srv;
        std::string fatal;
        if (g.needs_server) {
            try { srv = std::make_unique<Server>(base.binary, g.name, g.opts(env)); env.srv = srv.get(); }
            catch (const std::exception& e) { fatal = e.what(); }
        }
        for (const TestDef* t : todo) {
            Outcome o;
            o.name = g.name + "/" + t->name;
            Ctx ctx;
            ctx.name = o.name;
            int64_t t0 = now_ms();
            if (!fatal.empty()) { ctx.check(false, "server did not start: " + fatal); }
            else {
                try { t->fn(ctx, env); }
                catch (const std::exception& e) { ctx.check(false, std::string("exception: ") + e.what()); }
                if (srv && !srv->alive()) ctx.check(false, "the server died: " + srv->log());
            }
            o.secs = double(now_ms() - t0) / 1000.0;
            o.ok = ctx.ok;
            o.text = ctx.out.str();
            results[gi].push_back(std::move(o));
        }
    };
    unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    size_t nthreads = serial ? 1 : std::min<size_t>(hw, reg.size());
    int64_t t0 = now_ms();
    std::vector<std::thread> th;
    for (size_t i = 0; i < nthreads; i++)
        th.emplace_back([&] { for (;;) { size_t g = next++; if (g >= reg.size()) return; run_group(g); } });
    for (auto& t : th) t.join();
    int failed = 0, total = 0;
    for (auto& gr : results)
        for (auto& o : gr) {
            total++;
            if (!o.ok) failed++;
            printf("%s  %-52s %5.1fs\n", o.ok ? "PASS" : "FAIL", o.name.c_str(), o.secs);
            if (!o.ok || verbose) printf("%s", o.text.c_str());
        }
    printf("%d tests, %d failed, %.1fs\n", total, failed, double(now_ms() - t0) / 1000.0);
    return failed ? 1 : 0;
}
