// Cheap named timers for the tasks the server runs (tick phases, chunk load/encode, saves ...), read by the `perf` command.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pn {

struct PerfTimer {
    std::string name;
    std::atomic<uint64_t> n{0}, total_ns{0}, max_ns{0};
    void add(uint64_t ns) {
        n++;
        total_ns += ns;
        uint64_t m = max_ns.load(std::memory_order_relaxed);
        while (ns > m && !max_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
    }
};

// Process-wide registry; timers are created on first use and live for the whole run.
struct PerfRegistry {
    std::mutex mu;
    std::vector<PerfTimer*> all;
};
inline PerfRegistry& perf_registry() { static PerfRegistry r; return r; }
inline PerfTimer& perf_timer(const char* name) {
    auto& r = perf_registry();
    std::lock_guard<std::mutex> l(r.mu);
    for (auto* t : r.all) if (t->name == name) return *t;
    r.all.push_back(new PerfTimer());
    r.all.back()->name = name;
    return *r.all.back();
}
inline std::vector<PerfTimer*> perf_all() { auto& r = perf_registry(); std::lock_guard<std::mutex> l(r.mu); return r.all; }

struct PerfScope {
    PerfTimer& t;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit PerfScope(PerfTimer& timer) : t(timer) {}
    ~PerfScope() { t.add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count())); }
};
#define PN_CAT2(a, b) a##b
#define PN_CAT(a, b) PN_CAT2(a, b)
#define PN_PERF(name) static ::pn::PerfTimer& PN_CAT(pn_perf_t_, __LINE__) = ::pn::perf_timer(name); ::pn::PerfScope PN_CAT(pn_perf_s_, __LINE__)(PN_CAT(pn_perf_t_, __LINE__))

}  // namespace pn
