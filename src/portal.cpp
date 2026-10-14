// Nether portals (obsidian frames lit with flint and steel) and End portals: lighting, standing in them, finding or building the other side.
#include "dimension.hpp"
#include "game.hpp"
#include "log.hpp"
#include "server.hpp"
#include <algorithm>
#include <cmath>

namespace pn {

namespace {

struct PortalBlocks {
    uint16_t obsidian, portal_x, portal_z, fire, end_portal;
    PortalBlocks() {
        const Blocks& B = Blocks::get();
        obsidian = B.default_state("minecraft:obsidian");
        portal_x = B.find("minecraft:nether_portal", "axis=x");
        portal_z = B.find("minecraft:nether_portal", "axis=z");
        fire = B.default_state("minecraft:fire");
        end_portal = B.default_state("minecraft:end_portal");
    }
};
const PortalBlocks& pb() { static const PortalBlocks b; return b; }

bool is_fire(uint16_t id) { return id && Blocks::get().info(id).name == "minecraft:fire"; }

}  // namespace

// ---- lighting ---------------------------------------------------------------------------------------------

bool Game::portal_ignite(int dim, int x, int y, int z) {
    if (dim != 0 && dim != 1) return false;
    const PortalBlocks& P = pb();
    World& w = s_.dim(dim);
    const DimInfo& di = dim_info(dim);
    auto empty = [&](int px, int py, int pz) {
        if (py < di.min_y || py >= di.min_y + di.height) return false;
        uint16_t id = w.get_block(px, py, pz);
        return id == 0 || is_fire(id) || Blocks::get().info(id).replaceable;
    };
    auto obs = [&](int px, int py, int pz) { return py >= di.min_y && py < di.min_y + di.height && w.get_block(px, py, pz) == P.obsidian; };
    if (!empty(x, y, z)) return false;
    for (int axis = 0; axis < 2; axis++) {
        int dx = axis == 0 ? 1 : 0, dz = axis == 0 ? 0 : 1;
        int by = y;
        while (by > y - 21 && empty(x, by - 1, z)) by--;
        if (!obs(x, by - 1, z)) continue;
        int lx = x, lz = z;
        while (x - lx + z - lz < 21 && empty(lx - dx, by, lz - dz)) { lx -= dx; lz -= dz; }
        if (!obs(lx - dx, by, lz - dz)) continue;
        int width = 0;
        while (width < 22 && empty(lx + dx * width, by, lz + dz * width)) width++;
        if (width < 2 || width > 21 || !obs(lx + dx * width, by, lz + dz * width)) continue;
        int height = 0;
        bool ok = true;
        while (height < 22 && ok) {
            for (int i = 0; i < width && ok; i++) if (!empty(lx + dx * i, by + height, lz + dz * i)) ok = false;
            if (!ok) break;
            // both side columns of this row must be obsidian
            if (!obs(lx - dx, by + height, lz - dz) || !obs(lx + dx * width, by + height, lz + dz * width)) { ok = false; break; }
            height++;
        }
        if (height < 3 || height > 21) continue;
        // the row above must be the obsidian top, and the floor obsidian under every column
        bool frame = true;
        for (int i = 0; i < width && frame; i++) if (!obs(lx + dx * i, by + height, lz + dz * i) || !obs(lx + dx * i, by - 1, lz + dz * i)) frame = false;
        if (!frame) continue;
        uint16_t pid = axis == 0 ? P.portal_x : P.portal_z;
        for (int h = 0; h < height; h++)
            for (int i = 0; i < width; i++) {
                int px = lx + dx * i, pz = lz + dz * i, py = by + h;
                if (w.set_block(px, py, pz, pid)) block_changed(dim, px, py, pz, pid);
            }
        return true;
    }
    return false;
}

// ---- eyes of ender and the End portal ---------------------------------------------------------------------

bool Game::put_ender_eye(int dim, int x, int y, int z) {
    World& w = s_.dim(dim);
    const Blocks& B = Blocks::get();
    uint16_t cur = w.get_block(x, y, z);
    const BlockInfo& bi = B.info(cur);
    if (bi.name != "minecraft:end_portal_frame" || bi.props.find("eye=false") == std::string::npos) return false;
    std::string props = bi.props;
    props.replace(props.find("eye=false"), 9, "eye=true");
    uint16_t with_eye = B.find(bi.name, props);
    if (!with_eye || !w.set_block(x, y, z, with_eye)) return false;
    block_changed(dim, x, y, z, with_eye);
    auto eye = [&](int px, int pz) {
        const BlockInfo& f = B.info(w.get_block(px, y, pz));
        return f.name == "minecraft:end_portal_frame" && f.props.find("eye=true") != std::string::npos;
    };
    for (int ix = x - 3; ix <= x + 3; ix++)
        for (int iz = z - 3; iz <= z + 3; iz++) {
            bool ring = true;
            for (int i = -1; i <= 1 && ring; i++) ring = eye(ix + i, iz - 2) && eye(ix + i, iz + 2) && eye(ix - 2, iz + i) && eye(ix + 2, iz + i);
            if (!ring) continue;
            uint16_t portal = pb().end_portal;
            for (int px = ix - 1; px <= ix + 1; px++)
                for (int pz = iz - 1; pz <= iz + 1; pz++)
                    if (w.set_block(px, y, pz, portal)) block_changed(dim, px, y, pz, portal);
            return true;
        }
    return true;
}

// ---- finding and building the other side ----------------------------------------------------------------------

// Nearest portal block to (x, z) within `radius` blocks in `dim`, walked down to the bottom of its column. False when there is none.
bool Game::find_portal(int dim, int x, int z, int radius, int& ox, int& oy, int& oz) {
    const PortalBlocks& P = pb();
    World& w = s_.dim(dim);
    long best = -1;
    int cx0 = (x - radius) >> 4, cx1 = (x + radius) >> 4, cz0 = (z - radius) >> 4, cz1 = (z + radius) >> 4;
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++) {
            auto ch = w.chunk(cx, cz);
            std::lock_guard<std::mutex> l(ch->mu);
            for (int s = 0; s < kSections; s++) {
                if (!ch->sec[s]) continue;
                const uint16_t* b = ch->sec[s]->b;
                for (int i = 0; i < 4096; i++) {
                    if (b[i] != P.portal_x && b[i] != P.portal_z) continue;
                    int px = cx * 16 + (i & 15), pz = cz * 16 + ((i >> 4) & 15), py = kMinY + s * 16 + (i >> 8);
                    long d = static_cast<long>(px - x) * (px - x) + static_cast<long>(pz - z) * (pz - z);
                    if (d > static_cast<long>(radius) * radius) continue;
                    if (best < 0 || d < best || (d == best && py < oy)) { best = d; ox = px; oy = py; oz = pz; }
                }
            }
        }
    if (best < 0) return false;
    while (w.get_block(ox, oy - 1, oz) == P.portal_x || w.get_block(ox, oy - 1, oz) == P.portal_z) oy--;
    return true;
}

// Builds a 4x5 obsidian frame with a portal in it (axis x) standing on the ground at (x, y, z) = its lower left inside corner, with room around it.
void Game::build_portal(int dim, int x, int y, int z) {
    const PortalBlocks& P = pb();
    World& w = s_.dim(dim);
    auto put = [&](int px, int py, int pz, uint16_t id) { if (w.get_block(px, py, pz) != id && w.set_block(px, py, pz, id)) block_changed(dim, px, py, pz, id); };
    for (int dz = -1; dz <= 1; dz++)
        for (int px = x - 1; px <= x + 2; px++) {
            put(px, y - 1, z + dz, P.obsidian);
            for (int py = y; py <= y + 3; py++) put(px, py, z + dz, 0);
        }
    for (int px = x - 1; px <= x + 2; px++) { put(px, y - 1, z, P.obsidian); put(px, y + 3, z, P.obsidian); }
    for (int py = y; py <= y + 2; py++) { put(x - 1, py, z, P.obsidian); put(x + 2, py, z, P.obsidian); }
    for (int py = y; py <= y + 2; py++) for (int px = x; px <= x + 1; px++) put(px, py, z, P.portal_x);
}

// A good spot for a new portal near (x, z): open air above solid ground, found by scanning down columns around the target.
void Game::portal_site(int dim, int x, int z, int& oy, int& ox, int& oz) {
    World& w = s_.dim(dim);
    const Blocks& B = Blocks::get();
    const DimInfo& di = dim_info(dim);
    int top = dim == 0 ? 300 : 120, bottom = dim == 0 ? -60 : 34;
    auto solid = [&](int px, int py, int pz) { uint16_t id = w.get_block(px, py, pz); return id && B.info(id).solid && !B.info(id).replaceable; };
    auto clear = [&](int px, int py, int pz) { uint16_t id = w.get_block(px, py, pz); return id == 0 || B.info(id).replaceable || B.info(id).air; };
    for (int r = 0; r <= 16; r += 2)
        for (int ang = 0; ang < std::max(1, r * 2); ang++) {
            double a = ang * 6.2831853 / std::max(1, r * 2);
            int cx = x + static_cast<int>(std::lround(std::cos(a) * r)), cz = z + static_cast<int>(std::lround(std::sin(a) * r));
            int y0 = dim == 0 ? std::min(top, std::max(w.top_y(cx, cz), 62) + 1) : top;
            for (int y = y0; y >= bottom; y--) {
                if (!solid(cx, y - 1, cz) || !clear(cx, y, cz)) continue;
                bool room = true;
                for (int px = cx - 1; px <= cx + 2 && room; px++) for (int pz = cz - 1; pz <= cz + 1 && room; pz++) {
                    if (!solid(px, y - 1, pz)) room = false;
                    for (int py = y; py <= y + 3 && room; py++) if (!clear(px, py, pz) || (B.info(w.get_block(px, py, pz)).fluid)) room = false;
                }
                if (room) { ox = cx; oy = y; oz = cz; return; }
                if (dim == 0) break;   // overworld: only the surface counts
            }
        }
    // nothing suitable: carve the site out where the target is
    ox = x; oz = z;
    oy = dim == 0 ? std::max(di.min_y + 8, std::min(250, std::max(w.top_y(x, z), 62) + 1)) : 70;
}

// ---- standing in a portal ---------------------------------------------------------------------------------

void Game::portal_tick() {
    struct Who { uint64_t gid; GPos pos; int gm; };
    std::vector<Who> who;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& kv : players_) who.push_back({kv.first, kv.second.pos, kv.second.gamemode});
    }
    const PortalBlocks& P = pb();
    for (auto it = portal_state_.begin(); it != portal_state_.end();) {
        bool online = false;
        for (auto& wh : who) if (wh.gid == it->first) { online = true; break; }
        if (online) ++it; else it = portal_state_.erase(it);
    }
    for (auto& wh : who) {
        auto& st = portal_state_[wh.gid];
        if (st.cooldown > 0) st.cooldown--;
        if (wh.gm == 3) { st.ticks = 0; continue; }
        World& w = s_.dim(wh.pos.dim);
        int bx = static_cast<int>(std::floor(wh.pos.x)), by = static_cast<int>(std::floor(wh.pos.y + 0.1)), bz = static_cast<int>(std::floor(wh.pos.z));
        if (!w.chunk_if_loaded(bx >> 4, bz >> 4)) continue;
        uint16_t feet = w.get_block(bx, by, bz), head = w.get_block(bx, by + 1, bz);
        bool nether_portal = feet == P.portal_x || feet == P.portal_z || head == P.portal_x || head == P.portal_z;
        bool end_portal = feet == P.end_portal || w.get_block(bx, by - 1, bz) == P.end_portal;
        if (wh.pos.dim == 2 && wh.pos.y < -20) { portal_travel(wh.gid, wh.pos, 2); continue; }   // fell off the End: back to the overworld
        if (end_portal && st.cooldown == 0) { portal_travel(wh.gid, wh.pos, 2); st.cooldown = 40; st.ticks = 0; continue; }
        if (!nether_portal) { st.ticks = 0; continue; }
        if (st.cooldown > 0) { st.cooldown = 20; continue; }   // still inside the portal we arrived in: wait until we step out
        st.ticks++;
        if (st.ticks >= (wh.gm == 1 ? 1 : 80)) {
            st.ticks = 0;
            st.cooldown = 300;
            portal_travel(wh.gid, wh.pos, 1);
        }
    }
}

// kind 1: nether portal (overworld <-> nether), kind 2: end portal (overworld <-> the end)
void Game::portal_travel(uint64_t gid, const GPos& from, int kind) {
    GPos to = from;
    to.ground = true;
    if (kind == 1) {
        int dest = from.dim == 0 ? 1 : 0;
        double scale = from.dim == 0 ? 1.0 / 8 : 8.0;
        int tx = static_cast<int>(std::floor(from.x * scale)), tz = static_cast<int>(std::floor(from.z * scale));
        int px, py, pz;
        if (find_portal(dest, tx, tz, from.dim == 0 ? 16 : 128, px, py, pz)) {
            to.x = px + 0.5; to.y = py; to.z = pz + 0.5;
        } else {
            int sx, sy, sz;
            portal_site(dest, tx, tz, sy, sx, sz);
            build_portal(dest, sx, sy, sz);
            to.x = sx + 1.0; to.y = sy; to.z = sz + 0.5;
        }
        to.dim = dest;
    } else if (from.dim == 0) {
        World& w = s_.dim(2);
        const PortalBlocks& P = pb();
        for (int dx = 98; dx <= 102; dx++)
            for (int dz = -2; dz <= 2; dz++) {
                if (w.get_block(dx, 48, dz) != P.obsidian && w.set_block(dx, 48, dz, P.obsidian)) block_changed(2, dx, 48, dz, P.obsidian);
                for (int dy = 49; dy <= 52; dy++) if (w.get_block(dx, dy, dz) != 0 && w.set_block(dx, dy, dz, 0)) block_changed(2, dx, dy, dz, 0);
            }
        to.x = 100.5; to.y = 49; to.z = 0.5; to.dim = 2; to.yaw = 90; to.pitch = 0;
    } else {
        World& w = s_.world;
        int sx = w.spawn_x, sz = w.spawn_z;
        int top = w.top_y(sx, sz);
        to.x = sx + 0.5; to.y = (top >= kMinY ? top + 1 : w.spawn_y); to.z = sz + 0.5; to.dim = 0;
    }
    teleport(gid, to);
}

}  // namespace pn
