// Compares Painite's port of the 26.3 world generation with Mojang's own code bit by bit (the oracle is tools/oracle/Oracle3.java on the 26.3 server jar).
//   painite_v3check --seed 12345 [--settings overworld] [--what noise|noisevol|all] [--count 100]
#include "vg/df3.hpp"
#include "vg/noise3.hpp"
#include "vg/registry.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <unistd.h>

using namespace pn::vg;
namespace v3 = pn::vg::v3;

struct OracleProc {
    FILE* in = nullptr;
    FILE* out = nullptr;
    OracleProc(int64_t seed, const std::string& settings) {
        std::ifstream cp("build/mojang/cp-26.3.txt");
        std::string classpath;
        std::getline(cp, classpath);
        std::string cmd = "cd build/mojang && java -Xshare:auto -XX:TieredStopAtLevel=1 -cp \"" + classpath + "\" ../../tools/oracle/Oracle3.java " + std::to_string(seed) + " " + settings + " 2>/dev/null";
        int to_child[2], from_child[2];
        if (pipe(to_child) || pipe(from_child)) throw std::runtime_error("pipe");
        pid_t pid = fork();
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
        char* line = nullptr;
        size_t cap = 0;
        for (;;) {
            ssize_t n = getline(&line, &cap, out);
            if (n < 0) { free(line); return ""; }
            std::string s(line, size_t(n));
            size_t p = s.find("[STDOUT]: ");
            if (p != std::string::npos) { free(line); s = s.substr(p + 10); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; }
        }
    }
};

static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// NoiseParams of data/minecraft/worldgen/noise/NAME.json
static v3::NoiseParams noise_params(const Registry& reg, const std::string& name) {
    const Json& j = reg.get("noise", name);
    v3::NoiseParams p;
    p.base_amplitude = j.has("base_amplitude") ? j.at("base_amplitude").number() : 1.0;
    p.base_octave = int(j.at("base_octave").int_value());
    p.octave_count = int(j.get_int("octave_count", 1));
    if (const Json* n = j.find("normalize")) p.normalize = n->is_bool() ? (n->b ? v3::NoiseParams::ENABLED : v3::NoiseParams::DISABLED) : v3::NoiseParams::LEGACY;
    if (const Json* a = j.find("amplitude_modifiers")) for (auto& e : a->arr) p.amplitude_modifiers.push_back(e.number());
    return p;
}

int main(int argc, char** argv) {
    std::string settings = "overworld", what = "all";
    int64_t seed = 12345;
    int count = 100, range = 3000;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--seed") seed = std::stoll(val());
        else if (a == "--settings") settings = val();
        else if (a == "--what") what = val();
        else if (a == "--count") count = std::stoi(val());
        else if (a == "--range") range = std::stoi(val());
    }
    Registry reg("data/worldgen/26.3");
    OracleProc oracle(seed, settings);
    std::mt19937 rng(1234);
    auto rnd = [&](int lo, int hi) { return lo + int(rng() % unsigned(hi - lo + 1)); };
    int bad = 0, total = 0;
    auto report = [&](const std::string& what_, const std::string& q, uint32_t ours, uint32_t theirs) {
        total++;
        if (ours != theirs) {
            float a, b;
            memcpy(&a, &ours, 4); memcpy(&b, &theirs, 4);
            if (bad++ < 12) printf("MISMATCH %s [%s]: ours %08x (%.9g) oracle %08x (%.9g)\n", what_.c_str(), q.c_str(), ours, a, theirs, b);
        }
    };
    if (what == "noise" || what == "noisevol" || what == "all") {
        Xoroshiro base(seed);
        auto factory = base.fork_positional();
        for (const char* name : {"temperature", "vegetation", "continentalness", "erosion", "ridge", "offset", "jagged", "aquifer_barrier", "cave_cheese", "spaghetti_2d", "surface", "calcite"}) {
            auto r = factory->from_hash_of(std::string("minecraft:") + name);
            auto stack = v3::create_normal_noise(noise_params(reg, name), *r);
            for (int i = 0; i < count / 4; i++) {
                double x = rnd(-range, range) + 0.37, y = rnd(-64, 200) + 0.11, z = rnd(-range, range) + 0.73;
                char q[200];
                snprintf(q, sizeof q, "noise %s %.17g %.17g %.17g", name, x, y, z);
                std::string ans = oracle.ask(q);
                size_t eq = ans.find(" = ");
                uint32_t theirs = eq == std::string::npos ? 0 : uint32_t(std::stoul(ans.substr(eq + 3, 8), nullptr, 16));
                report(std::string("noise ") + name, q, bits_of(stack->get(x, y, z)), theirs);
            }
            if (what == "noise") continue;
            for (int i = 0; i < 4; i++) {
                int x = rnd(-range, range), y = rnd(-64, 150), z = rnd(-range, range);
                int sx = rnd(1, 6), sy = rnd(1, 20), sz = rnd(1, 6);
                double xz = i % 2 ? 0.25 : 1.0, ys = i < 2 ? 1.0 : 0.5;
                char q[200];
                snprintf(q, sizeof q, "noisevol %s %d %d %d %d %d %d %.17g %.17g 1.0", name, x, y, z, sx, sy, sz, xz, ys);
                std::string ans = oracle.ask(q);
                std::istringstream is(ans);
                std::string tok, nm;
                is >> tok >> nm;
                v3::Volume vol;
                vol.sx = sx; vol.sy = sy; vol.sz = sz; vol.minx = x; vol.miny = y; vol.minz = z;
                std::vector<float> buf(size_t(vol.size()), 0.0f);
                stack->add_to_volume(buf.data(), vol, xz, ys, 1.0f);
                for (int k = 0; k < vol.size(); k++) {
                    std::string h;
                    is >> h;
                    report(std::string("noisevol ") + name, std::string(q) + " #" + std::to_string(k), bits_of(buf[size_t(k)]), uint32_t(std::stoul(h, nullptr, 16)));
                }
            }
        }
        printf("noise checked\n");
    }
    if (what == "df" || what == "vol" || what == "all") {
        v3::RandomState3 rs(reg, seed, false);
        v3::DfCompiler comp(rs);
        const Json& router = reg.get("noise_settings", settings).at("noise_router");
        v3::SamplerCtx plain, cached;
        cached.caches = true;
        const char* names[] = {"temperature", "vegetation", "continents", "erosion", "depth", "ridges", "chunk_surface_level", "final_density"};
        for (const char* name : names) {
            const v3::Sampler* sm = comp.sampler(router.at(name));
            if (what != "vol") {
                for (int i = 0; i < count / 4; i++) {
                    int x = rnd(-range, range), y = rnd(-64, 250), z = rnd(-range, range);
                    char q[200];
                    snprintf(q, sizeof q, "df %s %d %d %d", name, x, y, z);
                    std::string ans = oracle.ask(q);
                    size_t eq = ans.find(" = ");
                    uint32_t theirs = eq == std::string::npos ? 0 : uint32_t(std::stoul(ans.substr(eq + 3, 8), nullptr, 16));
                    report(std::string("df ") + name, q, bits_of(sm->value(plain, x, y, z)), theirs);
                }
            }
            if (what == "df") continue;
            for (int i = 0; i < 6; i++) {
                int x = rnd(-range, range), y = rnd(-64, 100), z = rnd(-range, range);
                int sx = rnd(1, 9), sy = rnd(1, 40), sz = rnd(1, 9);
                if (i == 0) { x = x & ~15; z = z & ~15; y = -64; sx = 16; sz = 16; sy = 384; }
                if (i == 1) { x -= x % 4; y -= y % 8; z -= z % 4; }
                char q[200];
                snprintf(q, sizeof q, "vol %s %d %d %d %d %d %d", name, x, y, z, sx, sy, sz);
                std::string ans = oracle.ask(q);
                std::istringstream is(ans);
                std::string tok, nm;
                is >> tok >> nm;
                v3::Volume vol;
                vol.sx = sx; vol.sy = sy; vol.sz = sz; vol.minx = x; vol.miny = y; vol.minz = z;
                std::vector<float> buf(size_t(vol.size()), 0.0f);
                sm->volume(cached, buf.data(), vol);
                for (int k = 0; k < vol.size(); k++) {
                    std::string h;
                    is >> h;
                    report(std::string("vol ") + name, std::string(q) + " #" + std::to_string(k), bits_of(buf[size_t(k)]), uint32_t(std::stoul(h, nullptr, 16)));
                }
            }
        }
        printf("density functions checked\n");
    }
    printf("%d values compared, %d mismatches\n", total, bad);
    return bad ? 1 : 0;
}
