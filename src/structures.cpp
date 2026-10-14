#include "structures.hpp"
#include "structures_util.hpp"
#include <cmath>

namespace pn {

struct StructureGen::Kind {
    const char* name;
    int dim;
    int spacing, separation;   // regions of `spacing` chunks hold at most one start, at most spacing - separation chunks from the region corner; 0 = fixed list
    uint32_t salt;
    int reach;                 // chunks around the start the structure may extend
};

static const StructureGen::Kind kKinds[] = {
    {"village", 0, 34, 8, 10387312, 8},
    {"desert_pyramid", 0, 32, 8, 14357617, 2},
    {"ruined_portal", 0, 40, 15, 34222645, 2},
    {"stronghold", 0, 0, 0, 0, 6},
    {"fortress", 1, 27, 4, 30084232, 9},
    {"end_city", 2, 20, 11, 10387313, 4},
};

static int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

StructureGen::StructureGen(const WorldGen& g, int dim) : g_(g), dim_(dim) {
    if (dim != 0) return;
    // strongholds: rings around the origin like vanilla's first three (3, 6 and 10 of them)
    uint64_t rs = hash2(g.seed(), 0x5472, 0x6f6e);
    static const int counts[3] = {3, 6, 10};
    for (int ring = 0; ring < 3; ring++) {
        double angle = rnd01(rs) * 6.283185307179586;
        for (int i = 0; i < counts[ring]; i++) {
            double dist = 4 * 32 + 32.0 * ring * 6 + (rnd01(rs) - 0.5) * 32 * 2.5;
            strongholds_.emplace_back(static_cast<int>(std::lround(std::cos(angle) * dist)), static_cast<int>(std::lround(std::sin(angle) * dist)));
            angle += 6.283185307179586 / counts[ring];
        }
    }
}

std::vector<std::string> StructureGen::kinds() const {
    std::vector<std::string> v;
    for (const Kind& k : kKinds) if (k.dim == dim_) v.push_back(k.name);
    return v;
}

bool StructureGen::start_of(const Kind& k, int rx, int rz, int& cx, int& cz) const {
    if (k.spacing <= 0) return false;
    uint64_t rs = hash2(g_.seed() ^ k.salt, rx, rz);
    cx = rx * k.spacing + rndi(rs, k.spacing - k.separation);
    cz = rz * k.spacing + rndi(rs, k.spacing - k.separation);
    return true;
}

std::shared_ptr<const Structure> StructureGen::build(const Kind& k, int cx, int cz) const {
    std::string n = k.name;
    std::unique_ptr<Structure> s;
    if (n == "village") s = build_village(g_, cx, cz);
    else if (n == "stronghold") s = build_stronghold(g_, cx, cz);
    else if (n == "desert_pyramid") s = build_desert_temple(g_, cx, cz);
    else if (n == "ruined_portal") s = build_ruined_portal(g_, 0, cx, cz);
    else if (n == "fortress") s = build_fortress(g_, cx, cz);
    else if (n == "end_city") s = build_end_city(g_, cx, cz);
    if (s) s->kind = n;
    return std::shared_ptr<const Structure>(std::move(s));
}

std::shared_ptr<const Structure> StructureGen::get(const Kind& k, int cx, int cz) const {
    uint64_t key = (static_cast<uint64_t>(&k - kKinds) << 58) ^ (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 28) ^ static_cast<uint32_t>(cz);
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
    }
    auto s = build(k, cx, cz);   // built outside the lock; two threads may build the same one, the results are equal
    std::lock_guard<std::mutex> l(mu_);
    if (cache_.size() > 600) cache_.clear();
    cache_[key] = s;
    return s;
}

namespace {
const std::vector<uint8_t>& soft_table() {
    static const std::vector<uint8_t> t = [] {
        const Blocks& B = Blocks::get();
        std::vector<uint8_t> v(B.count(), 0);
        for (size_t i = 0; i < v.size(); i++) {
            const BlockInfo& bi = B.info(static_cast<uint16_t>(i));
            bool leaves = bi.name.size() > 7 && bi.name.compare(bi.name.size() - 7, 7, "_leaves") == 0;
            v[i] = (bi.air || bi.fluid || bi.replaceable || bi.needs_floor || leaves) ? 1 : 0;
        }
        v[0] = 1;
        return v;
    }();
    return t;
}
}  // namespace

void StructureGen::paint(Chunk& c) const {
    const std::vector<uint8_t>& soft = soft_table();
    int x0 = c.x * 16, z0 = c.z * 16;
    auto set = [&](int x, int y, int z, uint16_t b) {
        if (y < kMinY || y >= kMinY + kHeight) return;
        int s = (y - kMinY) >> 4;
        if (!c.sec[s]) { c.sec[s] = std::make_unique<Section>(); memcpy(c.sec[s]->biome, c.air_biomes[s], 64); }
        c.sec[s]->b[((y & 15) << 8) | ((z - z0) << 4) | (x - x0)] = b;
    };
    auto is_soft = [&](uint16_t b) { return b < soft.size() && soft[b]; };
    auto paint_one = [&](const Structure& st) {
        if (st.maxx < x0 || st.minx > x0 + 15 || st.maxz < z0 || st.minz > z0 + 15) return;
        for (const StructPrim& p : st.prims) {
            int ax = std::max(p.x0, x0), bx = std::min(p.x1, x0 + 15), az = std::max(p.z0, z0), bz = std::min(p.z1, z0 + 15);
            if (ax > bx || az > bz) continue;
            int ay = std::max(p.y0, kMinY), by = std::min(p.y1, kMinY + kHeight - 1);
            switch (p.mode) {
                case P_FILL:
                    for (int y = ay; y <= by; y++) for (int z = az; z <= bz; z++) for (int x = ax; x <= bx; x++) set(x, y, z, p.block);
                    break;
                case P_SOFT:
                    for (int y = ay; y <= by; y++) for (int z = az; z <= bz; z++) for (int x = ax; x <= bx; x++)
                        if (is_soft(c.get(x - x0, y, z - z0))) set(x, y, z, p.block);
                    break;
                case P_FOUND:
                    for (int z = az; z <= bz; z++) for (int x = ax; x <= bx; x++)
                        for (int y = by; y >= ay; y--) {
                            uint16_t cur = c.get(x - x0, y, z - z0);
                            if (y != by && !is_soft(cur)) break;
                            set(x, y, z, p.block);
                        }
                    break;
                case P_SURFACE:
                    for (int z = az; z <= bz; z++) for (int x = ax; x <= bx; x++)
                        for (int y = by; y >= ay; y--) {
                            uint16_t cur = c.get(x - x0, y, z - z0);
                            if (is_soft(cur)) continue;
                            set(x, y, z, p.block);
                            if (is_soft(c.get(x - x0, y + 1, z - z0)) && Blocks::get().info(c.get(x - x0, y + 1, z - z0)).needs_floor) set(x, y + 1, z, 0);
                            break;
                        }
                    break;
            }
        }
    };
    for (const Kind& k : kKinds) {
        if (k.dim != dim_) continue;
        if (k.spacing == 0) {
            for (auto& sh : strongholds_)
                if (std::abs(sh.first - c.x) <= k.reach && std::abs(sh.second - c.z) <= k.reach)
                    if (auto st = get(k, sh.first, sh.second)) paint_one(*st);
            continue;
        }
        for (int rz = floor_div(c.z - k.reach, k.spacing); rz <= floor_div(c.z + k.reach, k.spacing); rz++)
            for (int rx = floor_div(c.x - k.reach, k.spacing); rx <= floor_div(c.x + k.reach, k.spacing); rx++) {
                int sx, sz;
                if (!start_of(k, rx, rz, sx, sz)) continue;
                if (std::abs(sx - c.x) > k.reach || std::abs(sz - c.z) > k.reach) continue;
                if (auto st = get(k, sx, sz)) paint_one(*st);
            }
    }
}

bool StructureGen::locate(const std::string& name, int x, int z, int& ox, int& oz) const {
    std::string n = name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
    for (const Kind& k : kKinds) {
        if (k.dim != dim_ || n != k.name) continue;
        long best = -1;
        auto consider = [&](int sx, int sz) {
            if (!get(k, sx, sz)) return;
            long dx = static_cast<long>(sx) * 16 + 8 - x, dz = static_cast<long>(sz) * 16 + 8 - z;
            long d = dx * dx + dz * dz;
            if (best < 0 || d < best) { best = d; ox = sx * 16 + 8; oz = sz * 16 + 8; }
        };
        if (k.spacing == 0) {
            for (auto& sh : strongholds_) consider(sh.first, sh.second);
            return best >= 0;
        }
        int rx0 = floor_div(x >> 4, k.spacing), rz0 = floor_div(z >> 4, k.spacing);
        for (int r = 0; r <= 8; r++) {
            for (int rz = rz0 - r; rz <= rz0 + r; rz++)
                for (int rx = rx0 - r; rx <= rx0 + r; rx++) {
                    if (std::max(std::abs(rx - rx0), std::abs(rz - rz0)) != r) continue;
                    int sx, sz;
                    if (start_of(k, rx, rz, sx, sz)) consider(sx, sz);
                }
            if (best >= 0 && r >= 1) return true;   // one more ring covers starts that are closer than the first hit
        }
        return best >= 0;
    }
    return false;
}

}  // namespace pn
