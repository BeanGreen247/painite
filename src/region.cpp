#include "region.hpp"
#include "dimension.hpp"
#include "log.hpp"
#include "server.hpp"
#include <algorithm>
#include <cmath>
#include <pthread.h>

namespace pn {

static uint64_t pos_key(int x, int y, int z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) ^ (static_cast<uint64_t>(static_cast<uint32_t>(z)) << 8) ^ static_cast<uint64_t>(static_cast<uint32_t>(y + 64));
}

RegionTicker::Region& RegionTicker::region_for(int dim, int x, int z) {
    int64_t key = ((static_cast<int64_t>(x >> 9) << 32) | static_cast<uint32_t>(z >> 9)) ^ static_cast<int64_t>(static_cast<uint64_t>(dim) * 0x9e3779b97f4a7c15ull);
    std::lock_guard<std::mutex> l(map_mu_);
    auto& slot = regions_[key];
    if (!slot) slot = std::make_unique<Region>();
    return *slot;
}

void RegionTicker::schedule(int dim, int x, int y, int z, int delay) {
    if (y < kMinY || y >= kMinY + kHeight) return;
    Region& r = region_for(dim, x, z);
    std::lock_guard<std::mutex> l(r.mu);
    if (!r.keys.insert(pos_key(x, y, z)).second) return;
    r.q.push_back({dim, x, y, z, now_.load() + delay});
}

void RegionTicker::schedule_around(int dim, int x, int y, int z) {
    schedule(dim, x, y, z, 2);
    schedule(dim, x + 1, y, z, 2); schedule(dim, x - 1, y, z, 2);
    schedule(dim, x, y + 1, z, 2); schedule(dim, x, y - 1, z, 2);
    schedule(dim, x, y, z + 1, 2); schedule(dim, x, y, z - 1, 2);
}

size_t RegionTicker::pending() {
    std::lock_guard<std::mutex> l(map_mu_);
    size_t n = 0;
    for (auto& kv : regions_) { std::lock_guard<std::mutex> rl(kv.second->mu); n += kv.second->q.size(); }
    return n;
}

void RegionTicker::start(int threads) {
    for (int i = 1; i < threads; i++) pool_.emplace_back([this] { worker(); });   // the game thread takes part too
}

void RegionTicker::stop() {
    {
        std::lock_guard<std::mutex> l(job_mu_);
        if (stop_) return;
        stop_ = true;
    }
    job_cv_.notify_all();
    for (auto& t : pool_) if (t.joinable()) t.join();
    pool_.clear();
}

void RegionTicker::worker() {
    pthread_setname_np(pthread_self(), "pn-region");
    std::unique_lock<std::mutex> l(job_mu_);
    for (;;) {
        job_cv_.wait(l, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        Region* r = jobs_.back();
        jobs_.pop_back();
        running_++;
        int64_t tick = job_tick_;
        l.unlock();
        try { process_region(*r, tick); } catch (const std::exception& e) { error(std::string("region tick failed: ") + e.what()); }
        l.lock();
        running_--;
        if (jobs_.empty() && running_ == 0) done_cv_.notify_all();
    }
}

void RegionTicker::run(int64_t tick) {
    now_ = tick;
    std::vector<Region*> due;
    {
        std::lock_guard<std::mutex> l(map_mu_);
        for (auto& kv : regions_) {
            Region& r = *kv.second;
            std::lock_guard<std::mutex> rl(r.mu);
            for (auto& e : r.q) if (e.due <= tick) { due.push_back(&r); break; }
        }
    }
    if (due.empty()) return;
    if (pool_.empty() || due.size() == 1) {
        for (Region* r : due) process_region(*r, tick);
        return;
    }
    {
        std::lock_guard<std::mutex> l(job_mu_);
        job_tick_ = tick;
        jobs_ = due;
    }
    job_cv_.notify_all();
    // the game thread helps until every region is taken, then waits for the stragglers
    for (;;) {
        Region* r = nullptr;
        {
            std::lock_guard<std::mutex> l(job_mu_);
            if (!jobs_.empty()) { r = jobs_.back(); jobs_.pop_back(); running_++; }
        }
        if (!r) break;
        try { process_region(*r, tick); } catch (const std::exception& e) { error(std::string("region tick failed: ") + e.what()); }
        std::lock_guard<std::mutex> l(job_mu_);
        running_--;
    }
    std::unique_lock<std::mutex> l(job_mu_);
    done_cv_.wait(l, [this] { return jobs_.empty() && running_ == 0; });
}

void RegionTicker::process_region(Region& r, int64_t tick) {
    std::vector<Entry> due;
    {
        std::lock_guard<std::mutex> l(r.mu);
        size_t keep = 0;
        for (auto& e : r.q) {
            if (e.due <= tick && due.size() < 1024) { due.push_back(e); r.keys.erase(pos_key(e.x, e.y, e.z)); }
            else r.q[keep++] = e;
        }
        r.q.resize(keep);
    }
    for (auto& e : due) {
        process_block(e.dim, e.x, e.y, e.z);
        processed++;
    }
}

// What a block does when something next to it changed: sand-like blocks fall, plants lose their ground, tall plants lose their other half.
void RegionTicker::process_block(int dim, int x, int y, int z) {
    World& w = s_.dim(dim);
    const int min_y = dim_info(dim).min_y;
    Blocks& B = Blocks::get();
    if (!w.chunk_if_loaded(x >> 4, z >> 4)) return;   // nobody is there; the block settles when the chunk is next used
    uint16_t id = w.get_block(x, y, z);
    if (!id) return;
    const BlockInfo& bi = B.info(id);
    auto set = [&](int bx, int by, int bz, uint16_t v) {
        if (w.set_block(bx, by, bz, v)) { s_.game.block_changed(dim, bx, by, bz, v); changed++; return true; }
        return false;
    };
    auto free_below = [&](int bx, int by, int bz) {
        if (by <= min_y) return false;
        const BlockInfo& b = B.info(w.get_block(bx, by - 1, bz));
        return b.air || b.replaceable || b.fluid;
    };
    if (bi.falling) {
        if (!free_below(x, y, z)) return;
        int ly = y;
        while (free_below(x, ly, z)) ly--;
        if (ly == y) return;
        set(x, y, z, 0);
        set(x, ly, z, id);
        return;
    }
    if (bi.needs_floor) {
        const BlockInfo& below = B.info(w.get_block(x, y - 1, z));
        bool upper = bi.props.find("half=upper") != std::string::npos;
        bool ok = upper ? below.name == bi.name : (below.solid || below.name == "minecraft:farmland");
        if (!ok) set(x, y, z, 0);
    }
}

}  // namespace pn
