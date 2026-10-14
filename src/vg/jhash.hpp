// java.util.HashSet<BlockPos> iteration order: tree decorators walk sets of positions, and which position comes first changes their random draws.
#pragma once
#include "level.hpp"
#include <vector>

namespace pn::vg {

class JHashSet {
public:
    bool add(const BlockPos& p) {
        if (table_.empty()) resize();
        size_t idx = bucket(p, table_.size());
        auto& b = table_[idx];
        for (auto& q : b) if (q == p) return false;
        b.push_back(p);
        // HashMap.putVal: treeifyBin on the 9th node; below 64 buckets that means a resize
        if (b.size() >= 9 && table_.size() < 64) resize();
        if (++size_ > threshold_) resize();
        return true;
    }
    bool contains(const BlockPos& p) const {
        if (table_.empty()) return false;
        for (auto& q : table_[bucket(p, table_.size())]) if (q == p) return true;
        return false;
    }
    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }
    std::vector<BlockPos> items() const {   // iteration order
        std::vector<BlockPos> out;
        out.reserve(size_);
        for (auto& b : table_) out.insert(out.end(), b.begin(), b.end());
        return out;
    }
private:
    static size_t bucket(const BlockPos& p, size_t n) {
        uint32_t h = uint32_t(p.hash());
        h ^= h >> 16;
        return h & (n - 1);
    }
    void resize() {
        size_t old_cap = table_.size();
        size_t cap = old_cap ? old_cap * 2 : 16;
        std::vector<std::vector<BlockPos>> next(cap);
        for (size_t j = 0; j < old_cap; j++)
            for (auto& p : table_[j]) next[bucket(p, cap)].push_back(p);   // same relative order as HashMap.resize splitting lo/hi
        table_ = std::move(next);
        threshold_ = cap * 3 / 4;
    }
    std::vector<std::vector<BlockPos>> table_;
    size_t size_ = 0, threshold_ = 12;
};

}  // namespace pn::vg
