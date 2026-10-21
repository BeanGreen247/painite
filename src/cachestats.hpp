// Counters for every cache in the server: entries, bytes, hits and misses. The `cache` command prints them.
// Header only, so every program that builds a cached component links it without further changes.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pn {

struct CacheStat {
    std::string name;
    std::atomic<uint64_t> hits{0}, misses{0};
    std::atomic<int64_t> entries{0}, bytes{0};     // kept up to date by the cache itself ...
    std::function<void(int64_t& entries, int64_t& bytes)> sampler;   // ... or measured on demand when this is set
    int64_t limit_bytes = 0;                       // 0: not bounded
    bool counts_lookups = true;                    // false: a static table, hits and misses make no sense
    bool bytes_are_source = false;                 // the size is that of the files the entries were read from; the parsed form is larger
    void hit() { hits.fetch_add(1, std::memory_order_relaxed); }
    void miss() { misses.fetch_add(1, std::memory_order_relaxed); }
    void add(int64_t n_entries, int64_t n_bytes) { entries.fetch_add(n_entries, std::memory_order_relaxed); bytes.fetch_add(n_bytes, std::memory_order_relaxed); }
};

inline std::mutex& cache_registry_mutex() { static std::mutex m; return m; }
inline std::deque<CacheStat>& cache_registry() { static std::deque<CacheStat> d; return d; }   // a deque: addresses stay valid

inline CacheStat& cache_stat(const std::string& name) {
    std::lock_guard<std::mutex> l(cache_registry_mutex());
    for (CacheStat& s : cache_registry()) if (s.name == name) return s;
    cache_registry().emplace_back();
    cache_registry().back().name = name;
    return cache_registry().back();
}

inline std::vector<CacheStat*> cache_stats() {
    std::lock_guard<std::mutex> l(cache_registry_mutex());
    std::vector<CacheStat*> v;
    for (CacheStat& s : cache_registry()) v.push_back(&s);
    return v;
}

}  // namespace pn
