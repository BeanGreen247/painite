#include "path.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace pn::sim {

namespace {

struct Cell { int x, y, z; };

struct Search {
    EntityWorld& w;
    double hw, height;
    std::unordered_map<uint64_t, bool> clear_cache, stand_cache;
    Search(EntityWorld& world, double width, double h) : w(world), hw(std::min(width, 0.9) / 2.0 - 0.001), height(h - 0.002) {}

    static uint64_t key(int x, int y, int z) { return (uint64_t(uint32_t(x) & 0x3FFFFFF) << 38) | (uint64_t(uint32_t(z) & 0x3FFFFFF) << 12) | uint64_t(uint32_t(y + 2048) & 0xFFF); }

    // the mob's body fits with its feet on the floor of this cell
    bool clear(int x, int y, int z) {
        uint64_t k = key(x, y, z);
        auto it = clear_cache.find(k);
        if (it != clear_cache.end()) return it->second;
        bool ok = w.no_collision(AABB(x + 0.5 - hw, y + 0.0001, z + 0.5 - hw, x + 0.5 + hw, y + height, z + 0.5 + hw));
        clear_cache[k] = ok;
        return ok;
    }
    // clear, with something to stand on right below, and not standing in a fluid
    bool stand(int x, int y, int z) {
        uint64_t k = key(x, y, z);
        auto it = stand_cache.find(k);
        if (it != stand_cache.end()) return it->second;
        bool ok = clear(x, y, z);
        if (ok) {
            std::vector<AABB> below;
            w.block_collisions(AABB(x + 0.5 - 0.1, y - 0.3, z + 0.5 - 0.1, x + 0.5 + 0.1, y - 0.0001, z + 0.5 + 0.1), below);
            ok = !below.empty();
        }
        if (ok) ok = w.fluid_height(AABB(x + 0.2, y, z + 0.2, x + 0.8, y + 0.8, z + 0.8), true) == 0.0;   // never walk into lava
        stand_cache[k] = ok;
        return ok;
    }
};

struct Node { int x, y, z; float g; int parent; };

}  // namespace

std::vector<Vec3> find_path(EntityWorld& world, const Vec3& from, const Vec3& to, double width, double height, int max_nodes, int max_fall) {
    Search s(world, width, height);
    int sx = int(std::floor(from.x)), sy = int(std::floor(from.y + 0.001)), sz = int(std::floor(from.z));
    int gx = int(std::floor(to.x)), gy = int(std::floor(to.y + 0.001)), gz = int(std::floor(to.z));
    if (sx == gx && sy == gy && sz == gz) return {};
    std::vector<Node> nodes;
    nodes.push_back({sx, sy, sz, 0.0f, -1});
    std::unordered_map<uint64_t, float> best_g;
    best_g[Search::key(sx, sy, sz)] = 0.0f;
    auto heuristic = [&](int x, int y, int z) { double dx = x - gx, dy = y - gy, dz = z - gz; return float(std::sqrt(dx * dx + dy * dy * 0.5 + dz * dz)); };
    using Item = std::pair<float, int>;   // f, node index
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    open.push({heuristic(sx, sy, sz), 0});
    int closest = 0;
    float closest_h = heuristic(sx, sy, sz);
    int expanded = 0, found = -1;
    static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    while (!open.empty() && expanded < max_nodes) {
        int idx = open.top().second;
        open.pop();
        Node cur = nodes[size_t(idx)];
        if (std::abs(cur.x - gx) <= 0 && std::abs(cur.z - gz) <= 0 && std::abs(cur.y - gy) <= 1) { found = idx; break; }
        if (++expanded > max_nodes) break;
        for (auto& d : dirs) {
            int nx = cur.x + d[0], nz = cur.z + d[1];
            bool diagonal = d[0] != 0 && d[1] != 0;
            if (diagonal && (!s.clear(cur.x + d[0], cur.y, cur.z) || !s.clear(cur.x, cur.y, cur.z + d[1]))) continue;   // no cutting corners
            int ny = cur.y;
            bool ok = false;
            if (s.stand(nx, cur.y, nz)) { ok = true; }
            else if (s.stand(nx, cur.y + 1, nz) && s.clear(cur.x, cur.y + 1, cur.z)) { ny = cur.y + 1; ok = true; }   // a step up needs headroom here
            else if (s.clear(nx, cur.y, nz)) {   // a drop: walk off the edge and fall to the next floor
                for (int fall = 1; fall <= max_fall; ++fall)
                    if (s.stand(nx, cur.y - fall, nz)) { ny = cur.y - fall; ok = true; break; }
            }
            if (!ok) continue;
            float g = cur.g + (diagonal ? 1.4142f : 1.0f) + (ny != cur.y ? 0.3f : 0.0f);
            uint64_t k = Search::key(nx, ny, nz);
            auto it = best_g.find(k);
            if (it != best_g.end() && it->second <= g) continue;
            best_g[k] = g;
            nodes.push_back({nx, ny, nz, g, idx});
            float h = heuristic(nx, ny, nz);
            if (h < closest_h) { closest_h = h; closest = int(nodes.size()) - 1; }
            open.push({g + h, int(nodes.size()) - 1});
        }
    }
    int end = found >= 0 ? found : closest;
    if (end == 0) return {};
    std::vector<Vec3> path;
    for (int i = end; i > 0; i = nodes[size_t(i)].parent) path.push_back(Vec3(nodes[size_t(i)].x + 0.5, nodes[size_t(i)].y, nodes[size_t(i)].z + 0.5));
    std::reverse(path.begin(), path.end());
    return path;
}

}  // namespace pn::sim
