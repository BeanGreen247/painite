// Block entities that hold items: chests, barrels, shulker boxes, dispensers, droppers, hoppers and furnaces.
// The items live in the chunk's block entity list (like a vanilla world) and are written back whenever a slot changes.
// Everything here runs with Game::mu_ held (players_ and the menus are protected by it); block changes that need mu_ themselves are applied after it is released.
#include "blockstate.hpp"
#include "game.hpp"
#include "log.hpp"
#include "perf.hpp"
#include "server.hpp"
#include "sim/entity_manager.hpp"
#include "sim/item_nbt.hpp"
#include "sim/loot.hpp"
#include "sim/recipe.hpp"
#include <algorithm>

namespace pn {

struct Game::BEC {
    int dim = 0, x = 0, y = 0, z = 0;
    std::string block;                // minecraft:chest ...
    sim::SimpleContainer items;
    std::vector<sim::Menu*> viewers;  // open windows on this container
    int lit = 0, lit_total = 0, cook = 0, cook_total = 0;   // furnaces
    explicit BEC(int n) : items(n) {}
};

namespace {

int container_size(const std::string& n) {
    if (n == "minecraft:chest" || n == "minecraft:trapped_chest" || n == "minecraft:barrel" || n.find("shulker_box") != std::string::npos) return 27;
    if (n == "minecraft:dispenser" || n == "minecraft:dropper") return 9;
    if (n == "minecraft:hopper") return 5;
    if (n == "minecraft:furnace" || n == "minecraft:blast_furnace" || n == "minecraft:smoker") return 3;
    return 0;
}

const char* furnace_kind(const std::string& n) { return n == "minecraft:blast_furnace" ? "blasting" : n == "minecraft:smoker" ? "smoking" : "smelting"; }

uint64_t bec_key(int dim, int x, int y, int z) {
    return (uint64_t(uint32_t(x)) << 32) ^ (uint64_t(uint32_t(z)) << 8) ^ uint64_t(uint32_t(y + 64)) ^ (uint64_t(dim) << 62);
}

const char* title_of(const std::string& n) {
    if (n == "minecraft:chest" || n == "minecraft:trapped_chest") return "Chest";
    if (n == "minecraft:barrel") return "Barrel";
    if (n == "minecraft:dispenser") return "Dispenser";
    if (n == "minecraft:dropper") return "Dropper";
    if (n == "minecraft:hopper") return "Item Hopper";
    if (n == "minecraft:furnace") return "Furnace";
    if (n == "minecraft:blast_furnace") return "Blast Furnace";
    if (n == "minecraft:smoker") return "Smoker";
    return "Shulker Box";
}

// a furnace may smelt `input` into the output slot
bool can_burn(const sim::CookingRecipe* r, sim::SimpleContainer& c) {
    if (!r) return false;
    sim::ItemStack& out = c.item(2);
    if (out.is_empty()) return true;
    if (!sim::ItemStack::is_same_item_same_components(out, r->result)) return false;
    return out.count() + r->result.count() <= std::min(c.max_stack_size(), out.max_stack_size());
}

}  // namespace

// the container of the block entity at a spot, loaded from the chunk when no window or furnace holds it yet; null when the block has no inventory
std::shared_ptr<Game::BEC> Game::bec_get_locked(int dim, int x, int y, int z) {
    uint64_t key = bec_key(dim, x, y, z);
    auto it = bec_registry_.find(key);
    if (it != bec_registry_.end()) if (auto sp = it->second.lock()) return sp;
    World& w = s_.dim(dim);
    if (!w.chunk_if_loaded(x >> 4, z >> 4)) return nullptr;
    const BlockInfo& bi = Blocks::get().info(w.get_block(x, y, z));
    int n = container_size(bi.name);
    if (!n) return nullptr;
    auto b = std::make_shared<BEC>(n);
    b->dim = dim; b->x = x; b->y = y; b->z = z;
    b->block = bi.name;
    Nbt be;
    if (w.get_block_entity(x, y, z, be)) {
        if (const Nbt* list = be.get("Items"); list && list->type == Nbt::List)
            for (const Nbt& e : list->list) {
                sim::ItemStack st;
                int slot = -1;
                if (sim::item_from_nbt(e, st, &slot) && slot >= 0 && slot < n) b->items.items()[size_t(slot)] = st;
            }
        b->lit = int(be.geti("lit_time_remaining", be.geti("BurnTime", 0)));
        b->lit_total = int(be.geti("lit_total_time", 0));
        b->cook = int(be.geti("cooking_time_spent", be.geti("CookTime", 0)));
        b->cook_total = int(be.geti("cooking_total_time", be.geti("CookTimeTotal", 0)));
        if (const Nbt* lt = be.get("LootTable"); lt && lt->type == Nbt::String) {   // a generated chest rolls its loot the first time somebody looks inside
            if (const sim::LootTable* table = sim::LootTables::get(lt->s)) {
                sim::LootParams lp;
                sim::ItemStack none;
                lp.tool = &none;
                lp.has_origin = true;
                lp.ox = x + 0.5; lp.oy = y + 0.5; lp.oz = z + 0.5;
                int64_t seed = be.geti("LootTableSeed", 0);
                if (!seed) seed = int64_t(entity_managers_[dim] ? entity_managers_[dim]->random().next_long() : uint64_t(x) * 31 + uint64_t(z));
                std::vector<sim::ItemStack> out;
                table->get_random_items(lp, seed, out);
                std::vector<int> free_slots;
                for (int i = 0; i < n; i++) if (b->items.items()[size_t(i)].is_empty()) free_slots.push_back(i);
                vg::LegacyRandom rnd(seed);
                for (int i = int(free_slots.size()) - 1; i > 0; i--) std::swap(free_slots[size_t(i)], free_slots[size_t(rnd.next_int(i + 1))]);   // Util.shuffle
                for (sim::ItemStack& st : out) {
                    if (st.is_empty() || free_slots.empty()) continue;
                    b->items.items()[size_t(free_slots.back())] = st;
                    free_slots.pop_back();
                }
                be.erase("LootTable");
                be.erase("LootTableSeed");
                bec_registry_[key] = b;
                bec_save_locked(*b);
            }
        }
    }
    BEC* raw = b.get();
    b->items.on_changed = [this, raw] {
        bec_save_locked(*raw);
        for (sim::Menu* m : raw->viewers) m->broadcast_changes();
        if (raw->block == "minecraft:furnace" || raw->block == "minecraft:blast_furnace" || raw->block == "minecraft:smoker") bec_active_[bec_key(raw->dim, raw->x, raw->y, raw->z)] = bec_registry_[bec_key(raw->dim, raw->x, raw->y, raw->z)].lock();
    };
    bec_registry_[key] = b;
    return b;
}

void Game::bec_save_locked(BEC& b) {
    Nbt be = Nbt::compound();
    be.put("id", Nbt::str(b.block));
    be.put("x", Nbt::make(Nbt::Int, b.x));
    be.put("y", Nbt::make(Nbt::Int, b.y));
    be.put("z", Nbt::make(Nbt::Int, b.z));
    Nbt items = Nbt::list_of(Nbt::Compound);
    for (int i = 0; i < b.items.size(); i++) if (!b.items.items()[size_t(i)].is_empty()) items.list.push_back(sim::item_to_nbt(b.items.items()[size_t(i)], i));
    be.put("Items", std::move(items));
    if (container_size(b.block) == 3) {
        be.put("lit_time_remaining", Nbt::make(Nbt::Short, b.lit));
        be.put("lit_total_time", Nbt::make(Nbt::Short, b.lit_total));
        be.put("cooking_time_spent", Nbt::make(Nbt::Short, b.cook));
        be.put("cooking_total_time", Nbt::make(Nbt::Short, b.cook_total));
    }
    s_.dim(b.dim).put_block_entity(b.x, b.y, b.z, be);
}

// right click on a chest, barrel, furnace ...: open its window
bool Game::open_container(uint64_t gid, int dim, int x, int y, int z) {
    if (dim < 0 || dim > 2) return false;
    World& w = s_.dim(dim);
    uint16_t id = w.get_block(x, y, z);
    const BlockInfo& bi = Blocks::get().info(id);
    const std::string name = bi.name;
    if (!container_size(name)) return false;
    {   // a solid block on top shuts a chest
        const BlockInfo& above = Blocks::get().info(w.get_block(x, y + 1, z));
        if ((name == "minecraft:chest" || name == "minecraft:trapped_chest") && above.solid && above.opacity >= 15) return true;
    }
    std::string kind;
    if (name == "minecraft:chest" || name == "minecraft:trapped_chest" || name == "minecraft:barrel") kind = "generic_9x3";
    else if (name.find("shulker_box") != std::string::npos) kind = "shulker_box";
    else if (name == "minecraft:dispenser" || name == "minecraft:dropper") kind = "generic_3x3";
    else if (name == "minecraft:hopper") kind = "hopper";
    else kind = name.substr(10);   // furnace, blast_furnace, smoker
    std::string type_prop = (name == "minecraft:chest" || name == "minecraft:trapped_chest") ? block_prop(id, "type") : "single";
    int px = x, pz = z;   // the other half of a double chest
    if (type_prop == "left" || type_prop == "right") {
        static const char* dirs[4] = {"north", "east", "south", "west"};
        std::string f = block_prop(id, "facing");
        int fi = 0;
        for (int i = 0; i < 4; i++) if (f == dirs[i]) fi = i;
        int cw = (fi + (type_prop == "left" ? 1 : 3)) & 3;   // ChestBlock.getConnectedDirection
        static const int ddx[4] = {0, 1, 0, -1}, ddz[4] = {-1, 0, 1, 0};
        px = x + ddx[cw]; pz = z + ddz[cw];
        if (Blocks::get().info(w.get_block(px, y, pz)).name != name) { px = x; pz = z; type_prop = "single"; }
        if (kind == "generic_9x3" && type_prop != "single") kind = "generic_9x6";
    }
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return false;
    auto first = bec_get_locked(dim, x, y, z);
    if (!first) return false;
    std::shared_ptr<BEC> second;
    if (px != x || pz != z) second = bec_get_locked(dim, px, y, pz);
    if (!second) { if (kind == "generic_9x6") kind = "generic_9x3"; }
    auto keep = std::make_shared<std::vector<std::shared_ptr<BEC>>>();
    std::vector<std::shared_ptr<BEC>> order;
    if (second) { if (type_prop == "right") order = {first, second}; else order = {second, first}; }   // the right half holds the first 27 slots
    else order = {first};
    *keep = order;
    std::string title = second ? "Large Chest" : title_of(name);
    bool furnace = container_size(name) == 3;
    // the window is built under mu_ (open_menu takes it, so build here and hand over)
    PState& p = it->second;
    sim::PlayerSim& sm = *p.sim;
    if (p.v.dead || p.gamemode == 3) return false;
    if (sm.menu != sm.inventory_menu.get()) sm.handle_container_close(sm.menu->container_id);
    int type = sim::Registries::get().id("minecraft:menu", "minecraft:" + kind);
    if (type < 0) return false;
    int cid = sm.new_container_id();
    std::unique_ptr<sim::Menu> m;
    if (furnace) {
        auto fm = std::make_unique<sim::FurnaceMenu>(type, cid, &sm.inv, &first->items, furnace_kind(name), keep);
        sim::Menu* raw = fm.get();
        sim::FurnaceMenu* frm = fm.get();
        first->viewers.push_back(raw);
        BEC* fb = first.get();
        frm->on_closed = [fb, raw] { fb->viewers.erase(std::remove(fb->viewers.begin(), fb->viewers.end(), raw), fb->viewers.end()); };
        frm->data_slots = {first->lit, first->lit_total, first->cook, first->cook_total};
        m = std::move(fm);
    } else {
        std::vector<sim::Container*> parts;
        for (auto& b : order) parts.push_back(&b->items);
        auto cm = std::make_unique<sim::ContainerMenu>(type, cid, &sm.inv, parts, name.find("shulker_box") != std::string::npos, keep);
        sim::Menu* raw = cm.get();
        for (auto& b : order) b->viewers.push_back(raw);
        std::vector<BEC*> raws;
        for (auto& b : order) raws.push_back(b.get());
        cm->on_closed = [raws, raw] { for (BEC* b : raws) b->viewers.erase(std::remove(b->viewers.begin(), b->viewers.end(), raw), b->viewers.end()); };
        m = std::move(cm);
    }
    GEvent e;
    e.kind = GEvent::OpenScreen;
    e.container = cid;
    e.value = type;
    e.text = title;
    send_event(p.id, e);
    sm.open_menu = std::move(m);
    sm.menu = sm.open_menu.get();
    sm.menu->set_synchronizer(p.sync.get());
    if (furnace) bec_active_[bec_key(dim, x, y, z)] = first;
    return true;
}

// the items of a container block that is being broken pop out of it
void Game::spill_container(int dim, int x, int y, int z, uint16_t state) {
    if (!container_size(Blocks::get().info(state).name)) return;
    std::vector<sim::ItemStack> stacks;
    {
        std::lock_guard<std::mutex> l(mu_);
        uint64_t key = bec_key(dim, x, y, z);
        auto b = bec_get_locked(dim, x, y, z);
        if (!b) return;
        for (auto& st : b->items.items()) if (!st.is_empty()) stacks.push_back(st);
        for (auto& st : b->items.items()) st = sim::ItemStack();
        for (sim::Menu* m : b->viewers) m->broadcast_changes();
        bec_active_.erase(key);
        bec_registry_.erase(key);
    }
    if (stacks.empty()) return;
    post_task([this, dim, x, y, z, stacks] {
        if (!entity_managers_[dim]) return;
        sim::EntityManager& mgr = *entity_managers_[dim];
        for (const sim::ItemStack& st : stacks) {
            auto item = std::make_unique<sim::ItemEntity>(&mgr, x + 0.5 + mgr.random().next_double() * 0.5 - 0.25, y + 0.5 + mgr.random().next_double() * 0.5 - 0.25, z + 0.5 + mgr.random().next_double() * 0.5 - 0.25, st);
            item->set_default_pickup_delay();
            mgr.add_entity(std::move(item));
        }
    });
}

// AbstractFurnaceBlockEntity.serverTick for the furnaces somebody has used and that still have work to do
void Game::containers_tick() {
    if (bec_active_.empty()) return;
    struct Pending { int dim, x, y, z; bool lit; };
    std::vector<Pending> lights;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto it = bec_active_.begin(); it != bec_active_.end();) {
            BEC& f = *it->second;
            World& w = s_.dim(f.dim);
            if (!w.chunk_if_loaded(f.x >> 4, f.z >> 4)) { ++it; continue; }
            uint16_t id = w.get_block(f.x, f.y, f.z);
            if (Blocks::get().info(id).name != f.block) { it = bec_active_.erase(it); continue; }   // the furnace is gone
            const char* kind = furnace_kind(f.block);
            bool was_lit = f.lit > 0, changed = false;
            if (was_lit) f.lit--;
            sim::ItemStack& fuel = f.items.item(1);
            sim::ItemStack& input = f.items.item(0);
            bool has_input = !input.is_empty(), has_fuel = !fuel.is_empty();
            const sim::CookingRecipe* r = has_input ? sim::Recipes::get().find_cooking(kind, input) : nullptr;
            if (f.lit > 0 || (has_fuel && has_input)) {
                if (f.lit <= 0 && can_burn(r, f.items)) {
                    f.lit = sim::Recipes::get().fuel_ticks(fuel);
                    f.lit_total = f.lit;
                    if (f.lit > 0) {
                        changed = true;
                        if (has_fuel) {
                            sim::ItemStack rest;
                            if (fuel.item()->name == "minecraft:lava_bucket") rest = sim::ItemStack(sim::Items::get().by_name("minecraft:bucket"), 1);
                            fuel.shrink(1);
                            if (fuel.is_empty()) f.items.items()[1] = rest;
                        }
                    }
                }
                if (f.lit > 0 && can_burn(r, f.items)) {
                    f.cook++;
                    f.cook_total = r->time;
                    if (f.cook >= f.cook_total) {
                        f.cook = 0;
                        sim::ItemStack& out = f.items.item(2);
                        if (out.is_empty()) f.items.items()[2] = r->result; else out.grow(r->result.count());
                        input.shrink(1);
                        changed = true;
                    }
                } else if (f.cook != 0) f.cook = 0;
            } else if (f.lit <= 0 && f.cook > 0) f.cook = std::max(0, f.cook - 2);
            bool lit_now = f.lit > 0;
            if (was_lit != lit_now) { changed = true; lights.push_back({f.dim, f.x, f.y, f.z, lit_now}); }
            if (changed) bec_save_locked(f);
            for (sim::Menu* m : f.viewers) {
                if (m->data_slots.size() == 4) m->data_slots = {f.lit, f.lit_total, f.cook, f.cook_total};
                m->broadcast_changes();
            }
            bool idle = f.lit <= 0 && f.cook <= 0 && !(has_fuel && has_input && r) && f.viewers.empty();
            if (idle) it = bec_active_.erase(it); else ++it;
        }
    }
    for (auto& p : lights) {   // the lit property of the block, applied outside the lock
        World& w = s_.dim(p.dim);
        uint16_t id = w.get_block(p.x, p.y, p.z);
        uint16_t now = block_with(id, "lit", p.lit);
        if (now != id && w.set_block(p.x, p.y, p.z, now)) block_changed(p.dim, p.x, p.y, p.z, now);
    }
}

}  // namespace pn
