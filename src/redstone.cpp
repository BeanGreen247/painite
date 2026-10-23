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

namespace pn {

namespace {

// directions as the block faces number them: 0 down, 1 up, 2 north (z-1), 3 south (z+1), 4 west (x-1), 5 east (x+1)
const int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
const char* const DNAME[6] = {"down", "up", "north", "south", "west", "east"};
inline int opposite(int d) { return d ^ 1; }
int dir_of(const std::string& name) { for (int i = 0; i < 6; i++) if (name == DNAME[i]) return i; return -1; }

enum Kind : uint8_t { None, Wire, RedBlock, Lever, Button, Plate, Torch, WallTorch, Repeater, Lamp, Door, Trapdoor, Gate, Tnt, Piston, StickyPiston, PistonHead, Hopper, Dispenser, Dropper };

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
    Server& s;
    int dim;
    World& w;
    int64_t now;
    Ctx(Server& srv, int d) : s(srv), dim(d), w(srv.dim(d)), now(srv.game.game_time()) {}
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
    int out_signal(uint16_t id, int d, bool strong, bool count_wires) {
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
        for (int e = 0; e < 6 && best < 15; e++) best = std::max(best, out_signal(at(x + DX[e], y + DY[e], z + DZ[e]), e, true, count_wires));
        return best;
    }
    // Level.getSignal: the power the block next to a receiver gives it
    int signal_at(int rx, int ry, int rz, int d, bool count_wires) {
        int nx = rx + DX[d], ny = ry + DY[d], nz = rz + DZ[d];
        uint16_t id = at(nx, ny, nz);
        int s = out_signal(id, d, false, count_wires);
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
    if (lit == powered) c.put_notify(x, y, z, block_with(id, "lit", !powered));   // a torch burns while the block it is on is not powered
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

bool Redstone::push_line(Ctx& c, int x, int y, int z, int dir, std::vector<Pos>& moved, std::vector<Pos>& destroyed) {
    int cx = x + DX[dir], cy = y + DY[dir], cz = z + DZ[dir];
    for (int i = 0; i < 13; i++) {
        if (cy < kMinY || cy >= kMinY + kHeight || !c.loaded(cx, cz)) return false;
        uint16_t id = c.at(cx, cy, cz);
        bool dest;
        if (!movable(id, dest)) return false;
        if (Blocks::get().info(id).air) return true;
        if (dest) { destroyed.push_back({cx, cy, cz}); return true; }
        moved.push_back({cx, cy, cz});
        if (moved.size() > 12) return false;
        cx += DX[dir]; cy += DY[dir]; cz += DZ[dir];
    }
    return false;
}

void Redstone::extend_piston(Ctx& c, int x, int y, int z, uint16_t id) {
    int dir = dir_of(block_prop(id, "facing"));
    if (dir < 0) return;
    std::vector<Pos> moved, destroyed;
    if (!push_line(c, x, y, z, dir, moved, destroyed)) return;
    for (const Pos& d : destroyed) c.put(d.x, d.y, d.z, 0);
    for (size_t i = moved.size(); i-- > 0;) {   // the far end first
        uint16_t b = c.at(moved[i].x, moved[i].y, moved[i].z);
        c.put(moved[i].x + DX[dir], moved[i].y + DY[dir], moved[i].z + DZ[dir], b);
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
    if (kind_of(id) == StickyPiston) {   // a sticky piston takes the block in front of its head back with it
        int px = hx + DX[dir], py = hy + DY[dir], pz = hz + DZ[dir];
        uint16_t pulled = c.at(px, py, pz);
        bool dest;
        if (!Blocks::get().info(pulled).air && movable(pulled, dest) && !dest && c.at(hx, hy, hz) == 0) {
            c.put(hx, hy, hz, pulled);
            c.put(px, py, pz, 0);
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

// ---- entry points

void Redstone::update(int dim, int x, int y, int z) {
    Ctx c(s_, dim);
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
        case Lever: case Button: case Gate: return true;
        case Door: case Trapdoor: return !is_iron(id);
        default: return false;
    }
}

bool Redstone::use(int dim, int x, int y, int z) {
    Ctx c(s_, dim);
    uint16_t id = c.at(x, y, z);
    switch (kind_of(id)) {
        case Lever: c.put_notify(x, y, z, block_with(id, "powered", !block_flag(id, "powered"))); return true;
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
    Ctx c(s_, dim);
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
    for (const PlateHit& h : hits) {   // a stone plate wants a mob or a player, a wooden one reacts to everything
        Ctx c(s_, h.dim);
        uint16_t id = c.at(h.x, h.y, h.z);
        if (kind_of(id) == Plate && (h.living || !block_name_has(id, "stone"))) valid.push_back(h);
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        for (const PlateHit& h : valid) {
            uint64_t k = pos_key(h.dim, h.x, h.y, h.z);
            auto it = plates_.find(k);
            if (it == plates_.end()) { plates_[k] = {h.dim, h.x, h.y, h.z, tick + 20}; press.push_back(h); }
            else it->second.until = tick + 20;
        }
        for (auto it = plates_.begin(); it != plates_.end();)
            if (it->second.until <= tick) { release.push_back(it->second); it = plates_.erase(it); } else ++it;
    }
    for (const PlateHit& h : press) {
        Ctx c(s_, h.dim);
        uint16_t id = c.at(h.x, h.y, h.z);
        if (kind_of(id) == Plate) c.put_notify(h.x, h.y, h.z, block_with(id, "powered", true));
    }
    for (const PlateState& p : release) {
        Ctx c(s_, p.dim);
        uint16_t id = c.at(p.x, p.y, p.z);
        if (kind_of(id) == Plate) c.put_notify(p.x, p.y, p.z, block_with(id, "powered", false));
    }
}

}  // namespace pn
