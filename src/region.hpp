// Scheduled block updates, sharded by region (512 x 512 blocks). Each region is processed by one thread at a time and different
// regions run in parallel, the way Folia ticks them; the world and the player registry are safe to call from any of those threads.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pn {

class Server;

class RegionTicker {
public:
    RegionTicker(Server& s) : s_(s) {}
    ~RegionTicker() { stop(); }
    void start(int threads);
    void stop();
    void schedule(int x, int y, int z, int delay);        // thread-safe
    void schedule_around(int x, int y, int z);            // the block and its six neighbours, two ticks from now
    void run(int64_t tick);                               // game thread, once per tick; returns when the due work is done
    size_t pending();
    int threads() const { return static_cast<int>(pool_.size()); }
    std::atomic<int64_t> processed{0}, changed{0};

private:
    struct Entry { int x, y, z; int64_t due; };
    struct Region {
        std::mutex mu;
        std::vector<Entry> q;
        std::unordered_set<uint64_t> keys;
    };
    Region& region_for(int x, int z);
    void process_region(Region& r, int64_t tick);
    void process_block(int x, int y, int z);
    void worker();
    Server& s_;
    std::mutex map_mu_;
    std::unordered_map<int64_t, std::unique_ptr<Region>> regions_;
    std::atomic<int64_t> now_{0};
    std::vector<std::thread> pool_;
    std::mutex job_mu_;
    std::condition_variable job_cv_, done_cv_;
    std::vector<Region*> jobs_;
    int64_t job_tick_ = 0;
    size_t running_ = 0;
    bool stop_ = false;
};

}  // namespace pn
