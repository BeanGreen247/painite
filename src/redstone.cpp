#include "redstone.hpp"
#include "blockstate.hpp"
#include "dimension.hpp"
#include "game.hpp"
#include "server.hpp"
#include "sim/data.hpp"
#include "sim/entity.hpp"
#include "sim/entity_manager.hpp"
#include <algorithm>
#include <cmath>
#include <functional>

namespace pn {

namespace {

// directions as the block faces number them: 0 down, 1 up, 2 north (z-1), 3 south (z+1), 4 west (x-1), 5 east (x+1)
const int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
const char* const DNAME[6] = {"down", "up", "north", "south", "west", "east"};
inline int opposite(int d) { return d ^ 1; }
int dir_of(const std::string& name) { for (int i = 0; i < 6; i++) if (name == DNAME[i]) return i; return -1; }

enum Kind : uint8_t { None, Wire, RedBlock, Lever, Button, Plate, Torch, WallTorch, Repeater, Lamp, Door, Trapdoor, Gate, Tnt, Piston, StickyPiston, PistonHead, Hopper, Dispenser, Dropper, Observer, Comparator, Daylight, Target, NoteBlock, WeightedPlate };

Kind kind_of(uint16_t id) {
    static const std::vector<uint8_t> table = [] {
        std::vector<uint8_t> t(Blocks::get().count(), None);
        for (size_t i = 0; i < t.size(); i++) {
            const std::string& n = Blocks::get().info(uint16_t(i)).name;
            auto ends = [&](const char* suffix) { size_t l = strlen(suffix); return n.size() >= l && n.compare(n.size() - l, l, suffix) == 0; };
            Kind k = None;
            if (n == "minecraft:redstone_wire") k = Wire;
            else if (n == "minecraft:redstone_block") k = RedBlock;
            else if (n == "minecraft:lever") k = Lever;
            else if (ends("_button")) k = Button;
            else if (ends("_pressure_plate") && n != "minecraft:heavy_weighted_pressure_plate" && n != "minecraft:light_weighted_pressure_plate") k = Plate;
            else if (n == "minecraft:redstone_torch") k = Torch;
            else if (n == "minecraft:redstone_wall_torch") k = WallTorch;
            else if (n == "minecraft:repeater") k = Repeater;
            else if (n == "minecraft:redstone_lamp") k = Lamp;
            else if (ends("_door")) k = Door;
            else if (ends("_trapdoor")) k = Trapdoor;
            else if (ends("_fence_gate")) k = Gate;
            else if (n == "minecraft:tnt") k = Tnt;
            else if (n == "minecraft:piston") k = Piston;
            else if (n == "minecraft:sticky_piston") k = StickyPiston;
            else if (n == "minecraft:piston_head") k = PistonHead;
            else if (n == "minecraft:hopper") k = Hopper;
            else if (n == "minecraft:dispenser") k = Dispenser;
            else if (n == "minecraft:dropper") k = Dropper;
            else if (n == "minecraft:observer") k = Observer;
            else if (n == "minecraft:comparator") k = Comparator;
            else if (n == "minecraft:daylight_detector") k = Daylight;
            else if (n == "minecraft:target") k = Target;
            else if (n == "minecraft:note_block") k = NoteBlock;
            else if (n == "minecraft:heavy_weighted_pressure_plate" || n == "minecraft:light_weighted_pressure_plate") k = WeightedPlate;
            t[i] = k;
        }
        return t;
    }();
    return id < table.size() ? Kind(table[id]) : None;
}

uint64_t pos_key(int dim, int x, int y, int z) {
    return (uint64_t(uint32_t(x)) << 32) ^ (uint64_t(uint32_t(z)) << 8) ^ uint64_t(uint32_t(y + 64)) ^ (uint64_t(dim) << 62);
}

bool is_iron(uint16_t id) { return block_name_has(id, "iron_"); }

}  // namespace

struct Redstone::Ctx {
    Redstone* rs = nullptr;
    Server& s;
    int dim;
    World& w;
    int64_t now;
    Ctx(Redstone* r, Server& srv, int d) : rs(r), s(srv), dim(d), w(srv.dim(d)), now(srv.game.game_time()) {}
    uint16_t at(int x, int y, int z) {
        auto ch = w.chunk_if_loaded(x >> 4, z >> 4);
        if (!ch) return 0;
        std::lock_guard<std::mutex> l(ch->mu);
        return ch->get(x & 15, y, z & 15);
    }
    bool loaded(int x, int z) { return bool(w.chunk_if_loaded(x >> 4, z >> 4)); }
    bool put(int x, int y, int z, uint16_t id) {
        if (y < kMinY || y >= kMinY + kHeight || !loaded(x, z)) return false;
        if (!w.set_block(x, y, z, id)) return false;
        s.game.block_changed(dim, x, y, z, id);
        return true;
    }
    void schedule(int x, int y, int z, int delay) { s.game.ticker().schedule(dim, x, y, z, delay); }
    // redstone conductors pass strong power on: full opaque blocks
    bool conductor(uint16_t id) {
        const BlockInfo& bi = Blocks::get().info(id);
        return bi.solid && !bi.fluid && bi.opacity >= 15 && sim::BlockCollisionShapes::get().is_full_block(id);
    }
    // a power change at a block must wake its neighbours and the neighbours of the conductors next to it
    void notify(int x, int y, int z) {
        for (int d = 0; d < 6; d++) {
            int nx = x + DX[d], ny = y + DY[d], nz = z + DZ[d];
            s.game.ticker().schedule(dim, nx, ny, nz, 0);
            if (conductor(at(nx, ny, nz)))
                for (int e = 0; e < 6; e++) s.game.ticker().schedule(dim, nx + DX[e], ny + DY[e], nz + DZ[e], 0);
        }
    }
    void put_notify(int x, int y, int z, uint16_t id) { if (put(x, y, z, id)) notify(x, y, z); }

    // what the block `id` at a source position gives to a receiver; d is the direction from the receiver to the source
    int comparator_output(uint64_t key, uint16_t id) {
        std::lock_guard<std::mutex> l(rs->mu_);
        auto it = rs->comp_out_.find(key);
        return it == rs->comp_out_.end() ? (block_flag(id, "powered") ? 15 : 0) : it->second;
    }
    int out_signal(uint16_t id, int d, bool strong, bool count_wires, int px = 0, int py = 0, int pz = 0) {
        switch (kind_of(id)) {
            case RedBlock: return strong ? 0 : 15;
            case Lever: case Button: {
                if (!block_flag(id, "powered")) return 0;
                if (!strong) return 15;
                std::string face = block_prop(id, "face");
                int attached = face == "floor" ? 1 : face == "ceiling" ? 0 : dir_of(block_prop(id, "facing"));
                return d == attached ? 15 : 0;
            }
            case Plate: return block_flag(id, "powered") && (!strong || d == 1) ? 15 : 0;
            case Torch: return block_flag(id, "lit") && (strong ? d == 0 : d != 1) ? 15 : 0;
            case WallTorch: return block_flag(id, "lit") && (strong ? d == 0 : dir_of(block_prop(id, "facing")) != d) ? 15 : 0;
            case Repeater: return block_flag(id, "powered") && dir_of(block_prop(id, "facing")) == d ? 15 : 0;
            case Observer: return block_flag(id, "powered") && dir_of(block_prop(id, "facing")) == d ? 15 : 0;
            case Comparator: return dir_of(block_prop(id, "facing")) == d ? comparator_output(pos_key(dim, px, py, pz), id) : 0;
            case Daylight: return block_int(id, "power");
            case Target: return strong ? 0 : block_int(id, "power");
            case WeightedPlate: return (!strong || d == 1) ? block_int(id, "power") : 0;
            case Wire: {
                if (!count_wires || d == 0) return 0;
                int power = block_int(id, "power");
                if (power == 0) return 0;
                if (d == 1) return power;
                static const char* const side_prop[6] = {"", "", "north", "south", "west", "east"};
                return block_prop(id, side_prop[opposite(d)]) != "none" ? power : 0;
            }
            default: return 0;
        }
    }
    // strong power into the conductor at (x, y, z)
    int direct_into(int x, int y, int z, bool count_wires) {
        int best = 0;
        for (int e = 0; e < 6 && best < 15; e++) best = std::max(best, out_signal(at(x + DX[e], y + DY[e], z + DZ[e]), e, true, count_wires, x + DX[e], y + DY[e], z + DZ[e]));
        return best;
    }
    // Level.getSignal: the power the block next to a receiver gives it
    int signal_at(int rx, int ry, int rz, int d, bool count_wires) {
        int nx = rx + DX[d], ny = ry + DY[d], nz = rz + DZ[d];
        uint16_t id = at(nx, ny, nz);
        int s = out_signal(id, d, false, count_wires, nx, ny, nz);
        if (conductor(id)) s = std::max(s, direct_into(nx, ny, nz, count_wires));
        return s;
    }
    bool has_signal(int x, int y, int z, int skip_dir = -1) {
        for (int d = 0; d < 6; d++) if (d != skip_dir && signal_at(x, y, z, d, true) > 0) return true;
        return false;
    }
    int base_input(int x, int y, int z) {
        int best = 0;
        for (int d = 0; d < 6 && best < 15; d++) best = std::max(best, signal_at(x, y, z, d, false));
        return best;
    }
};

// ---- dust

namespace {

// what a wire connects sideways to: other dust, aligned repeaters, and anything that is a power source
bool connects_to(uint16_t id, int dir) {
    switch (kind_of(id)) {
        case Wire: case RedBlock: case Lever: case Button: case Plate: case Torch: case WallTorch: return true;
        case Repeater: { int f = dir_of(block_prop(id, "facing")); return f == dir || f == opposite(dir); }
        default: return block_named(id, "minecraft:observer") ? dir_of(block_prop(id, "facing")) == dir : block_named(id, "minecraft:comparator") || block_named(id, "minecraft:target") || block_named(id, "minecraft:daylight_detector");
    }
}

}  // namespace

void Redstone::update_wire(Ctx& c, int x, int y, int z) {
    struct Node { int x, y, z; int power; int base; };
    std::vector<Node> nodes;
    std::unordered_map<uint64_t, int> index;
    auto key = [&](int px, int py, int pz) { return pos_key(0, px, py, pz); };
    auto add = [&](int px, int py, int pz) {
        if (kind_of(c.at(px, py, pz)) != Wire || index.count(key(px, py, pz)) || nodes.size() >= 3000) return;
        index[key(px, py, pz)] = int(nodes.size());
        nodes.push_back({px, py, pz, 0, 0});
    };
    add(x, y, z);
    for (size_t i = 0; i < nodes.size(); i++) {   // every wire that could be linked to this one, whatever side the link is judged from
        Node n = nodes[i];
        for (int h = 2; h < 6; h++) {
            int qx = n.x + DX[h], qz = n.z + DZ[h];
            add(qx, n.y, qz); add(qx, n.y + 1, qz); add(qx, n.y - 1, qz);
        }
    }
    for (Node& n : nodes) n.base = n.power = c.base_input(n.x, n.y, n.z);
    // from the wire's own side: which wires feed it
    auto feeds = [&](const Node& n, std::vector<int>& out) {
        out.clear();
        bool above_conductor = c.conductor(c.at(n.x, n.y + 1, n.z));
        for (int h = 2; h < 6; h++) {
            int qx = n.x + DX[h], qz = n.z + DZ[h];
            uint16_t side = c.at(qx, n.y, qz);
            auto it = index.find(key(qx, n.y, qz));
            if (it != index.end() && kind_of(side) == Wire) out.push_back(it->second);
            bool side_conductor = c.conductor(side);
            if (side_conductor && !above_conductor) { auto up = index.find(key(qx, n.y + 1, qz)); if (up != index.end()) out.push_back(up->second); }
            else if (!side_conductor) { auto dn = index.find(key(qx, n.y - 1, qz)); if (dn != index.end()) out.push_back(dn->second); }
        }
    };
    std::vector<std::vector<int>> adj(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) feeds(nodes[i], adj[i]);
    for (int round = 0; round < 17; round++) {   // power falls by one per wire, so 15 rounds settle any network
        bool changed = false;
        for (size_t i = 0; i < nodes.size(); i++)
            for (int m : adj[i]) if (nodes[size_t(m)].power - 1 > nodes[i].power) { nodes[i].power = nodes[size_t(m)].power - 1; changed = true; }
        if (!changed) break;
    }
    // the shape of each wire: up, side or none for the four directions
    for (Node& n : nodes) {
        uint16_t id = c.at(n.x, n.y, n.z);
        if (kind_of(id) != Wire) continue;
        static const char* const names[4] = {"north", "south", "west", "east"};
        int conn[4];
        bool above_conductor = c.conductor(c.at(n.x, n.y + 1, n.z));
        for (int i = 0; i < 4; i++) {
            int h = 2 + i, qx = n.x + DX[h], qz = n.z + DZ[h];
            uint16_t side = c.at(qx, n.y, qz);
            conn[i] = 0;   // 0 none, 1 side, 2 up
            if (!above_conductor && c.conductor(side) && kind_of(c.at(qx, n.y + 1, qz)) == Wire) conn[i] = 2;
            else if (connects_to(side, h)) conn[i] = 1;
            else if (!c.conductor(side) && kind_of(c.at(qx, n.y - 1, qz)) == Wire) conn[i] = 1;
        }
        bool ns = conn[0] || conn[1], ew = conn[2] || conn[3];
        if (!ns && !ew) { for (int i = 0; i < 4; i++) conn[i] = 1; }
        else if (!ew) { if (!conn[0]) conn[0] = 1; if (!conn[1]) conn[1] = 1; }
        else if (!ns) { if (!conn[2]) conn[2] = 1; if (!conn[3]) conn[3] = 1; }
        static const char* const val[3] = {"none", "side", "up"};
        std::string props = std::string("east=") + val[conn[3]] + ",north=" + val[conn[0]] + ",power=" + std::to_string(n.power) + ",south=" + val[conn[1]] + ",west=" + val[conn[2]];
        (void)names;
        uint16_t want = Blocks::get().find("minecraft:redstone_wire", props);
        if (want != id) c.put(n.x, n.y, n.z, want);
    }
}

// ---- the other parts

void Redstone::update_torch(Ctx& c, int x, int y, int z, uint16_t id) {
    int attached = kind_of(id) == WallTorch ? opposite(dir_of(block_prop(id, "facing"))) : 0;
    bool powered = c.signal_at(x, y, z, attached, true) > 0;
    bool lit = block_flag(id, "lit");
    uint64_t k = pos_key(c.dim, x, y, z);
    if (lit && powered) {   // a torch burns while the block it is on is not powered; turning off 8 times within 3 seconds burns it out for a while
        bool burnt;
        {
            std::lock_guard<std::mutex> l(mu_);
            auto& h = burnout_[k];
            h.push_back(c.now);
            h.erase(std::remove_if(h.begin(), h.end(), [&](int64_t t) { return t < c.now - 60; }), h.end());
            burnt = h.size() >= 8;
        }
        c.put_notify(x, y, z, block_with(id, "lit", false));
        if (burnt) c.schedule(x, y, z, 160);
    } else if (!lit && !powered) {
        {
            std::lock_guard<std::mutex> l(mu_);
            auto it = burnout_.find(k);
            if (it != burnout_.end()) {
                auto& h = it->second;
                h.erase(std::remove_if(h.begin(), h.end(), [&](int64_t t) { return t < c.now - 60; }), h.end());
                if (h.size() >= 8) { c.schedule(x, y, z, 20); return; }
                if (h.empty()) burnout_.erase(it);
            }
        }
        c.put_notify(x, y, z, block_with(id, "lit", true));
    }
}

void Redstone::update_repeater(Ctx& c, int x, int y, int z, uint16_t id) {
    int facing = dir_of(block_prop(id, "facing"));
    int in = c.signal_at(x, y, z, facing, true);
    if (in < 15) { uint16_t back = c.at(x + DX[facing], y + DY[facing], z + DZ[facing]); if (kind_of(back) == Wire) in = std::max(in, block_int(back, "power")); }
    bool want = in > 0, powered = block_flag(id, "powered");
    uint64_t k = pos_key(c.dim, x, y, z);
    if (want == powered) { std::lock_guard<std::mutex> l(mu_); pending_.erase(k); return; }
    int64_t due;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = pending_.find(k);
        if (it == pending_.end()) it = pending_.emplace(k, c.now + std::max(0, block_int(id, "delay", 1) * 2 - 2)).first;
        due = it->second;
        if (c.now >= due) pending_.erase(it);
    }
    if (c.now < due) { c.schedule(x, y, z, int(due - c.now)); return; }
    if (c.put(x, y, z, block_with(id, "powered", want))) { c.notify(x, y, z); c.notify(x + DX[opposite(facing)], y, z + DZ[opposite(facing)]); }
}

void Redstone::update_lamp(Ctx& c, int x, int y, int z, uint16_t id) {
    bool powered = c.has_signal(x, y, z), lit = block_flag(id, "lit");
    uint64_t k = pos_key(c.dim, x, y, z);
    if (powered == lit) { std::lock_guard<std::mutex> l(mu_); pending_.erase(k); return; }
    if (powered) { c.put(x, y, z, block_with(id, "lit", true)); return; }
    int64_t due;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = pending_.find(k);
        if (it == pending_.end()) it = pending_.emplace(k, c.now + 2).first;   // a lamp stays lit for a moment after the power goes
        due = it->second;
        if (c.now >= due) pending_.erase(it);
    }
    if (c.now < due) { c.schedule(x, y, z, int(due - c.now)); return; }
    c.put(x, y, z, block_with(id, "lit", false));
}

void Redstone::update_openable(Ctx& c, int x, int y, int z, uint16_t id) {
    bool door = kind_of(id) == Door;
    int other_y = y;
    uint16_t other = 0;
    if (door) { other_y = block_prop(id, "half") == "lower" ? y + 1 : y - 1; other = c.at(x, other_y, z); }
    bool powered = c.has_signal(x, y, z) || (door && kind_of(other) == Door && c.has_signal(x, other_y, z));
    if (powered == block_flag(id, "powered")) return;   // the door follows the power only when the power changes: a hand can still open and close it
    c.put(x, y, z, block_with(block_with(id, "powered", powered), "open", powered));
    if (door && kind_of(other) == Door) c.put(x, other_y, z, block_with(block_with(other, "powered", powered), "open", powered));
}

void Redstone::update_button(Ctx& c, int x, int y, int z, uint16_t id) {
    if (!block_flag(id, "powered")) return;
    bool wooden = !block_name_has(id, "stone");
    uint64_t k = pos_key(c.dim, x, y, z);
    int64_t due;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = pending_.find(k);
        if (it == pending_.end()) it = pending_.emplace(k, c.now + (wooden ? 30 : 20)).first;
        due = it->second;
        if (c.now >= due) pending_.erase(it);
    }
    if (c.now < due) { c.schedule(x, y, z, int(due - c.now)); return; }
    c.put_notify(x, y, z, block_with(id, "powered", false));
}

// ---- pistons

namespace {
bool movable(uint16_t id, bool& destroyed) {
    destroyed = false;
    const BlockInfo& bi = Blocks::get().info(id);
    if (bi.air) return true;
    const sim::BlockProps* bp = sim::BlockTable::get().find(bi.name);
    if (!bp) return false;
    if (bp->push_reaction == "POPPED") { destroyed = true; return true; }   // plants, torches ... are destroyed by a piston
    if (bp->push_reaction == "IMMOVEABLE" || bp->has_block_entity || bp->destroy_time < 0) return false;
    if ((kind_of(id) == Piston || kind_of(id) == StickyPiston) && block_flag(id, "extended")) return false;
    if (kind_of(id) == PistonHead) return false;
    return true;
}
}  // namespace

// PistonStructureResolver: what moves along when a piston pushes (move = direction of the movement) the block at (sx, sy, sz). Slime and honey blocks carry
// what touches them (but not each other); at most 12 blocks. Plants and the like in the way are destroyed instead of moved.
bool Redstone::push_line(Ctx& c, int px, int py, int pz, int sx, int sy, int sz, int move, std::vector<Pos>& moved, std::vector<Pos>& destroyed) {
    std::function<bool(int, int, int)> add = [&](int x, int y, int z) -> bool {
        if (y < kMinY || y >= kMinY + kHeight || !c.loaded(x, z)) return false;
        if (x == px && y == py && z == pz) return true;
        uint16_t id = c.at(x, y, z);
        bool dest;
        if (!movable(id, dest)) return false;
        if (Blocks::get().info(id).air) return true;
        if (dest) { destroyed.push_back({x, y, z}); return true; }
        for (const Pos& m : moved) if (m.x == x && m.y == y && m.z == z) return true;
        moved.push_back({x, y, z});
        if (moved.size() > 12) return false;
        if (!add(x + DX[move], y + DY[move], z + DZ[move])) return false;   // whatever is ahead is pushed too
        const std::string& n = Blocks::get().info(id).name;
        bool slime = n == "minecraft:slime_block", honey = n == "minecraft:honey_block";
        if (slime || honey) {
            if (!add(x - DX[move], y - DY[move], z - DZ[move])) { /* the block behind only comes along when it can move */ }
            for (int d = 0; d < 6; d++) {
                if (d == move || d == opposite(move) || moved.size() > 12) continue;
                int nx = x + DX[d], ny = y + DY[d], nz = z + DZ[d];
                if (!c.loaded(nx, nz)) continue;
                uint16_t nid = c.at(nx, ny, nz);
                if (Blocks::get().info(nid).air) continue;
                const std::string& nn = Blocks::get().info(nid).name;
                if ((slime && nn == "minecraft:honey_block") || (honey && nn == "minecraft:slime_block")) continue;
                bool nd;
                if (!movable(nid, nd) || nd) continue;
                if (!add(nx, ny, nz)) return false;
            }
        }
        return true;
    };
    bool ok = add(sx, sy, sz);
    return ok && moved.size() <= 12;
}

void Redstone::extend_piston(Ctx& c, int x, int y, int z, uint16_t id) {
    int dir = dir_of(block_prop(id, "facing"));
    if (dir < 0) return;
    std::vector<Pos> moved, destroyed;
    if (!push_line(c, x, y, z, x + DX[dir], y + DY[dir], z + DZ[dir], dir, moved, destroyed)) return;
    for (const Pos& d : destroyed) c.put(d.x, d.y, d.z, 0);
    std::sort(moved.begin(), moved.end(), [&](const Pos& a, const Pos& b) { return a.x * DX[dir] + a.y * DY[dir] + a.z * DZ[dir] > b.x * DX[dir] + b.y * DY[dir] + b.z * DZ[dir]; });   // the far end first
    for (const Pos& m : moved) {
        uint16_t b = c.at(m.x, m.y, m.z);
        c.put(m.x, m.y, m.z, 0);
        c.put(m.x + DX[dir], m.y + DY[dir], m.z + DZ[dir], b);
    }
    bool sticky = kind_of(id) == StickyPiston;
    std::string props = std::string("facing=") + DNAME[dir] + ",short=false,type=" + (sticky ? "sticky" : "normal");
    c.put(x + DX[dir], y + DY[dir], z + DZ[dir], Blocks::get().find("minecraft:piston_head", props));
    c.put(x, y, z, block_with(id, "extended", true));
    c.notify(x, y, z);
}

void Redstone::retract_piston(Ctx& c, int x, int y, int z, uint16_t id) {
    int dir = dir_of(block_prop(id, "facing"));
    if (dir < 0) return;
    int hx = x + DX[dir], hy = y + DY[dir], hz = z + DZ[dir];
    uint16_t head = c.at(hx, hy, hz);
    if (kind_of(head) == PistonHead && dir_of(block_prop(head, "facing")) == dir) c.put(hx, hy, hz, 0);
    if (kind_of(id) == StickyPiston) {   // a sticky piston takes the block in front of its head back with it (and what sticks to that block)
        int px = hx + DX[dir], py = hy + DY[dir], pz = hz + DZ[dir];
        uint16_t pulled = c.at(px, py, pz);
        bool dest;
        if (!Blocks::get().info(pulled).air && movable(pulled, dest) && !dest && c.at(hx, hy, hz) == 0) {
            std::vector<Pos> moved, destroyed;
            int back = opposite(dir);
            if (push_line(c, x, y, z, px, py, pz, back, moved, destroyed) && !moved.empty()) {
                std::sort(moved.begin(), moved.end(), [&](const Pos& a, const Pos& b) { return a.x * DX[back] + a.y * DY[back] + a.z * DZ[back] > b.x * DX[back] + b.y * DY[back] + b.z * DZ[back]; });
                for (const Pos& d : destroyed) c.put(d.x, d.y, d.z, 0);
                for (const Pos& m : moved) {
                    uint16_t bl = c.at(m.x, m.y, m.z);
                    c.put(m.x, m.y, m.z, 0);
                    c.put(m.x + DX[back], m.y + DY[back], m.z + DZ[back], bl);
                }
            }
        }
    }
    c.put(x, y, z, block_with(id, "extended", false));
    c.notify(x, y, z);
}

void Redstone::update_piston(Ctx& c, int x, int y, int z, uint16_t id) {
    int dir = dir_of(block_prop(id, "facing"));
    bool powered = c.has_signal(x, y, z, dir), extended = block_flag(id, "extended");
    if (powered && !extended) extend_piston(c, x, y, z, id);
    else if (!powered && extended) retract_piston(c, x, y, z, id);
}

void Redstone::schedule_daylight(Ctx& c, int x, int y, int z) { c.schedule(x, y, z, 1); }

// ---- observers, comparators, daylight sensors, target blocks, note blocks

// an observer pulses when the block it faces changes
void Redstone::update_observer(Ctx& c, int x, int y, int z, uint16_t id) {
    int f = dir_of(block_prop(id, "facing"));
    if (f < 0) return;
    uint64_t k = pos_key(c.dim, x, y, z);
    uint16_t front = c.at(x + DX[f], y + DY[f], z + DZ[f]);
    bool powered = block_flag(id, "powered");
    int64_t due = -1;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto seen = observed_.find(k);
        if (seen == observed_.end()) observed_[k] = front;
        else if (seen->second != front) {
            seen->second = front;
            if (!powered && !pending_.count(k)) pending_[k] = c.now + 2;
        }
        if (powered && !pending_.count(k)) pending_[k] = c.now + 2;   // lit when loaded: turn it off again
        auto it = pending_.find(k);
        if (it != pending_.end()) due = it->second;
    }
    if (due < 0) return;
    if (c.now < due) { c.schedule(x, y, z, int(due - c.now)); return; }
    {
        std::lock_guard<std::mutex> l(mu_);
        if (powered) pending_.erase(k); else pending_[k] = c.now + 2;
    }
    if (c.put(x, y, z, block_with(id, "powered", !powered))) {
        c.notify(x, y, z);
        c.notify(x - DX[f], y - DY[f], z - DZ[f]);
        if (!powered) c.schedule(x, y, z, 2);
    }
}

// the analog signal of a block a comparator reads (containers by how full they are, cauldrons, cakes ...); -1 when it has none
int Redstone::analog_signal(Ctx& c, int x, int y, int z) {
    uint16_t id = c.at(x, y, z);
    const std::string& n = Blocks::get().info(id).name;
    if (n == "minecraft:cauldron") return 0;
    if (n == "minecraft:water_cauldron" || n == "minecraft:powder_snow_cauldron") return block_int(id, "level", 1);
    if (n == "minecraft:lava_cauldron") return 3;
    if (n == "minecraft:cake") return (7 - block_int(id, "bites")) * 2;
    if (n == "minecraft:end_portal_frame") return block_flag(id, "eye") ? 15 : 0;
    if (n == "minecraft:respawn_anchor") { int ch = block_int(id, "charges"); return ch == 0 ? 0 : (ch * 4 > 15 ? 15 : ch * 4); }
    if (n == "minecraft:beehive" || n == "minecraft:bee_nest") return block_int(id, "honey_level");
    if (n == "minecraft:composter") return block_int(id, "level");
    return s_.game.container_signal(c.dim, x, y, z);
}

int Redstone::comparator_signal(Ctx& c, int x, int y, int z, uint16_t id) {
    int f = dir_of(block_prop(id, "facing"));
    int rear = c.signal_at(x, y, z, f, true);
    if (rear < 15) { uint16_t back = c.at(x + DX[f], y + DY[f], z + DZ[f]); if (kind_of(back) == Wire) rear = std::max(rear, block_int(back, "power")); }
    int bx = x + DX[f], by = y + DY[f], bz = z + DZ[f];
    int a = analog_signal(c, bx, by, bz);
    if (a >= 0) rear = a;
    else if (rear < 15 && c.conductor(c.at(bx, by, bz))) {   // through one solid block
        int a2 = analog_signal(c, bx + DX[f], by + DY[f], bz + DZ[f]);
        if (a2 >= 0) rear = a2;
    }
    int left = (f == 2 || f == 3) ? 4 : 2, right = left ^ 1;   // the two sides of the facing
    int side = std::max(c.signal_at(x, y, z, left, true), c.signal_at(x, y, z, right, true));
    if (block_prop(id, "mode") == "subtract") return std::max(rear - side, 0);
    return rear >= side ? rear : 0;
}

void Redstone::update_comparator(Ctx& c, int x, int y, int z, uint16_t id) {
    uint64_t k = pos_key(c.dim, x, y, z);
    int want = comparator_signal(c, x, y, z, id);
    bool powered = block_flag(id, "powered");
    int cur;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = comp_out_.find(k);
        if (it == comp_out_.end()) { comp_out_[k] = powered ? want : 0; cur = comp_out_[k]; }
        else cur = it->second;
    }
    int64_t due;
    {
        std::lock_guard<std::mutex> l(mu_);
        bool differs = want != cur || (want > 0) != powered;
        if (!differs) { pending_.erase(k); return; }
        auto it = pending_.find(k);
        if (it == pending_.end()) it = pending_.emplace(k, c.now + 2).first;
        due = it->second;
        if (c.now >= due) pending_.erase(it);
    }
    if (c.now < due) { c.schedule(x, y, z, int(due - c.now)); return; }
    {
        std::lock_guard<std::mutex> l(mu_);
        comp_out_[k] = want;
    }
    int f = dir_of(block_prop(id, "facing"));
    if ((want > 0) != powered) c.put(x, y, z, block_with(id, "powered", want > 0));
    c.notify(x, y, z);
    c.notify(x - DX[f], y - DY[f], z - DZ[f]);
}

// the daylight sensor follows the sun
void Redstone::update_daylight(Ctx& c, int x, int y, int z, uint16_t id) {
    c.schedule(x, y, z, 20);
    int i = (c.loaded(x, z) && c.w.top_y(x, z) <= y) ? 15 : 0;   // the sensor itself is the highest solid block of a column in the open
    double t = double(s_.game.day_time() % 24000) / 24000.0 - 0.25;
    t -= std::floor(t);
    double d1 = 0.5 - std::cos(t * 3.14159265358979) / 2.0;
    double angle = (t + (d1 - t) / 3.0) * 2.0 * 3.14159265358979;   // Level.getSunAngle
    bool inverted = block_flag(id, "inverted");
    if (inverted) i = 15 - i;
    else if (i > 0) {
        double f1 = angle < 3.14159265358979 ? 0.0 : 2.0 * 3.14159265358979;
        angle += (f1 - angle) * 0.2;
        i = int(std::lround(double(i) * std::cos(angle)));
    }
    i = std::max(0, std::min(15, i));
    if (block_int(id, "power") != i) c.put_notify(x, y, z, block_with(id, "power", i));
}

// a target block lights up when something hits it and falls back after a while
void Redstone::target_hit(int dim, int x, int y, int z, double rx, double ry, double rz, int ticks) {
    Ctx c(this, s_, dim);
    uint16_t id = c.at(x, y, z);
    if (kind_of(id) != Target) return;
    double a[3] = {std::fabs(rx - 0.5), std::fabs(ry - 0.5), std::fabs(rz - 0.5)};
    std::sort(a, a + 3);
    double d = a[1];   // the hit axis sits at 0.5: the second largest is the distance from the middle of the face
    int power = std::max(1, std::min(15, int(std::ceil(15.0 * std::max(0.0, std::min(1.0, (0.5 - d) / 0.5))))));
    {
        std::lock_guard<std::mutex> l(mu_);
        pending_[pos_key(dim, x, y, z)] = c.now + ticks;
    }
    if (block_int(id, "power") != power) c.put_notify(x, y, z, block_with(id, "power", power));
    c.schedule(x, y, z, ticks);
}

void Redstone::update_target(Ctx& c, int x, int y, int z, uint16_t id) {
    if (block_int(id, "power") == 0) return;
    uint64_t k = pos_key(c.dim, x, y, z);
    int64_t due;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = pending_.find(k);
        if (it == pending_.end()) it = pending_.emplace(k, c.now + 8).first;
        due = it->second;
        if (c.now >= due) pending_.erase(it);
    }
    if (c.now < due) { c.schedule(x, y, z, int(due - c.now)); return; }
    c.put_notify(x, y, z, block_with(id, "power", 0));
}

// NoteBlockInstrument of the block under a note block
static const char* instrument_below(const std::string& n) {
    auto has = [&](const char* p) { return n.find(p) != std::string::npos; };
    if (has("glass") ) return "hat";
    if (n == "minecraft:sand" || n == "minecraft:red_sand" || n == "minecraft:gravel" || has("concrete_powder") || n == "minecraft:soul_sand") return n == "minecraft:soul_sand" ? "cow_bell" : "snare";
    if (has("_wool")) return "guitar";
    if (n == "minecraft:clay") return "flute";
    if (n == "minecraft:gold_block") return "bell";
    if (n == "minecraft:packed_ice") return "chime";
    if (n == "minecraft:bone_block") return "xylophone";
    if (n == "minecraft:iron_block") return "iron_xylophone";
    if (n == "minecraft:pumpkin" || n == "minecraft:carved_pumpkin" || n == "minecraft:jack_o_lantern") return "didgeridoo";
    if (n == "minecraft:emerald_block") return "bit";
    if (n == "minecraft:hay_block") return "banjo";
    if (n == "minecraft:glowstone") return "pling";
    if (has("planks") || has("_log") || has("_wood") || has("bookshelf") || has("chest") || has("crafting_table") || has("barrel") || has("jukebox") || has("note_block") || has("_door") || has("_fence") || has("_sign")) return "bass";
    if (has("stone") || has("cobble") || has("brick") || has("deepslate") || has("netherrack") || has("obsidian") || has("ore") || has("terracotta") || has("basalt") || has("blackstone") || has("andesite") || has("diorite") || has("granite") || has("end_stone") || has("prismarine") || has("quartz") || has("furnace") || has("dispenser") || has("dropper") || has("observer")) return "basedrum";
    return "harp";
}

void Redstone::play_note(Ctx& c, int x, int y, int z, uint16_t id) {
    if (!Blocks::get().info(c.at(x, y + 1, z)).air) return;
    std::string inst = block_prop(id, "instrument");
    int note = block_int(id, "note");
    float pitch = float(std::pow(2.0, double(note - 12) / 12.0));
    s_.game.play_sound(c.dim, x + 0.5, y + 0.5, z + 0.5, "minecraft:block.note_block." + (inst.empty() ? std::string("harp") : inst), 3.0f, pitch);
}

void Redstone::update_note(Ctx& c, int x, int y, int z, uint16_t id) {
    std::string want_inst = instrument_below(Blocks::get().info(c.at(x, y - 1, z)).name);
    uint16_t cur = id;
    if (block_prop(cur, "instrument") != want_inst) cur = block_with(cur, "instrument", want_inst);
    bool powered = c.has_signal(x, y, z);
    if (powered != block_flag(cur, "powered")) {
        cur = block_with(cur, "powered", powered);
        if (cur != id) c.put(x, y, z, cur);
        if (powered) play_note(c, x, y, z, cur);
        return;
    }
    if (cur != id) c.put(x, y, z, cur);
}

// ---- entry points

void Redstone::update(int dim, int x, int y, int z) {
    Ctx c(this, s_, dim);
    uint16_t id = c.at(x, y, z);
    switch (kind_of(id)) {
        case Wire: update_wire(c, x, y, z); break;
        case Torch: case WallTorch: update_torch(c, x, y, z, id); break;
        case Repeater: update_repeater(c, x, y, z, id); break;
        case Lamp: update_lamp(c, x, y, z, id); break;
        case Door: case Trapdoor: case Gate: update_openable(c, x, y, z, id); break;
        case Button: update_button(c, x, y, z, id); break;
        case Piston: case StickyPiston: update_piston(c, x, y, z, id); break;
        case Tnt:
            if (c.has_signal(x, y, z)) ignite_tnt(dim, x, y, z);
            break;
        case Observer: update_observer(c, x, y, z, id); break;
        case Comparator: update_comparator(c, x, y, z, id); break;
        case Daylight: update_daylight(c, x, y, z, id); break;
        case Target: update_target(c, x, y, z, id); break;
        case NoteBlock: update_note(c, x, y, z, id); break;
        case Hopper: {   // a powered hopper is switched off
            bool enabled = !c.has_signal(x, y, z);
            if (block_flag(id, "enabled") != enabled) c.put(x, y, z, block_with(id, "enabled", enabled));
            s_.game.hopper_register(dim, x, y, z);
            break;
        }
        case Dispenser: case Dropper: {   // a rising edge of power (the block or the one above it) fires it four ticks later
            bool powered = c.has_signal(x, y, z) || c.has_signal(x, y + 1, z), triggered = block_flag(id, "triggered");
            if (powered && !triggered) { c.put(x, y, z, block_with(id, "triggered", true)); s_.game.queue_dispense(dim, x, y, z, 4); }
            else if (!powered && triggered) c.put(x, y, z, block_with(id, "triggered", false));
            break;
        }
        default: break;
    }
}

bool Redstone::is_interactive(uint16_t id) {
    switch (kind_of(id)) {
        case Lever: case Button: case Gate: case Daylight: case NoteBlock: return true;
        case Door: case Trapdoor: return !is_iron(id);
        default: return false;
    }
}

bool Redstone::use(int dim, int x, int y, int z) {
    Ctx c(this, s_, dim);
    uint16_t id = c.at(x, y, z);
    switch (kind_of(id)) {
        case Lever: c.put_notify(x, y, z, block_with(id, "powered", !block_flag(id, "powered"))); return true;
        case Daylight: c.put(x, y, z, block_with(id, "inverted", !block_flag(id, "inverted"))); schedule_daylight(c, x, y, z); return true;
        case NoteBlock: {
            uint16_t n = block_with(id, "note", (block_int(id, "note") + 1) % 25);
            c.put(x, y, z, n);
            play_note(c, x, y, z, n);
            return true;
        }
        case Button: {
            if (block_flag(id, "powered")) return true;
            bool wooden = !block_name_has(id, "stone");
            c.put_notify(x, y, z, block_with(id, "powered", true));
            {
                std::lock_guard<std::mutex> l(mu_);
                pending_[pos_key(dim, x, y, z)] = c.now + (wooden ? 30 : 20);
            }
            c.schedule(x, y, z, wooden ? 30 : 20);
            return true;
        }
        case Door: case Trapdoor: case Gate: {
            if (is_iron(id)) return false;
            bool open = !block_flag(id, "open");
            c.put(x, y, z, block_with(id, "open", open));
            if (kind_of(id) == Door) {
                int oy = block_prop(id, "half") == "lower" ? y + 1 : y - 1;
                uint16_t other = c.at(x, oy, z);
                if (kind_of(other) == Door) c.put(x, oy, z, block_with(other, "open", open));
            }
            return true;
        }
        default: return false;
    }
}

bool Redstone::ignite_tnt(int dim, int x, int y, int z) {
    Ctx c(this, s_, dim);
    if (kind_of(c.at(x, y, z)) != Tnt) return false;
    if (!c.put(x, y, z, 0)) return false;
    s_.game.add_entity(dim, std::make_unique<sim::PrimedTnt>(nullptr, x + 0.5, y, z + 0.5));
    return true;
}

// pressed plates: powered while something stands on them and a moment longer
void Redstone::plates(const std::vector<PlateHit>& hits, int64_t tick) {
    std::vector<PlateState> release;
    std::vector<PlateHit> press;
    std::vector<PlateHit> valid;
    std::unordered_map<uint64_t, std::pair<PlateHit, int>> weighted;   // plate -> hit and how many things stand on it
    for (const PlateHit& h : hits) {   // a stone plate wants a mob or a player, a wooden one reacts to everything
        Ctx c(this, s_, h.dim);
        uint16_t id = c.at(h.x, h.y, h.z);
        if (kind_of(id) == WeightedPlate) { auto& w = weighted[pos_key(h.dim, h.x, h.y, h.z)]; w.first = h; w.second++; }
        else if (kind_of(id) == Plate && (h.living || !block_name_has(id, "stone"))) valid.push_back(h);
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        for (const PlateHit& h : valid) {
            uint64_t k = pos_key(h.dim, h.x, h.y, h.z);
            auto it = plates_.find(k);
            if (it == plates_.end()) { plates_[k] = {h.dim, h.x, h.y, h.z, tick + 20}; press.push_back(h); }
            else it->second.until = tick + 20;
        }
        for (auto& kv : weighted) {
            const PlateHit& h = kv.second.first;
            auto it = plates_.find(kv.first);
            if (it == plates_.end()) plates_[kv.first] = {h.dim, h.x, h.y, h.z, tick + 20};
            else it->second.until = tick + 20;
        }
        for (auto it = plates_.begin(); it != plates_.end();)
            if (it->second.until <= tick) { release.push_back(it->second); it = plates_.erase(it); } else ++it;
    }
    for (const PlateHit& h : press) {
        Ctx c(this, s_, h.dim);
        uint16_t id = c.at(h.x, h.y, h.z);
        if (kind_of(id) == Plate) c.put_notify(h.x, h.y, h.z, block_with(id, "powered", true));
    }
    for (auto& kv : weighted) {   // light plates give one level per thing (up to 15), heavy ones one per ten (up to 150)
        const PlateHit& h = kv.second.first;
        Ctx c(this, s_, h.dim);
        uint16_t id = c.at(h.x, h.y, h.z);
        if (kind_of(id) != WeightedPlate) continue;
        int n = kv.second.second;
        int power = block_name_has(id, "heavy") ? int(std::ceil(std::min(n, 150) / 10.0)) : std::min(n, 15);
        if (block_int(id, "power") != power) c.put_notify(h.x, h.y, h.z, block_with(id, "power", power));
    }
    for (const PlateState& p : release) {
        Ctx c(this, s_, p.dim);
        uint16_t id = c.at(p.x, p.y, p.z);
        if (kind_of(id) == Plate) c.put_notify(p.x, p.y, p.z, block_with(id, "powered", false));
        else if (kind_of(id) == WeightedPlate) c.put_notify(p.x, p.y, p.z, block_with(id, "power", 0));
    }
}

}  // namespace pn
