#include "climate.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace pn::vg {

int64_t quantize_coord(float v) { return int64_t(v * 10000.0f); }

namespace {
constexpr int kChildren = 6;
}

struct ParameterList::Node {
    std::array<Parameter, 7> space;
    std::vector<Node*> children;   // empty for leaves
    int leaf_index = -1;           // index into points_/values_ for leaves
    int64_t distance(const std::array<int64_t, 7>& target) const {
        int64_t d = 0;
        for (int i = 0; i < 7; i++) { int64_t x = space[size_t(i)].distance(target[size_t(i)]); d += x * x; }
        return d;
    }
};

struct Builder {
    ParameterList& L;
    using Node = ParameterList::Node;

    static int64_t cost(const std::array<Parameter, 7>& s) { int64_t r = 0; for (auto& p : s) r += std::llabs(p.max - p.min); return r; }

    Node* subtree(std::vector<Node*> children) {
        auto n = std::make_unique<Node>();
        bool first = true;
        for (int d = 0; d < 7; d++) n->space[size_t(d)] = Parameter{0, 0};
        std::array<bool, 7> have{};
        for (Node* c : children)
            for (int d = 0; d < 7; d++) {
                const Parameter& p = c->space[size_t(d)];
                if (!have[size_t(d)]) { n->space[size_t(d)] = p; have[size_t(d)] = true; }
                else n->space[size_t(d)] = Parameter{std::min(n->space[size_t(d)].min, p.min), std::max(n->space[size_t(d)].max, p.max)};
            }
        (void)first;
        n->children = std::move(children);
        Node* raw = n.get();
        L.pool_.push_back(std::move(n));
        return raw;
    }

    static int64_t center(const Node* n, int dim, bool absolute) {
        const Parameter& p = n->space[size_t(dim)];
        int64_t c = (p.min + p.max) / 2;   // Java long division truncates toward zero like C++
        return absolute ? std::llabs(c) : c;
    }

    static void sort_nodes(std::vector<Node*>& nodes, int dimensions, int dimension, bool absolute) {
        std::stable_sort(nodes.begin(), nodes.end(), [&](const Node* a, const Node* b) {
            for (int d = 0; d < dimensions; d++) {
                int dim = (dimension + d) % dimensions;
                int64_t ca = center(a, dim, absolute), cb = center(b, dim, absolute);
                if (ca != cb) return ca < cb;
            }
            return false;
        });
    }

    std::vector<Node*> bucketize(const std::vector<Node*>& nodes) {
        std::vector<Node*> buckets;
        std::vector<Node*> children;
        int expected = int(std::pow(double(kChildren), std::floor(std::log(double(nodes.size()) - 0.01) / std::log(double(kChildren)))));
        for (Node* c : nodes) {
            children.push_back(c);
            if (int(children.size()) >= expected) { buckets.push_back(subtree(children)); children.clear(); }
        }
        if (!children.empty()) buckets.push_back(subtree(children));
        return buckets;
    }

    Node* build(int dimensions, std::vector<Node*> children) {
        if (children.size() == 1) return children[0];
        if (children.size() <= size_t(kChildren)) {
            std::stable_sort(children.begin(), children.end(), [&](const Node* a, const Node* b) {
                int64_t ma = 0, mb = 0;
                for (int d = 0; d < dimensions; d++) { ma += std::llabs((a->space[size_t(d)].min + a->space[size_t(d)].max) / 2); mb += std::llabs((b->space[size_t(d)].min + b->space[size_t(d)].max) / 2); }
                return ma < mb;
            });
            return subtree(children);
        }
        int64_t min_cost = INT64_MAX;
        int min_dim = -1;
        std::vector<Node*> min_buckets;
        for (int d = 0; d < dimensions; d++) {
            sort_nodes(children, dimensions, d, false);
            std::vector<Node*> buckets = bucketize(children);
            int64_t total = 0;
            for (Node* b : buckets) total += cost(b->space);
            if (min_cost > total) { min_cost = total; min_dim = d; min_buckets = buckets; }
        }
        sort_nodes(min_buckets, dimensions, min_dim, true);
        std::vector<Node*> out;
        for (Node* b : min_buckets) out.push_back(build(dimensions, b->children));
        return subtree(out);
    }
};

ParameterList::ParameterList(const std::vector<ParameterPoint>& points, const std::vector<int>& values) : points_(points), values_(values) {
    std::vector<Node*> leaves;
    for (size_t i = 0; i < points.size(); i++) {
        auto n = std::make_unique<Node>();
        n->space = points[i].space();
        n->leaf_index = int(i);
        leaves.push_back(n.get());
        pool_.push_back(std::move(n));
    }
    Builder b{*this};
    root_ = b.build(7, leaves);
}

ParameterList::~ParameterList() = default;

// recursion over the tree: smallest distance wins, ties keep the first found (strict improvement, like Mojang's search)
static const ParameterList::Node* search_node(const ParameterList::Node* n, const std::array<int64_t, 7>& target, const ParameterList::Node* candidate) {
    if (n->children.empty()) return n;
    int64_t min_distance = candidate ? candidate->distance(target) : INT64_MAX;
    const ParameterList::Node* closest = candidate;
    for (const ParameterList::Node* child : n->children) {
        int64_t cd = child->distance(target);
        if (min_distance > cd) {
            const ParameterList::Node* leaf = search_node(child, target, closest);
            int64_t ld = child == leaf ? cd : leaf->distance(target);
            if (min_distance > ld) { min_distance = ld; closest = leaf; }
        }
    }
    return closest;
}

int ParameterList::find_value(const TargetPoint& t) const {
    auto target = t.params();
    const Node* leaf = search_node(root_, target, nullptr);
    return values_[size_t(leaf->leaf_index)];
}

int ParameterList::find_value_brute_force(const TargetPoint& t) const {
    int best = 0;
    int64_t best_fit = INT64_MAX;
    auto target = t.params();
    for (size_t i = 0; i < points_.size(); i++) {
        auto s = points_[i].space();
        int64_t f = 0;
        for (int d = 0; d < 7; d++) { int64_t x = s[size_t(d)].distance(target[size_t(d)]); f += x * x; }
        if (f < best_fit) { best_fit = f; best = values_[i]; }
    }
    return best;
}

ClimateData load_climate_dump(const std::string& path, const std::string& preset) {
    ClimateData out;
    std::ifstream f(path);
    std::string line;
    std::vector<ParameterPoint> points;
    std::vector<int> values;
    std::map<std::string, int> index;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string p;
        is >> p;
        if (p != preset) continue;
        ParameterPoint pt;
        is >> pt.temperature.min >> pt.temperature.max >> pt.humidity.min >> pt.humidity.max >> pt.continentalness.min >> pt.continentalness.max >> pt.erosion.min >> pt.erosion.max >>
            pt.depth.min >> pt.depth.max >> pt.weirdness.min >> pt.weirdness.max >> pt.offset;
        std::string biome;
        is >> biome;
        auto it = index.find(biome);
        if (it == index.end()) { it = index.emplace(biome, int(out.biomes.size())).first; out.biomes.push_back(biome); }
        points.push_back(pt);
        values.push_back(it->second);
    }
    out.list = std::make_unique<ParameterList>(points, values);
    return out;
}

}  // namespace pn::vg
