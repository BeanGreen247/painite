// Random block ticks (crops, grass, saplings, farmland, leaves, cane ...), leaf distance, fire, pressure plates under entities.
#include "blockstate.hpp"
#include "dimension.hpp"
#include "game.hpp"
#include "log.hpp"
#include "perf.hpp"
#include "server.hpp"
#include "sim/data.hpp"
#include "sim/loot.hpp"
#include <algorithm>
#include <cmath>
#include <random>

namespace pn {

namespace {

std::mt19937& rng() { thread_local std::mt19937 r{std::random_device{}()}; return r; }
int rnd(int n) { return n <= 1 ? 0 : int(rng()() % uint32_t(n)); }

const int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};

// the world as the block behaviours see it: chunks that are not loaded are air and cannot be changed
struct BW {
    Server& s;
    int dim;
    World& w;
    BW(Server& srv, int d) : s(srv), dim(d), w(srv.dim(d)) {}
    uint16_t at(int x, int y, int z) {
        auto ch = w.chunk_if_loaded(x >> 4, z >> 4);
        if (!ch) return 0;
        std::lock_guard<std::mutex> l(ch->mu);
        return ch->get(x & 15, y, z & 15);
    }
    bool loaded(int x, int z) { return bool(w.chunk_if_loaded(x >> 4, z >> 4)); }
    const BlockInfo& info(int x, int y, int z) { return Blocks::get().info(at(x, y, z)); }
    bool put(int x, int y, int z, uint16_t id) {
        if (y < kMinY || y >= kMinY + kHeight || !loaded(x, z)) return false;
        if (!w.set_block(x, y, z, id)) return false;
        s.game.block_changed(dim, x, y, z, id);
        return true;
    }
    bool air(int x, int y, int z) { return info(x, y, z).air; }
    // the highest solid block of a column; kept for the tick, the same columns are asked for again and again
    int top(int x, int z) {
        thread_local std::unordered_map<uint64_t, int> cache;
        thread_local int64_t cache_tick = -1;
        int64_t now = s.game.game_time();
        if (now != cache_tick) { cache.clear(); cache_tick = now; }
        uint64_t key = (uint64_t(uint32_t(x)) << 34) ^ (uint64_t(uint32_t(z)) << 2) ^ uint64_t(dim);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        return cache[key] = w.top_y(x, z);
    }
    bool sky(int x, int y, int z) { return loaded(x, z) && top(x, z) < y; }
    // an estimate of the light at a spot (there is no lighting engine): the sky when it is open, light sources within a few blocks.
    // `need` lets it stop as soon as the answer is known: most calls ask for 9 or 4 under an open daytime sky
    int light(int x, int y, int z, int need = 15) {
        int best = 0;
        if (sky(x, y, z)) {
            int64_t t = s.game.day_time() % 24000;
            best = (t < 12500 || t > 23500) ? 15 : 4;
            if (best >= need) return best;
        }
        for (int dx = -3; dx <= 3; dx++)
            for (int dy = -3 + std::abs(dx); dy <= 3 - std::abs(dx); dy++)
                for (int dz = -(3 - std::abs(dx) - std::abs(dy)); dz <= 3 - std::abs(dx) - std::abs(dy); dz++) {
                    int dist = std::abs(dx) + std::abs(dy) + std::abs(dz);
                    int emit = info(x + dx, y + dy, z + dz).emit;
                    if (emit > dist) { best = std::max(best, emit - dist); if (best >= need) return best; }
                }
        return best;
    }
};

bool opaque(const BlockInfo& b) { return b.solid && b.opacity >= 15; }

// FireBlock.getBurnOdds / getIgniteOdds for the blocks that burn: {ignite odds, burn odds}
bool flammability(const std::string& n, int& ignite, int& burn) {
    auto has = [&](const char* p) { return n.find(p) != std::string::npos; };
    static const char* const woods[] = {"oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "cherry", "bamboo"};
    bool wooden = false;
    for (const char* w : woods) if (n.find(std::string("minecraft:") + w + "_") == 0) wooden = true;
    if (has("planks") || (wooden && (has("_slab") || has("_fence") || has("_stairs")))) { ignite = 5; burn = 20; return true; }
    if ((wooden && (has("_log") || has("_wood"))) || (has("stripped_") && (has("_log") || has("_wood"))) || has("bamboo_block")) { ignite = 5; burn = 5; return true; }
    if (has("leaves")) { ignite = 30; burn = 60; return true; }
    if (has("_wool") || (has("_carpet") && !has("moss"))) { ignite = 30; burn = 60; return true; }
    if (n == "minecraft:hay_block" || n == "minecraft:dried_kelp_block") { ignite = 60; burn = 20; return true; }
    if (n == "minecraft:coal_block") { ignite = 5; burn = 5; return true; }
    if (n == "minecraft:tnt") { ignite = 15; burn = 100; return true; }
    if (n == "minecraft:bookshelf") { ignite = 30; burn = 20; return true; }
    if (has("vine") && !has("cave") && !has("weeping") && !has("twisting")) { ignite = 15; burn = 100; return true; }
    if (n == "minecraft:short_grass" || n == "minecraft:fern" || n == "minecraft:tall_grass" || n == "minecraft:large_fern" || has("flower") || n == "minecraft:dandelion" || n == "minecraft:poppy" || n == "minecraft:sunflower" || n == "minecraft:lilac" || n == "minecraft:rose_bush" || n == "minecraft:peony" || n == "minecraft:dead_bush") { ignite = 60; burn = 100; return true; }
    return false;
}

}  // namespace

// ---- items dropped by blocks that broke by themselves

void Game::drop_block_loot(int dim, int x, int y, int z, uint16_t state) {
    post_task([this, dim, x, y, z, state] {
        if (dim < 0 || dim > 2 || !entity_managers_[dim] || sim::Items::get().size() == 0) return;
        const BlockInfo& bi = Blocks::get().info(state);
        const sim::LootTable* table = sim::LootTables::get("minecraft:blocks/" + bi.name.substr(bi.name.find(':') + 1));
        if (!table) return;
        sim::LootBlockState bs;
        bs.name = bi.name;
        for (size_t p = 0; p < bi.props.size();) {
            size_t end = bi.props.find(',', p);
            if (end == std::string::npos) end = bi.props.size();
            size_t eq = bi.props.find('=', p);
            if (eq != std::string::npos && eq < end) bs.props[bi.props.substr(p, eq - p)] = bi.props.substr(eq + 1, end - eq - 1);
            p = end + 1;
        }
        sim::ItemStack none;
        sim::LootParams lp;
        lp.tool = &none;
        lp.block_state = &bs;
        lp.has_origin = true;
        lp.ox = x + 0.5; lp.oy = y + 0.5; lp.oz = z + 0.5;
        sim::EntityManager& mgr = *entity_managers_[dim];
        std::vector<sim::ItemStack> out;
        table->get_random_items(lp, mgr.random().next_long(), out);
        for (sim::ItemStack& st : out) {
            if (st.is_empty()) continue;
            auto item = std::make_unique<sim::ItemEntity>(&mgr, x + 0.5 + mgr.random().next_double() * 0.5 - 0.25, y + 0.5 - 0.125 + mgr.random().next_double() * 0.5 - 0.25, z + 0.5 + mgr.random().next_double() * 0.5 - 0.25, st);
            item->set_default_pickup_delay();
            mgr.add_entity(std::move(item));
        }
    });
}

// ---- leaves: distance to the nearest log, decay without one

void Game::leaves_update(int dim, int x, int y, int z) {
    BW b(s_, dim);
    uint16_t id = b.at(x, y, z);
    if (!block_name_has(id, "leaves")) return;
    int dist = 7;
    for (int d = 0; d < 6 && dist > 1; d++) {
        uint16_t n = b.at(x + DX[d], y + DY[d], z + DZ[d]);
        const std::string& name = Blocks::get().info(n).name;
        int nd = 7;
        if (name.find("_log") != std::string::npos || name.find("_wood") != std::string::npos || (name.find("_stem") != std::string::npos && name.find("nether") != std::string::npos)) nd = 0;
        else if (name.find("leaves") != std::string::npos) nd = block_int(n, "distance", 7);
        dist = std::min(dist, nd + 1);
    }
    dist = std::min(7, dist);
    if (block_int(id, "distance", 7) != dist) b.put(x, y, z, block_with(id, "distance", dist));
}

// ---- fire

namespace {
int burn_odds(BW& b, int x, int y, int z, bool want_ignite) {
    int ig = 0, bu = 0;
    if (!flammability(b.info(x, y, z).name, ig, bu)) return 0;
    return want_ignite ? ig : bu;
}
bool fire_can_survive(BW& b, int x, int y, int z) {
    const BlockInfo& below = b.info(x, y - 1, z);
    if (below.solid && !below.fluid) return true;
    for (int d = 0; d < 6; d++) if (burn_odds(b, x + DX[d], y + DY[d], z + DZ[d], true) > 0) return true;
    return false;
}
}  // namespace

bool Game::set_fire(int dim, int x, int y, int z) {
    BW b(s_, dim);
    uint16_t fire = Blocks::get().default_state("minecraft:fire");
    if (!fire || !b.air(x, y, z) || !fire_can_survive(b, x, y, z)) return false;
    if (!b.put(x, y, z, fire)) return false;
    ticker_.schedule(dim, x, y, z, 30 + rnd(10));
    std::lock_guard<std::mutex> l(fire_mu_);
    fire_next_[(uint64_t(uint32_t(x)) << 32) ^ (uint64_t(uint32_t(z)) << 8) ^ uint64_t(uint32_t(y + 64)) ^ (uint64_t(dim) << 62)] = game_time_ + 30 + rnd(10);
    return true;
}

void Game::fire_update(int dim, int x, int y, int z) {
    BW b(s_, dim);
    uint16_t id = b.at(x, y, z);
    if (!block_named(id, "minecraft:fire")) return;
    uint64_t key = (uint64_t(uint32_t(x)) << 32) ^ (uint64_t(uint32_t(z)) << 8) ^ uint64_t(uint32_t(y + 64)) ^ (uint64_t(dim) << 62);
    {
        std::lock_guard<std::mutex> l(fire_mu_);
        auto it = fire_next_.find(key);
        if (it == fire_next_.end()) it = fire_next_.emplace(key, game_time_ + 30 + rnd(10)).first;   // fire loaded from disk
        if (game_time_ < it->second) { ticker_.schedule(dim, x, y, z, int(it->second - game_time_)); return; }   // a neighbour changed, but it is not its turn
        it->second = game_time_ + 30 + rnd(10);
    }
    ticker_.schedule(dim, x, y, z, 30 + rnd(10));
    if (!fire_can_survive(b, x, y, z)) { b.put(x, y, z, 0); std::lock_guard<std::mutex> l(fire_mu_); fire_next_.erase(key); return; }
    int age = block_int(id, "age");
    const std::string& below = b.info(x, y - 1, z).name;
    bool infinite = below == "minecraft:netherrack" || below == "minecraft:magma_block";
    int new_age = std::min(15, age + rnd(3) / 2);
    if (new_age != age) { id = block_with(id, "age", new_age); b.put(x, y, z, id); age = new_age; }
    if (!infinite) {
        bool valid = false;
        for (int d = 0; d < 6; d++) if (burn_odds(b, x + DX[d], y + DY[d], z + DZ[d], true) > 0) valid = true;
        if (!valid) {
            if (!(b.info(x, y - 1, z).solid) || age > 3) { b.put(x, y, z, 0); std::lock_guard<std::mutex> l(fire_mu_); fire_next_.erase(key); return; }
        }
        if (age == 15 && rnd(4) == 0 && burn_odds(b, x, y - 1, z, true) == 0) { b.put(x, y, z, 0); std::lock_guard<std::mutex> l(fire_mu_); fire_next_.erase(key); return; }
    }
    int diff = s_.cfg.difficulty >= 0 ? s_.cfg.difficulty : s_.world.difficulty;
    auto check_burn_out = [&](int bx, int by, int bz, int chance, int fire_age) {   // FireBlock.checkBurnOut
        int odds = burn_odds(b, bx, by, bz, false);
        if (odds > 0 && rnd(chance) < odds) {
            uint16_t target = b.at(bx, by, bz);
            if (rnd(fire_age + 10) < 5) {
                uint16_t fire = Blocks::get().default_state("minecraft:fire");
                b.put(bx, by, bz, block_with(fire, "age", std::min(15, fire_age + rnd(5) / 4)));
                ticker_.schedule(dim, bx, by, bz, 30 + rnd(10));
            } else b.put(bx, by, bz, 0);
            if (block_named(target, "minecraft:tnt")) { b.put(bx, by, bz, 0); redstone().ignite_tnt(dim, bx, by, bz); }
        }
    };
    check_burn_out(x + 1, y, z, 300, age); check_burn_out(x - 1, y, z, 300, age);
    check_burn_out(x, y - 1, z, 250, age); check_burn_out(x, y + 1, z, 250, age);
    check_burn_out(x, y, z - 1, 300, age); check_burn_out(x, y, z + 1, 300, age);
    for (int ox = -1; ox <= 1; ox++)
        for (int oz = -1; oz <= 1; oz++)
            for (int oy = -1; oy <= 4; oy++) {
                if (!ox && !oy && !oz) continue;
                int k = 100 + (oy > 1 ? (oy - 1) * 100 : 0);
                int px = x + ox, py = y + oy, pz = z + oz;
                if (!b.air(px, py, pz)) continue;
                int odds = 0;
                for (int d = 0; d < 6; d++) odds = std::max(odds, burn_odds(b, px + DX[d], py + DY[d], pz + DZ[d], true));
                if (odds <= 0) continue;
                int o = (odds + 40 + diff * 7) / (age + 30);
                if (o > 0 && rnd(k) <= o) set_fire(dim, px, py, pz);
            }
}

// ---- trees

namespace {
void grow_tree(BW& b, int x, int y, int z, uint16_t sapling) {
    const std::string& sn = Blocks::get().info(sapling).name;
    std::string species = sn.substr(sn.find(':') + 1);
    species = species.substr(0, species.find("_sapling"));
    if (species == "dark_oak" || species == "mangrove" || species == "azalea" || species == "flowering_azalea") return;   // not grown yet
    std::string log_name = "minecraft:" + species + "_log", leaf_name = "minecraft:" + species + "_leaves";
    if (species == "cherry") { log_name = "minecraft:cherry_log"; leaf_name = "minecraft:cherry_leaves"; }
    uint16_t log = Blocks::get().default_state(log_name), leaf = Blocks::get().default_state(leaf_name);
    if (!log || !leaf) return;
    leaf = block_with(block_with(leaf, "persistent", false), "distance", 1);
    int height = species == "spruce" ? 6 + rnd(4) : species == "jungle" ? 5 + rnd(5) : species == "birch" ? 5 + rnd(3) : species == "acacia" ? 5 + rnd(2) : 4 + rnd(3);
    for (int i = 1; i <= height + 1; i++) {   // room for the trunk and the crown
        const BlockInfo& bi = b.info(x, y + i, z);
        if (!(bi.air || bi.replaceable || bi.name.find("leaves") != std::string::npos)) return;
    }
    auto leaf_at = [&](int px, int py, int pz) { const BlockInfo& bi = b.info(px, py, pz); if (bi.air || bi.replaceable) b.put(px, py, pz, leaf); };
    if (species == "spruce") {
        for (int layer = 0; layer < height - 1; layer++) {
            int ly = y + height - layer;
            int r = layer == 0 ? 0 : 1 + (layer % 3 == 0 ? 0 : layer % 3 == 1 ? 1 : 2) % 3;
            r = std::min(3, r);
            for (int dx = -r; dx <= r; dx++) for (int dz = -r; dz <= r; dz++) if (std::abs(dx) + std::abs(dz) <= r + (r > 1 ? 1 : 0) && !(dx == 0 && dz == 0 && ly <= y + height - 1)) leaf_at(x + dx, ly, z + dz);
        }
        leaf_at(x, y + height + 1, z);
    } else {
        int top = y + height;
        for (int dy = -2; dy <= 1; dy++) {
            int r = dy >= 0 ? 1 : 2;
            for (int dx = -r; dx <= r; dx++)
                for (int dz = -r; dz <= r; dz++) {
                    if (dx == 0 && dz == 0 && dy < 1) continue;   // the trunk
                    if (std::abs(dx) == r && std::abs(dz) == r && (dy >= 0 || rnd(2) == 0)) continue;   // rounded corners
                    leaf_at(x + dx, top + dy, z + dz);
                }
        }
    }
    for (int i = 0; i < height; i++) b.put(x, y + i, z, log);
}
}  // namespace

// ---- random ticks

void Game::block_random_tick(int dim, int x, int y, int z, uint16_t id) {
    BW b(s_, dim);
    const std::string& name = Blocks::get().info(id).name;
    auto is = [&](const char* n) { return name == n; };
    auto has = [&](const char* p) { return name.find(p) != std::string::npos; };
    if (is("minecraft:hopper")) { hopper_register(dim, x, y, z); return; }
    // ---- kelp grows upwards through water (GrowingPlantHeadBlock.randomTick)
    if (is("minecraft:kelp")) {
        int age = block_int(id, "age");
        if (age < 25 && rnd(100) < 14) {
            uint16_t above = b.at(x, y + 1, z);
            if (block_named(above, "minecraft:water") && block_int(above, "level", 1) == 0) {
                b.put(x, y + 1, z, block_with(id, "age", age + 1));
                b.put(x, y, z, Blocks::get().default_state("minecraft:kelp_plant"));
            }
        }
        return;
    }
    // ---- lava sets fire to what burns around it (LavaFluid.randomTick)
    if (is("minecraft:lava")) {
        int n = rnd(3);
        if (n > 0) {
            int fx = x, fy = y, fz = z;
            for (int j = 0; j < n; j++) {
                fx += rnd(3) - 1; fy += 1; fz += rnd(3) - 1;
                if (b.air(fx, fy, fz)) {
                    bool near = false;
                    for (int d = 0; d < 6 && !near; d++) near = burn_odds(b, fx + DX[d], fy + DY[d], fz + DZ[d], true) > 0;
                    if (near) { set_fire(dim, fx, fy, fz); return; }
                } else if (b.info(fx, fy, fz).solid) return;
            }
        } else {
            for (int j = 0; j < 3; j++) {
                int fx = x + rnd(3) - 1, fz = z + rnd(3) - 1;
                if (b.air(fx, y + 1, fz) && burn_odds(b, fx, y, fz, true) > 0) set_fire(dim, fx, y + 1, fz);
            }
        }
        return;
    }
    // ---- crops
    bool crop = is("minecraft:wheat") || is("minecraft:carrots") || is("minecraft:potatoes") || is("minecraft:beetroots") || is("minecraft:melon_stem") || is("minecraft:pumpkin_stem") ||
                is("minecraft:torchflower_crop") || is("minecraft:sweet_berry_bush");
    if (crop) {
        if (b.light(x, y + 1, z, 9) < 9) return;
        int age = block_int(id, "age"), max_age = is("minecraft:beetroots") || is("minecraft:sweet_berry_bush") ? 3 : 7;
        if (is("minecraft:torchflower_crop")) max_age = 1;
        if (age < max_age || has("stem")) {
            // CropBlock.getGrowthSpeed
            float f = 1.0f;
            for (int dx = -1; dx <= 1; dx++)
                for (int dz = -1; dz <= 1; dz++) {
                    float f1 = 0.0f;
                    uint16_t soil = b.at(x + dx, y - 1, z + dz);
                    if (block_named(soil, "minecraft:farmland")) { f1 = 1.0f; if (block_int(soil, "moisture") > 0) f1 = 3.0f; }
                    if (dx || dz) f1 /= 4.0f;
                    f += f1;
                }
            bool row = b.at(x - 1, y, z) == id || b.at(x + 1, y, z) == id, col = b.at(x, y, z - 1) == id || b.at(x, y, z + 1) == id;
            bool diag = b.at(x - 1, y, z - 1) == id || b.at(x + 1, y, z - 1) == id || b.at(x + 1, y, z + 1) == id || b.at(x - 1, y, z + 1) == id;
            if ((row && col) || diag) f /= 2.0f;
            if (rnd(int(25.0f / f) + 1) != 0) return;
            if (age < max_age) { b.put(x, y, z, block_with(id, "age", age + 1)); return; }
            if (has("stem")) {   // a ripe stem puts its fruit on a free spot next to it
                int dir = 2 + rnd(4);
                int fx = x + DX[dir], fz = z + DZ[dir];
                const std::string& soil = b.info(fx, y - 1, fz).name;
                bool ok = soil == "minecraft:farmland" || soil == "minecraft:dirt" || soil == "minecraft:grass_block" || soil == "minecraft:coarse_dirt" || soil == "minecraft:podzol" || soil == "minecraft:moss_block" || soil == "minecraft:mud" || soil == "minecraft:rooted_dirt";
                if (ok && b.air(fx, y, fz)) {
                    bool melon = is("minecraft:melon_stem");
                    b.put(fx, y, fz, Blocks::get().default_state(melon ? "minecraft:melon" : "minecraft:pumpkin"));
                    uint16_t att = Blocks::get().find(melon ? "minecraft:attached_melon_stem" : "minecraft:attached_pumpkin_stem", std::string("facing=") + (dir == 2 ? "north" : dir == 3 ? "south" : dir == 4 ? "west" : "east"));
                    if (att) b.put(x, y, z, att);
                }
            }
        }
        return;
    }
    if (is("minecraft:nether_wart")) {
        int age = block_int(id, "age");
        if (age < 3 && rnd(10) == 0) b.put(x, y, z, block_with(id, "age", age + 1));
        return;
    }
    // ---- sugar cane and cactus grow upwards to three high
    if (is("minecraft:sugar_cane") || is("minecraft:cactus")) {
        if (!b.air(x, y + 1, z)) return;
        int h = 1;
        while (h < 3 && block_named(b.at(x, y - h, z), name.c_str())) h++;
        if (h >= 3) return;
        int age = block_int(id, "age");
        if (age >= 15) { b.put(x, y + 1, z, Blocks::get().default_state(name)); b.put(x, y, z, block_with(id, "age", 0)); }
        else b.put(x, y, z, block_with(id, "age", age + 1));
        return;
    }
    // ---- saplings
    if (has("_sapling") && !has("potted")) {
        if (rnd(7) != 0 || b.light(x, y + 1, z, 9) < 9) return;
        if (block_int(id, "stage") == 0) b.put(x, y, z, block_with(id, "stage", 1));
        else grow_tree(b, x, y, z, id);
        return;
    }
    // ---- grass and mycelium spread, and die under a roof
    if (is("minecraft:grass_block") || is("minecraft:mycelium")) {
        const BlockInfo& above = b.info(x, y + 1, z);
        if (opaque(above) || above.fluid) { b.put(x, y, z, Blocks::get().default_state("minecraft:dirt")); return; }
        if (b.light(x, y + 1, z, 9) < 9) return;
        for (int i = 0; i < 4; i++) {
            int tx = x + rnd(3) - 1, ty = y + rnd(5) - 3, tz = z + rnd(3) - 1;
            if (!block_named(b.at(tx, ty, tz), "minecraft:dirt")) continue;
            const BlockInfo& ta = b.info(tx, ty + 1, tz);
            if (opaque(ta) || ta.fluid || b.light(tx, ty + 1, tz, 4) < 4) continue;
                    b.put(tx, ty, tz, Blocks::get().default_state(name));
        }
        return;
    }
    // ---- leaves without a log nearby fall apart
    if (has("leaves")) {
        if (!block_flag(id, "persistent") && block_int(id, "distance", 7) == 7) {
            b.put(x, y, z, 0);
            drop_block_loot(dim, x, y, z, id);
        }
        return;
    }
    // ---- farmland dries out without water and turns back into dirt
    if (is("minecraft:farmland")) {
        bool water = false;
        for (int dx = -4; dx <= 4 && !water; dx++)
            for (int dz = -4; dz <= 4 && !water; dz++)
                for (int dy = 0; dy <= 1 && !water; dy++) water = b.info(x + dx, y + dy, z + dz).name == "minecraft:water";
        int moisture = block_int(id, "moisture");
        if (water) { if (moisture < 7) b.put(x, y, z, block_with(id, "moisture", 7)); }
        else if (moisture > 0) b.put(x, y, z, block_with(id, "moisture", moisture - 1));
        else {
            const std::string& up = b.info(x, y + 1, z).name;
            bool planted = up == "minecraft:wheat" || up == "minecraft:carrots" || up == "minecraft:potatoes" || up == "minecraft:beetroots" || up == "minecraft:melon_stem" || up == "minecraft:pumpkin_stem" || up == "minecraft:torchflower_crop" || up.find("attached_") != std::string::npos;
            if (!planted) b.put(x, y, z, Blocks::get().default_state("minecraft:dirt"));
        }
        return;
    }
    if (is("minecraft:fire")) { fire_update(dim, x, y, z); return; }
}

void Game::random_ticks() {
    int speed = random_tick_speed_.load();
    if (speed <= 0) return;
    static const std::vector<uint8_t> flags = [] {
        std::vector<uint8_t> f(Blocks::get().count(), 0);
        for (size_t i = 0; i < f.size(); i++) {
            const sim::BlockProps* bp = sim::BlockTable::get().find(Blocks::get().info(uint16_t(i)).name);
            f[i] = (bp && bp->random_ticks) || Blocks::get().info(uint16_t(i)).name == "minecraft:hopper";   // a hopper's random tick wakes it after a restart
        }
        return f;
    }();
    struct Todo { int dim, x, y, z; uint16_t id; };
    std::vector<Todo> todo;
    std::vector<std::array<int, 3>> chunks;   // dim, cx, cz around the players
    {
        std::lock_guard<std::mutex> l(mu_);
        int radius = std::min(s_.cfg.simulation_distance, 6);
        for (auto& kv : players_) {
            const PState& p = kv.second;
            if (p.v.dead || p.gamemode == 3) continue;
            int pcx = int(std::floor(p.pos.x / 16)), pcz = int(std::floor(p.pos.z / 16));
            for (int dx = -radius; dx <= radius; dx++)
                for (int dz = -radius; dz <= radius; dz++) chunks.push_back({p.pos.dim, pcx + dx, pcz + dz});
        }
    }
    std::sort(chunks.begin(), chunks.end());
    chunks.erase(std::unique(chunks.begin(), chunks.end()), chunks.end());
    {
    PN_PERF("rt.collect");
    for (auto& c : chunks) {
        if (c[0] < 0 || c[0] > 2) continue;
        auto ch = s_.dim(c[0]).chunk_if_loaded(c[1], c[2]);
        if (!ch) continue;
        std::lock_guard<std::mutex> l(ch->mu);
        for (int s = 0; s < kSections; s++) {
            if (!ch->sec[s]) continue;
            for (int i = 0; i < speed; i++) {
                int r = rnd(4096);
                uint16_t id = ch->sec[s]->b[r];
                if (id < flags.size() && flags[id]) todo.push_back({c[0], (c[1] << 4) | (r & 15), kMinY + s * 16 + (r >> 8), (c[2] << 4) | ((r >> 4) & 15), id});
            }
        }
    }
    }
    PN_PERF("rt.apply");
    for (const Todo& t : todo) block_random_tick(t.dim, t.x, t.y, t.z, t.id);
}

// ---- pressure plates: the entities standing on them

void Game::plates_tick() {
    if (game_time_ % 2 != 0) return;
    std::vector<Redstone::PlateHit> hits;
    auto check = [&](int dim, double px, double py, double pz, bool living) {
        World& w = s_.dim(dim);
        int x = int(std::floor(px)), y = int(std::floor(py + 1.0E-4)), z = int(std::floor(pz));
        auto ch = w.chunk_if_loaded(x >> 4, z >> 4);
        if (!ch) return;
        uint16_t id;
        { std::lock_guard<std::mutex> l(ch->mu); id = ch->get(x & 15, y, z & 15); }
        if (id && block_name_has(id, "_pressure_plate")) hits.push_back({dim, x, y, z, living});
    };
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& kv : players_) if (!kv.second.v.dead && kv.second.gamemode != 3) check(kv.second.pos.dim, kv.second.pos.x, kv.second.pos.y, kv.second.pos.z, true);
    }
    for (int dim = 0; dim < 3; ++dim)
        if (entity_managers_[dim])
            for (auto& e : entity_managers_[dim]->entities())
                if (!e->removed) check(dim, e->pos.x, e->pos.y, e->pos.z, e->is_living());
    redstone().plates(hits, game_time_);
}

}  // namespace pn
