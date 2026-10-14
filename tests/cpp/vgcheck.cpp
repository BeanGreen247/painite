// Compares Painite's port of Mojang's world generation with Mojang's own code, value by value (the oracle is tools/oracle/Oracle.java run on the server jar).
//   painite_vgcheck --version 26.2 --seed 12345 [--settings overworld] [--what noise|df|climate|all] [--count 200] [--range 3000]
#include "vg/biomes.hpp"
#include "vg/climate.hpp"
#include "vg/density.hpp"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <random>
#include <sstream>

using namespace pn::vg;

struct OracleProc {
    FILE* in = nullptr;
    FILE* out = nullptr;
    OracleProc(const std::string& version, int64_t seed, const std::string& settings) {
        std::ifstream cp("build/mojang/cp-" + version + ".txt");
        std::string classpath;
        std::getline(cp, classpath);
        std::string cmd = "cd build/mojang && java -Xshare:auto -XX:TieredStopAtLevel=1 -cp \"" + classpath + "\" ../../tools/oracle/Oracle.java " + std::to_string(seed) + " " + settings + " 2>/dev/null";
        int to_child[2], from_child[2];
        if (pipe(to_child) || pipe(from_child)) throw std::runtime_error("pipe");
        pid = fork();
        if (pid == 0) {
            dup2(to_child[0], 0);
            dup2(from_child[1], 1);
            close(to_child[1]); close(from_child[0]);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        close(to_child[0]); close(from_child[1]);
        in = fdopen(to_child[1], "w");
        out = fdopen(from_child[0], "r");
    }
    ~OracleProc() { if (in) fclose(in); if (out) fclose(out); }
    std::string ask(const std::string& q) {
        fprintf(in, "%s\n", q.c_str());
        fflush(in);
        char buf[4096];
        for (;;) {
            if (!fgets(buf, sizeof buf, out)) return "";
            std::string s = buf;
            size_t p = s.find("[STDOUT]: ");
            if (p != std::string::npos) { s = s.substr(p + 10); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; }
        }
    }
    pid_t pid = 0;
};

int main(int argc, char** argv) {
    std::string version = "26.2", settings = "overworld", what = "all";
    int64_t seed = 12345;
    int count = 200, range = 3000;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--version") version = val();
        else if (a == "--seed") seed = std::stoll(val());
        else if (a == "--settings") settings = val();
        else if (a == "--what") what = val();
        else if (a == "--count") count = std::stoi(val());
        else if (a == "--range") range = std::stoi(val());
    }
    Registry reg("data/worldgen/" + version);
    const Json& st = reg.get("noise_settings", settings);
    WorldState state(reg, st, seed);
    OracleProc oracle(version, seed, settings);
    std::mt19937 rng(1234);
    auto rnd = [&](int lo, int hi) { return lo + int(rng() % unsigned(hi - lo + 1)); };
    int bad = 0;
    auto report = [&](const std::string& what_, const std::string& q, double ours, double theirs) {
        double d = std::fabs(ours - theirs);
        if (!(d == 0.0)) { if (bad++ < 12) printf("MISMATCH %s [%s]: ours %.17g oracle %.17g (diff %.3g)\n", what_.c_str(), q.c_str(), ours, theirs, d); }
    };
    if (what == "noise" || what == "all") {
        for (const char* name : {"minecraft:temperature", "minecraft:vegetation", "minecraft:continentalness", "minecraft:erosion", "minecraft:ridge", "minecraft:offset", "minecraft:jagged", "minecraft:aquifer_barrier"}) {
            const NormalNoise* n = state.noise(name);
            for (int i = 0; i < count / 8; i++) {
                double x = rnd(-range, range) + 0.37, y = rnd(-64, 200) + 0.11, z = rnd(-range, range) + 0.73;
                char q[200];
                snprintf(q, sizeof q, "noise %s %.17g %.17g %.17g", name, x, y, z);
                std::string ans = oracle.ask(q);
                std::istringstream is(ans);
                std::string nm;
                double ox, oy, oz, v;
                is >> nm >> ox >> oy >> oz >> v;
                report(std::string("noise ") + name, q, n->get_value(x, y, z), v);
            }
        }
        printf("noise checked, %d mismatches so far\n", bad);
    }
    if (what == "df" || what == "all") {
        Compiler comp(state, nullptr);
        const Json& router = st.at("noise_router");
        for (const char* name : {"continents", "erosion", "temperature", "vegetation", "depth", "ridges", "preliminary_surface_level", "final_density", "barrier", "fluid_level_floodedness", "fluid_level_spread", "lava", "vein_toggle", "vein_ridged", "vein_gap"}) {
            NodeP node = comp.compile(router.at(name));
            for (int i = 0; i < count / 10; i++) {
                int x = rnd(-range, range), y = rnd(-64, 250), z = rnd(-range, range);
                char q[200];
                snprintf(q, sizeof q, "df %s %d %d %d", name, x, y, z);
                std::string ans = oracle.ask(q);
                std::istringstream is(ans);
                std::string nm;
                int ox, oy, oz;
                double v;
                is >> nm >> ox >> oy >> oz >> v;
                report(std::string("df ") + name, q, node->compute(Ctx{x, y, z, nullptr}), v);
            }
        }
        printf("density functions checked, %d mismatches so far\n", bad);
    }
    if (what == "lists" || what == "all") {
        for (const char* preset : {"overworld", "nether"}) {
            ClimateData dump = load_climate_dump("build/mojang/climate-" + version + ".txt", preset);
            ClimateData mine = std::string(preset) == "nether" ? nether_climate() : overworld_climate();
            int diffs = 0;
            if (dump.list->size() != mine.list->size()) { printf("LIST SIZE %s: dump %zu ours %zu\n", preset, dump.list->size(), mine.list->size()); diffs++; }
            for (size_t i = 0; i < std::min(dump.list->size(), mine.list->size()) && diffs < 8; i++) {
                auto a = dump.list->point(i).space(), b = mine.list->point(i).space();
                bool same = dump.biomes[size_t(dump.list->value(i))] == mine.biomes[size_t(mine.list->value(i))];
                for (size_t k = 0; k < 7; k++) same = same && a[k].min == b[k].min && a[k].max == b[k].max;
                if (!same) { diffs++; printf("LIST DIFF %s #%zu: dump %s ours %s\n", preset, i, dump.biomes[size_t(dump.list->value(i))].c_str(), mine.biomes[size_t(mine.list->value(i))].c_str()); }
            }
            printf("%s climate list: %zu points, %d differences\n", preset, mine.list->size(), diffs);
            bad += diffs;
        }
    }
    if (what == "climate" || what == "all") {
        ClimateData cd = load_climate_dump("build/mojang/climate-" + version + ".txt", settings == "nether" ? "nether" : "overworld");
        Compiler comp(state, nullptr);
        const Json& router = st.at("noise_router");
        NodeP t = comp.compile(router.at("temperature")), h = comp.compile(router.at("vegetation")), c = comp.compile(router.at("continents")), e = comp.compile(router.at("erosion")),
              d = comp.compile(router.at("depth")), w = comp.compile(router.at("ridges"));
        int biome_bad = 0, climate_bad = 0;
        for (int i = 0; i < count * 4; i++) {
            int x = rnd(-range, range), y = rnd(-64, 250), z = rnd(-range, range);
            int bx = (x >> 2) * 4, by = (y >> 2) * 4, bz = (z >> 2) * 4;
            Ctx ctx{bx, by, bz, nullptr};
            TargetPoint tp{quantize_coord(float(t->compute(ctx))), quantize_coord(float(h->compute(ctx))), quantize_coord(float(c->compute(ctx))), quantize_coord(float(e->compute(ctx))),
                           quantize_coord(float(d->compute(ctx))), quantize_coord(float(w->compute(ctx)))};
            char q[100];
            snprintf(q, sizeof q, "climate %d %d %d", x, y, z);
            std::string ans = oracle.ask(q);
            std::istringstream is(ans);
            long long ox, oy, oz, ot, oh, oc, oe, od, ow;
            std::string biome;
            is >> ox >> oy >> oz >> ot >> oh >> oc >> oe >> od >> ow >> biome;
            if (ot != tp.temperature || oh != tp.humidity || oc != tp.continentalness || oe != tp.erosion || od != tp.depth || ow != tp.weirdness) {
                if (climate_bad++ < 6) printf("CLIMATE MISMATCH at %d %d %d: ours %lld %lld %lld %lld %lld %lld oracle %lld %lld %lld %lld %lld %lld\n", x, y, z, (long long)tp.temperature, (long long)tp.humidity,
                                              (long long)tp.continentalness, (long long)tp.erosion, (long long)tp.depth, (long long)tp.weirdness, ot, oh, oc, oe, od, ow);
                continue;
            }
            std::string mine = cd.biomes[size_t(cd.list->find_value(tp))];
            if (mine != biome) { if (biome_bad++ < 6) printf("BIOME MISMATCH at %d %d %d: ours %s oracle %s\n", x, y, z, mine.c_str(), biome.c_str()); }
        }
        printf("climate: %d climate mismatches, %d biome mismatches of %d samples\n", climate_bad, biome_bad, count * 4);
        bad += climate_bad + biome_bad;
    }
    printf(bad ? "FAILED\n" : "all values match\n");
    return bad ? 1 : 0;
}
