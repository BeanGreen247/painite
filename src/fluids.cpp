// Water and lava: FlowingFluid's tick, spread and slope search, lava meeting water, buckets.
#include "blockstate.hpp"
#include "dimension.hpp"
#include "game.hpp"
#include "server.hpp"
#include "sim/inventory.hpp"
#include "sim/item.hpp"
#include "sim/player.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace pn {

namespace {

const int HX[4] = {0, 0, -1, 1}, HZ[4] = {-1, 1, 0, 0};

// what a block state holds as a fluid: kind 1 water, 2 lava; amount 1..8 (8 for sources and falling fluid)
struct FS {
    uint8_t kind = 0, amount = 0;
    bool falling = false, source = false;
};

const std::vector<FS>& fs_table() {
    static std::vector<FS> t;
    static std::once_flag once;
    std::call_once(once, [] {
        Blocks& B = Blocks::get();
        t.resize(B.count());
        for (size_t i = 0; i < t.size(); i++) {
            const BlockInfo& bi = B.info(uint16_t(i));
            FS s;
            if (bi.name == "minecraft:water" || bi.name == "minecraft:lava") {
                s.kind = bi.name == "minecraft:water" ? 1 : 2;
                int level = block_int(uint16_t(i), "level", 0);
                s.source = level == 0;
                s.falling = level >= 8;
                s.amount = uint8_t(level == 0 || level >= 8 ? 8 : 8 - level);
            } else if (bi.fluid && bi.props.find("waterlogged=false") == std::string::npos) {   // waterlogged blocks, kelp, seagrass, bubble columns hold a water source
                s.kind = 1; s.source = true; s.amount = 8;
            }
            t[i] = s;
        }
    });
    return t;
}
const FS& fs_of(uint16_t id) { const auto& t = fs_table(); return t[id < t.size() ? id : 0]; }

uint16_t fluid_block(int kind, const FS& s) {
    int level = s.source ? 0 : s.falling ? 8 : 8 - s.amount;
    return Blocks::get().find(kind == 1 ? "minecraft:water" : "minecraft:lava", "level=" + std::to_string(level));
}
uint16_t source_block(int kind) { return fluid_block(kind, FS{uint8_t(kind), 8, false, true}); }

struct FW {
    Server& s;
    int dim;
    World& w;
    FW(Server& srv, int d) : s(srv), dim(d), w(srv.dim(d)) {}
    uint16_t at(int x, int y, int z) {
        if (y < kMinY || y >= kMinY + kHeight) return 0;
        auto ch = w.chunk_if_loaded(x >> 4, z >> 4);
        if (!ch) return 0;
        std::lock_guard<std::mutex> l(ch->mu);
        return ch->get(x & 15, y, z & 15);
    }
    bool put(int x, int y, int z, uint16_t id) {
        if (y < kMinY || y >= kMinY + kHeight || !w.chunk_if_loaded(x >> 4, z >> 4)) return false;
        if (!w.set_block(x, y, z, id)) return false;
        s.game.block_changed(dim, x, y, z, id);
        return true;
    }
};

int spread_delay(int kind, int dim) { return kind == 1 ? 5 : dim == 1 ? 10 : 30; }

struct Flow {
    FW& f;
    int kind;
    int dim;
    int drop_off() const { return kind == 1 ? 1 : dim == 1 ? 1 : 2; }
    int slope_find() const { return kind == 1 ? 4 : dim == 1 ? 4 : 2; }

    // FlowingFluid.canHoldFluid: air, plants and fluids; not solid blocks, doors, signs, ladders, cane, portals
    bool holds(uint16_t id) {
        const BlockInfo& bi = Blocks::get().info(id);
        if (bi.air || bi.name == "minecraft:water" || bi.name == "minecraft:lava") return true;
        if (bi.fluid || bi.solid) return false;
        static const char* const no[] = {"sign", "portal", "ladder", "sugar_cane", "structure_void", "bubble", "minecraft:light"};
        for (const char* n : no) if (bi.name.find(n) != std::string::npos) return false;
        return true;
    }
    bool is_source(uint16_t id) { const FS& s = fs_of(id); return s.kind == kind && s.source; }
    // isWaterHole: the block below is this fluid or can take it
    bool hole(int x, int y, int z) {
        uint16_t below = f.at(x, y - 1, z);
        return fs_of(below).kind == kind || holds(below);
    }
    // getNewLiquid: what a spot becomes from its neighbours (amount 0 when dry)
    FS new_liquid(int x, int y, int z) {
        int highest = 0, sources = 0;
        for (int d = 0; d < 4; d++) {
            const FS& n = fs_of(f.at(x + HX[d], y, z + HZ[d]));
            if (n.kind != kind) continue;
            if (n.source) sources++;
            highest = std::max<int>(highest, n.amount);
        }
        if (kind == 1 && sources >= 2) {
            uint16_t below = f.at(x, y - 1, z);
            if (Blocks::get().info(below).solid || is_source(below)) return FS{1, 8, false, true};
        }
        if (fs_of(f.at(x, y + 1, z)).kind == kind) return FS{uint8_t(kind), 8, true, false};
        int amount = highest - drop_off();
        if (amount <= 0) return FS{};
        return FS{uint8_t(kind), uint8_t(amount), false, false};
    }
    int slope_distance(int x, int y, int z, int depth, int from) {
        int lowest = 1000;
        for (int d = 0; d < 4; d++) {
            if (d == from) continue;
            int nx = x + HX[d], nz = z + HZ[d];
            uint16_t n = f.at(nx, y, nz);
            if (!holds(n) || is_source(n)) continue;
            if (hole(nx, y, nz)) return depth;
            if (depth < slope_find()) lowest = std::min(lowest, slope_distance(nx, y, nz, depth + 1, d ^ 1));
        }
        return lowest;
    }
    bool can_spread_to(int x, int y, int z, bool down, const FS& nf) {
        uint16_t t = f.at(x, y, z);
        if (!holds(t)) return false;
        const FS& ts = fs_of(t);
        if (ts.kind == 0) return true;
        if (ts.kind == kind) return false;
        if (kind == 1) return ts.amount >= 4;   // water replaces lava that is at least 4/9 high
        return down;                              // lava onto water: stone
        (void)nf;
    }
    void spread_to(int x, int y, int z, bool down, const FS& nf) {
        uint16_t old = f.at(x, y, z);
        const BlockInfo& bi = Blocks::get().info(old);
        if (kind == 2 && down && fs_of(old).kind == 1) { f.put(x, y, z, Blocks::get().default_state("minecraft:stone")); return; }
        if (!bi.air && !bi.fluid) f.s.game.drop_block_loot(dim, x, y, z, old);
        f.put(x, y, z, fluid_block(kind, nf));
    }
    void spread_sides(int x, int y, int z, const FS& st) {
        int amount = st.falling ? 7 : st.amount - drop_off();
        if (amount <= 0) return;
        int lowest = 1000;
        int pick[4], n = 0;
        for (int d = 0; d < 4; d++) {
            int nx = x + HX[d], nz = z + HZ[d];
            uint16_t t = f.at(nx, y, nz);
            if (!holds(t) || is_source(t)) continue;
            FS nl = new_liquid(nx, y, nz);
            (void)nl;
            int dist = hole(nx, y, nz) ? 0 : slope_distance(nx, y, nz, 1, d ^ 1);
            if (dist < lowest) { n = 0; }
            if (dist <= lowest) { pick[n++] = d; lowest = dist; }
        }
        FS nf{uint8_t(kind), uint8_t(amount), false, false};
        for (int i = 0; i < n; i++) {
            int nx = x + HX[pick[i]], nz = z + HZ[pick[i]];
            if (can_spread_to(nx, y, nz, false, nf)) spread_to(nx, y, nz, false, nf);
        }
    }
    int source_neighbours(int x, int y, int z) {
        int c = 0;
        for (int d = 0; d < 4; d++) if (is_source(f.at(x + HX[d], y, z + HZ[d]))) c++;
        return c;
    }
    void spread(int x, int y, int z, const FS& st) {
        FS below = new_liquid(x, y - 1, z);
        if (below.kind && can_spread_to(x, y - 1, z, true, below)) {
            spread_to(x, y - 1, z, true, below);
            if (source_neighbours(x, y, z) >= 3) spread_sides(x, y, z, st);
        } else if (st.source || !hole(x, y, z)) spread_sides(x, y, z, st);
    }
};

// LiquidBlock.shouldSpreadLiquid: lava next to water turns to obsidian (source) or cobblestone; soul soil under lava turns blue ice into basalt
bool lava_reacts(FW& f, int x, int y, int z, uint16_t id) {
    static const int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
    Blocks& B = Blocks::get();
    bool soul = B.info(f.at(x, y - 1, z)).name == "minecraft:soul_soil";
    for (int d = 0; d < 6; d++) {
        uint16_t n = f.at(x + DX[d], y + DY[d], z + DZ[d]);
        if (fs_of(n).kind == 1) {
            f.put(x, y, z, B.default_state(fs_of(id).source ? "minecraft:obsidian" : "minecraft:cobblestone"));
            return true;
        }
        if (soul && B.info(n).name == "minecraft:blue_ice") {
            f.put(x, y, z, B.default_state("minecraft:basalt"));
            return true;
        }
    }
    return false;
}

// the first block along the view ray that a bucket works on (full: any block that is not a fluid; empty: also source fluids); `before` is the cell in front of it
bool bucket_ray(FW& f, double ex, double ey, double ez, double dx, double dy, double dz, bool want_source, int& hx, int& hy, int& hz, int& bx, int& by, int& bz) {
    Blocks& B = Blocks::get();
    int px = INT32_MIN, py = 0, pz = 0;
    for (double t = 0; t <= 4.5; t += 0.02) {
        int x = int(std::floor(ex + dx * t)), y = int(std::floor(ey + dy * t)), z = int(std::floor(ez + dz * t));
        if (x == px && y == py && z == pz) continue;
        uint16_t id = f.at(x, y, z);
        const BlockInfo& bi = B.info(id);
        bool fluid_block = bi.name == "minecraft:water" || bi.name == "minecraft:lava";
        bool hit = want_source ? ((fs_of(id).source && (fluid_block || bi.props.find("waterlogged=true") != std::string::npos)) || (!bi.air && !fluid_block && bi.solid) || bi.name == "minecraft:powder_snow")
                               : (!bi.air && !fluid_block);
        if (hit) { hx = x; hy = y; hz = z; bx = px; by = py; bz = pz; return true; }
        px = x; py = y; pz = z;
    }
    return false;
}

}  // namespace

// A fluid block's turn (a neighbour changed, or the delay of its tick is up): settle its level from the neighbours, then flow on.
void Game::fluid_update(int dim, int x, int y, int z) {
    FW f(s_, dim);
    uint16_t id = f.at(x, y, z);
    const FS st = fs_of(id);
    uint64_t key = (uint64_t(uint32_t(x)) << 32) ^ (uint64_t(uint32_t(z)) << 8) ^ uint64_t(uint32_t(y + 64)) ^ (uint64_t(dim) << 62);
    if (!st.kind) { std::lock_guard<std::mutex> l(fluid_mu_); fluid_next_.erase(key); return; }
    {   // BubbleColumnBlock: water above soul sand or magma turns into a column that carries on upwards, a column with nothing under it turns back into water
        const std::string& nm = Blocks::get().info(id).name;
        bool col = nm == "minecraft:bubble_column";
        if (col || (nm == "minecraft:water" && st.source)) {
            uint16_t below = f.at(x, y - 1, z);
            const std::string& bn = Blocks::get().info(below).name;
            int drag = bn == "minecraft:soul_sand" ? 0 : bn == "minecraft:magma_block" ? 1 : bn == "minecraft:bubble_column" ? (block_flag(below, "drag") ? 1 : 0) : -1;
            if (drag >= 0) {
                uint16_t want = Blocks::get().find("minecraft:bubble_column", drag ? "drag=true" : "drag=false");
                if (want != id) { f.put(x, y, z, want); return; }
            } else if (col) { f.put(x, y, z, source_block(1)); return; }
        }
    }
    if (st.kind == 2 && lava_reacts(f, x, y, z, id)) { std::lock_guard<std::mutex> l(fluid_mu_); fluid_next_.erase(key); return; }
    int delay = spread_delay(st.kind, dim);
    {
        std::lock_guard<std::mutex> l(fluid_mu_);
        auto it = fluid_next_.find(key);
        if (it == fluid_next_.end()) {   // first seen two ticks after the change that woke it
            if (delay > 2) { fluid_next_[key] = game_time_ + delay - 2; ticker_.schedule(dim, x, y, z, delay - 2); return; }
        } else if (game_time_ < it->second) { ticker_.schedule(dim, x, y, z, int(it->second - game_time_)); return; }
        else fluid_next_.erase(it);
    }
    Flow fl{f, int(st.kind), dim};
    FS cur = st;
    if (!st.source) {
        FS nl = fl.new_liquid(x, y, z);
        if (!nl.kind) { f.put(x, y, z, 0); return; }
        if (nl.source != st.source || nl.falling != st.falling || nl.amount != st.amount) { f.put(x, y, z, fluid_block(st.kind, nl)); cur = nl; }
    }
    fl.spread(x, y, z, cur);
}

// the held bucket turns into `to` (not in creative)
void Game::swap_held_bucket(uint64_t gid, const char* to) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim || it->second.sim->has_infinite_materials()) return;
    sim::PlayerSim& pl = *it->second.sim;
    const sim::ItemType* t = sim::Items::get().by_name(to);
    if (!t) return;
    sim::ItemStack filled(t, 1);
    sim::ItemStack& h = pl.held_item();
    if (h.count() == 1) h = filled;
    else { h.shrink(1); pl.inv.add(filled); }
    pl.inventory_menu->broadcast_changes();
}

// CauldronInteraction: buckets fill an empty cauldron or empty a full one. True when the click was used.
bool Game::cauldron_use(uint64_t gid, int dim, int x, int y, int z, const std::string& held) {
    if (dim < 0 || dim > 2) return false;
    FW f(s_, dim);
    uint16_t id = f.at(x, y, z);
    const std::string& n = Blocks::get().info(id).name;
    if (n != "minecraft:cauldron" && n != "minecraft:water_cauldron" && n != "minecraft:lava_cauldron" && n != "minecraft:powder_snow_cauldron") return false;
    Blocks& B = Blocks::get();
    int level = block_int(id, "level", 3);
    bool empty = n == "minecraft:cauldron";
    if (held == "minecraft:water_bucket" && (empty || (n == "minecraft:water_cauldron" && level < 3))) {
        f.put(x, y, z, B.find("minecraft:water_cauldron", "level=3")); swap_held_bucket(gid, "minecraft:bucket"); return true;
    }
    if (held == "minecraft:lava_bucket" && empty) { f.put(x, y, z, B.default_state("minecraft:lava_cauldron")); swap_held_bucket(gid, "minecraft:bucket"); return true; }
    if (held == "minecraft:powder_snow_bucket" && (empty || (n == "minecraft:powder_snow_cauldron" && level < 3))) {
        f.put(x, y, z, B.find("minecraft:powder_snow_cauldron", "level=3")); swap_held_bucket(gid, "minecraft:bucket"); return true;
    }
    if (held == "minecraft:bucket" && !empty && (n == "minecraft:lava_cauldron" || level == 3)) {
        const char* to = n == "minecraft:water_cauldron" ? "minecraft:water_bucket" : n == "minecraft:lava_cauldron" ? "minecraft:lava_bucket" : "minecraft:powder_snow_bucket";
        f.put(x, y, z, B.default_state("minecraft:cauldron")); swap_held_bucket(gid, to); return true;
    }
    return !empty && (held.size() > 7 && held.compare(held.size() - 7, 7, "_bucket") == 0);   // a bucket that does nothing here still counts as used
}

// use_item with a bucket (game thread): scoop up a fluid source, or pour the fluid out where the player looks.
void Game::bucket_use(uint64_t gid) {
    std::string name;
    GPos pos;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end() || !it->second.sim || it->second.v.dead || it->second.gamemode == 3 || it->second.gamemode == 2) return;
        sim::ItemStack& h = it->second.sim->held_item();
        if (h.is_empty()) return;
        name = h.item()->name;
        pos = it->second.pos;
    }
    int pour = name == "minecraft:water_bucket" ? 1 : name == "minecraft:lava_bucket" ? 2 : name == "minecraft:powder_snow_bucket" ? 3 : 0;
    if (name != "minecraft:bucket" && !pour) return;
    if (pos.dim < 0 || pos.dim > 2) return;
    double yr = double(pos.yaw) * 0.017453292519943295, pr = double(pos.pitch) * 0.017453292519943295;
    double dx = -std::sin(yr) * std::cos(pr), dy = -std::sin(pr), dz = std::cos(yr) * std::cos(pr);
    FW f(s_, pos.dim);
    Blocks& B = Blocks::get();
    auto swap_item = [&](const char* to) { swap_held_bucket(gid, to); };
    int hx, hy, hz, bx, by, bz;
    if (!bucket_ray(f, pos.x, pos.y + 1.62, pos.z, dx, dy, dz, !pour, hx, hy, hz, bx, by, bz)) return;
    if (!pour) {
        uint16_t id = f.at(hx, hy, hz);
        const BlockInfo& bi = B.info(id);
        const FS& s = fs_of(id);
        if (bi.name == "minecraft:powder_snow") { f.put(hx, hy, hz, 0); swap_item("minecraft:powder_snow_bucket"); return; }
        if (!s.source) return;
        if (bi.name == "minecraft:water" || bi.name == "minecraft:lava") f.put(hx, hy, hz, 0);
        else if (bi.props.find("waterlogged=true") != std::string::npos) f.put(hx, hy, hz, block_with(id, "waterlogged", false));
        else return;
        swap_item(s.kind == 1 ? "minecraft:water_bucket" : "minecraft:lava_bucket");
        return;
    }
    int tx = hx, ty = hy, tz = hz;
    uint16_t hit = f.at(hx, hy, hz);
    if (!B.info(hit).replaceable && !B.info(hit).fluid) {
        if (bx == INT32_MIN) return;
        tx = bx; ty = by; tz = bz;
    }
    uint16_t cur = f.at(tx, ty, tz);
    const BlockInfo& ci = B.info(cur);
    if (pos.dim == 1 && pour == 1) { swap_item("minecraft:bucket"); return; }   // the water evaporates in the Nether
    bool waterloggable = pour == 1 && ci.props.find("waterlogged=false") != std::string::npos;
    if (!ci.air && !ci.replaceable && !waterloggable && !(ci.name == "minecraft:water" || ci.name == "minecraft:lava")) return;
    if (ci.solid && !waterloggable) return;
    if (pour == 3) {
        if (!ci.air && !ci.fluid) drop_block_loot(pos.dim, tx, ty, tz, cur);
        f.put(tx, ty, tz, B.default_state("minecraft:powder_snow"));
    } else if (waterloggable) f.put(tx, ty, tz, block_with(cur, "waterlogged", true));
    else {
        if (!ci.air && !ci.fluid) drop_block_loot(pos.dim, tx, ty, tz, cur);
        f.put(tx, ty, tz, source_block(pour));
    }
    swap_item("minecraft:bucket");
}

}  // namespace pn
