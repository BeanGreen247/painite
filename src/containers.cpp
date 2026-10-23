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
#include "sim/brewing.hpp"
#include "sim/data.hpp"
#include "sim/enchant.hpp"
#include "sim/recipe.hpp"
#include "sim/workstation.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace pn {

struct Game::BEC {
    int dim = 0, x = 0, y = 0, z = 0;
    std::string block;                // minecraft:chest ...
    sim::SimpleContainer items;
    std::vector<sim::Menu*> viewers;  // open windows on this container
    int lit = 0, lit_total = 0, cook = 0, cook_total = 0;   // furnaces
    int brew_time = 0, brew_fuel = 0, bottles = 0;           // brewing stands (bottles: bit mask of the has_bottle_N properties last applied)
    std::string brew_item;                                    // the ingredient the running brew started with
    std::map<std::string, int> used;                        // furnaces: recipes smelted since the output was last taken (their experience waits in the furnace)
    explicit BEC(int n) : items(n) {}
};

namespace {

int container_size(const std::string& n) {
    if (n == "minecraft:chest" || n == "minecraft:trapped_chest" || n == "minecraft:barrel" || n.find("shulker_box") != std::string::npos) return 27;
    if (n == "minecraft:dispenser" || n == "minecraft:dropper") return 9;
    if (n == "minecraft:hopper") return 5;
    if (n == "minecraft:furnace" || n == "minecraft:blast_furnace" || n == "minecraft:smoker") return 3;
    if (n == "minecraft:brewing_stand") return 5;
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
    if (n == "minecraft:brewing_stand") return "Brewing Stand";
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
        b->brew_time = int(be.geti("BrewTime", 0));
        b->brew_fuel = int(be.geti("Fuel", 0));
        b->lit = int(be.geti("lit_time_remaining", be.geti("BurnTime", 0)));
        b->lit_total = int(be.geti("lit_total_time", 0));
        b->cook = int(be.geti("cooking_time_spent", be.geti("CookTime", 0)));
        b->cook_total = int(be.geti("cooking_total_time", be.geti("CookTimeTotal", 0)));
        if (const Nbt* ru = be.get("RecipesUsed"); ru && ru->type == Nbt::Compound)
            for (auto& kv : ru->map) b->used[kv.first] = int(kv.second.i);
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
        ticker_.schedule_around(raw->dim, raw->x, raw->y, raw->z);   // comparators next to it (or behind a block next to it) read the new fill level
        for (int d = 0; d < 4; d++) ticker_.schedule(raw->dim, raw->x + (d == 2 ? -2 : d == 3 ? 2 : 0), raw->y, raw->z + (d == 0 ? -2 : d == 1 ? 2 : 0), 2);
        if (raw->block == "minecraft:furnace" || raw->block == "minecraft:blast_furnace" || raw->block == "minecraft:smoker" || raw->block == "minecraft:brewing_stand") bec_active_[bec_key(raw->dim, raw->x, raw->y, raw->z)] = bec_registry_[bec_key(raw->dim, raw->x, raw->y, raw->z)].lock();
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
    if (b.block == "minecraft:brewing_stand") {
        be.put("BrewTime", Nbt::make(Nbt::Short, b.brew_time));
        be.put("Fuel", Nbt::make(Nbt::Byte, b.brew_fuel));
    }
    if (container_size(b.block) == 3) {
        be.put("lit_time_remaining", Nbt::make(Nbt::Short, b.lit));
        be.put("lit_total_time", Nbt::make(Nbt::Short, b.lit_total));
        be.put("cooking_time_spent", Nbt::make(Nbt::Short, b.cook));
        be.put("cooking_total_time", Nbt::make(Nbt::Short, b.cook_total));
        Nbt ru = Nbt::compound();
        for (auto& kv : b.used) ru.put(kv.first, Nbt::make(Nbt::Int, kv.second));
        be.put("RecipesUsed", std::move(ru));
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
    if (name == "minecraft:ender_chest") {
        const BlockInfo& above = Blocks::get().info(w.get_block(x, y + 1, z));
        if (above.solid && above.opacity >= 15) return true;
        std::lock_guard<std::mutex> l(mu_);
        auto it = players_.find(gid);
        if (it == players_.end() || !it->second.sim || !it->second.ender) return false;
        PState& p = it->second;
        sim::PlayerSim& sm = *p.sim;
        if (p.v.dead || p.gamemode == 3) return false;
        if (sm.menu != sm.inventory_menu.get()) sm.handle_container_close(sm.menu->container_id);
        int type = sim::Registries::get().id("minecraft:menu", "minecraft:generic_9x3");
        if (type < 0) return false;
        int cid = sm.new_container_id();
        std::vector<sim::Container*> parts{p.ender.get()};
        auto cm = std::make_unique<sim::ContainerMenu>(type, cid, &sm.inv, parts, false, p.ender);
        GEvent e;
        e.kind = GEvent::OpenScreen;
        e.container = cid;
        e.value = type;
        e.text = "Ender Chest";
        send_event(p.id, e);
        sm.open_menu = std::move(cm);
        sm.menu = sm.open_menu.get();
        sm.menu->set_synchronizer(p.sync.get());
        return true;
    }
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
    bool brewing = name == "minecraft:brewing_stand";
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
        frm->award = [this, gid, fb] {   // awardUsedRecipesAndPopExperience (mu_ is held: windows are clicked under it)
            double xp = 0;
            for (auto& kv : fb->used) {
                const sim::CookingRecipe* r = sim::Recipes::get().cooking_by_name(kv.first);
                if (!r) continue;
                double amount = double(kv.second) * double(r->experience);
                int whole = int(std::floor(amount));
                double frac = amount - whole;
                if (frac != 0.0 && (entity_managers_[fb->dim] ? entity_managers_[fb->dim]->random().next_double() : 0.5) < frac) whole++;
                xp += whole;
            }
            if (fb->used.empty()) return;
            fb->used.clear();
            bec_save_locked(*fb);
            auto pit = players_.find(gid);
            if (pit != players_.end() && xp > 0) add_xp_locked(pit->second, int(xp));
        };
        m = std::move(fm);
    } else if (brewing) {
        auto bm = std::make_unique<sim::BrewingMenu>(type, cid, &sm.inv, &first->items, keep);
        sim::Menu* raw = bm.get();
        first->viewers.push_back(raw);
        BEC* fb = first.get();
        bm->on_closed = [fb, raw] { fb->viewers.erase(std::remove(fb->viewers.begin(), fb->viewers.end(), raw), fb->viewers.end()); };
        bm->data_slots = {first->brew_time, first->brew_fuel};
        m = std::move(bm);
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
    if (furnace || brewing) bec_active_[bec_key(dim, x, y, z)] = first;
    return true;
}

// right click on an anvil or an enchanting table
bool Game::open_workstation(uint64_t gid, int dim, int x, int y, int z) {
    if (dim < 0 || dim > 2) return false;
    World& w = s_.dim(dim);
    const std::string name = Blocks::get().info(w.get_block(x, y, z)).name;
    bool anvil = name == "minecraft:anvil" || name == "minecraft:chipped_anvil" || name == "minecraft:damaged_anvil";
    if (!anvil && name != "minecraft:enchanting_table") return false;
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return false;
    PState& p = it->second;
    sim::PlayerSim& sm = *p.sim;
    if (p.v.dead || p.gamemode == 3) return false;
    if (sm.menu != sm.inventory_menu.get()) sm.handle_container_close(sm.menu->container_id);
    int type = sim::Registries::get().id("minecraft:menu", anvil ? "minecraft:anvil" : "minecraft:enchantment");
    if (type < 0) return false;
    int cid = sm.new_container_id();
    std::unique_ptr<sim::Menu> m;
    if (anvil) {
        auto am = std::make_unique<sim::AnvilMenu>(type, cid, &sm.inv, &sm);
        am->on_result_taken = [this, dim, x, y, z] {   // AnvilMenu: a used anvil may chip (12 percent)
            sim::EntityManager* mgr = entity_managers_[dim].get();
            if (!mgr || mgr->random().next_float() >= 0.12f) return;
            post_task([this, dim, x, y, z] {
                World& w = s_.dim(dim);
                uint16_t id = w.get_block(x, y, z);
                const std::string n = Blocks::get().info(id).name;
                uint16_t next = 0;
                if (n == "minecraft:anvil") next = Blocks::get().find("minecraft:chipped_anvil", "facing=" + block_prop(id, "facing"));
                else if (n == "minecraft:chipped_anvil") next = Blocks::get().find("minecraft:damaged_anvil", "facing=" + block_prop(id, "facing"));
                else if (n != "minecraft:damaged_anvil") return;
                bool broken = next == 0;
                if (w.set_block(x, y, z, next)) block_changed(dim, x, y, z, next);
                play_sound(dim, x + 0.5, y + 0.5, z + 0.5, broken ? "minecraft:block.anvil.destroy" : "minecraft:block.anvil.use", 1.0f, 1.0f);
            });
        };
        m = std::move(am);
    } else {
        m = std::make_unique<sim::EnchantmentMenu>(type, cid, &sm.inv, &sm, [this, dim, x, y, z] {   // EnchantingTableBlock.BOOKSHELF_OFFSETS: shelves two blocks away with a free block in between
            World& w = s_.dim(dim);
            int n = 0;
            for (int dx = -2; dx <= 2; dx++)
                for (int dz = -2; dz <= 2; dz++) {
                    if (std::abs(dx) != 2 && std::abs(dz) != 2) continue;
                    for (int dy = 0; dy <= 1; dy++) {
                        if (!sim::Tags::get().contains("block", "minecraft:enchantment_power_provider", Blocks::get().info(w.get_block(x + dx, y + dy, z + dz)).name)) continue;
                        if (sim::Tags::get().contains("block", "minecraft:enchantment_power_transmitter", Blocks::get().info(w.get_block(x + dx / 2, y + dy, z + dz / 2)).name)) n++;
                    }
                }
            return n;
        });
    }
    GEvent e;
    e.kind = GEvent::OpenScreen;
    e.container = cid;
    e.value = type;
    e.text = anvil ? "Repair & Name" : "Enchant";
    send_event(p.id, e);
    sm.open_menu = std::move(m);
    sm.menu = sm.open_menu.get();
    sm.menu->set_synchronizer(p.sync.get());
    return true;
}

void Game::menu_button(uint64_t gid, int container, int button) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return;
    sim::PlayerSim& sm = *it->second.sim;
    if (!sm.menu || sm.menu->container_id != container || sm.menu == sm.inventory_menu.get() || it->second.gamemode == 3) return;
    if (sm.menu->click_menu_button(sm, button)) sm.menu->broadcast_changes();
}

void Game::anvil_rename(uint64_t gid, const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = players_.find(gid);
    if (it == players_.end() || !it->second.sim) return;
    sim::PlayerSim& sm = *it->second.sim;
    if (auto* am = dynamic_cast<sim::AnvilMenu*>(sm.menu)) am->set_item_name(name);
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
    {   // hoppers and dispensers first
        std::vector<HopperRef> due, fire;
        {
            std::lock_guard<std::mutex> l(hop_mu_);
            for (auto& kv : hoppers_) if (kv.second.due <= game_time_) due.push_back(kv.second);
            for (size_t i = 0; i < dispenses_.size();) {
                if (dispenses_[i].due <= game_time_) { fire.push_back(dispenses_[i]); dispenses_.erase(dispenses_.begin() + long(i)); } else i++;
            }
        }
        if (!due.empty() || !fire.empty()) {
            std::vector<std::function<void()>> after;
            std::vector<uint64_t> gone;
            std::vector<std::pair<uint64_t, int64_t>> next;
            {
                std::lock_guard<std::mutex> l(mu_);
                for (const HopperRef& h : due) {
                    World& w = s_.dim(h.dim);
                    uint64_t key = bec_key(h.dim, h.x, h.y, h.z);
                    if (!w.chunk_if_loaded(h.x >> 4, h.z >> 4) || !block_named(w.get_block(h.x, h.y, h.z), "minecraft:hopper")) { gone.push_back(key); continue; }
                    bool moved = hopper_move_locked(h.dim, h.x, h.y, h.z, after);
                    next.push_back({key, game_time_ + (moved ? 8 : 4)});
                }
                for (const HopperRef& d : fire) {
                    if (!s_.dim(d.dim).chunk_if_loaded(d.x >> 4, d.z >> 4)) continue;
                    dispense_locked(d.dim, d.x, d.y, d.z, after);
                }
            }
            {
                std::lock_guard<std::mutex> l(hop_mu_);
                for (uint64_t k : gone) hoppers_.erase(k);
                for (auto& n : next) { auto it = hoppers_.find(n.first); if (it != hoppers_.end()) it->second.due = n.second; }
            }
            for (auto& f : after) f();
        }
    }
    if (bec_active_.empty()) return;
    struct Pending { int dim, x, y, z; bool lit; };
    std::vector<Pending> lights;
    struct BrewPending { int dim, x, y, z, bottles; bool done; };
    std::vector<BrewPending> brews;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto it = bec_active_.begin(); it != bec_active_.end();) {
            BEC& f = *it->second;
            World& w = s_.dim(f.dim);
            if (!w.chunk_if_loaded(f.x >> 4, f.z >> 4)) { ++it; continue; }
            uint16_t id = w.get_block(f.x, f.y, f.z);
            if (Blocks::get().info(id).name != f.block) { it = bec_active_.erase(it); continue; }   // the furnace is gone
            if (f.block == "minecraft:brewing_stand") {   // BrewingStandBlockEntity.serverTick
                bool changed = false;
                sim::ItemStack& ing = f.items.item(3);
                sim::ItemStack& blaze = f.items.item(4);
                if (f.brew_fuel <= 0 && blaze.is("minecraft:blaze_powder")) {
                    f.brew_fuel = 20;
                    blaze.shrink(1);
                    if (blaze.is_empty()) f.items.items()[4] = sim::ItemStack();
                    changed = true;
                }
                bool brewable = false;
                if (sim::is_brewing_ingredient(ing))
                    for (int i = 0; i < 3; i++) if (sim::brew_changes(ing, f.items.item(i))) brewable = true;
                bool done = false;
                if (f.brew_time > 0) {
                    f.brew_time--;
                    changed = true;
                    if (f.brew_time == 0 && brewable) {
                        for (int i = 0; i < 3; i++) f.items.items()[size_t(i)] = sim::brew_mix(ing, f.items.item(i));
                        ing.shrink(1);
                        if (ing.is_empty()) f.items.items()[3] = sim::ItemStack();
                        done = true;
                    } else if (!brewable || ing.item()->name != f.brew_item) f.brew_time = 0;
                } else if (brewable && f.brew_fuel > 0) {
                    f.brew_fuel--;
                    f.brew_time = 400;
                    f.brew_item = ing.item()->name;
                    changed = true;
                }
                int mask = 0;
                for (int i = 0; i < 3; i++) if (!f.items.item(i).is_empty()) mask |= 1 << i;
                if (mask != f.bottles || done) { f.bottles = mask; brews.push_back({f.dim, f.x, f.y, f.z, mask, done}); }
                if (changed) bec_save_locked(f);
                for (sim::Menu* m : f.viewers) {
                    if (m->data_slots.size() == 2) m->data_slots = {f.brew_time, f.brew_fuel};
                    m->broadcast_changes();
                }
                if (f.brew_time <= 0 && !(brewable && f.brew_fuel > 0) && f.viewers.empty()) it = bec_active_.erase(it); else ++it;
                continue;
            }
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
                        f.used[r->name]++;
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
    for (auto& b : brews) {
        World& w = s_.dim(b.dim);
        uint16_t id = w.get_block(b.x, b.y, b.z), now = id;
        for (int i = 0; i < 3; i++) now = block_with(now, ("has_bottle_" + std::to_string(i)).c_str(), bool(b.bottles >> i & 1));
        if (now != id && w.set_block(b.x, b.y, b.z, now)) block_changed(b.dim, b.x, b.y, b.z, now);
        if (b.done) play_sound(b.dim, b.x + 0.5, b.y + 0.5, b.z + 0.5, "minecraft:block.brewing_stand.brew", 1.0f, 1.0f);
    }
    for (auto& p : lights) {   // the lit property of the block, applied outside the lock
        World& w = s_.dim(p.dim);
        uint16_t id = w.get_block(p.x, p.y, p.z);
        uint16_t now = block_with(id, "lit", p.lit);
        if (now != id && w.set_block(p.x, p.y, p.z, now)) block_changed(p.dim, p.x, p.y, p.z, now);
    }
}

int Game::container_signal(int dim, int x, int y, int z) {
    std::lock_guard<std::mutex> l(mu_);
    auto b = bec_get_locked(dim, x, y, z);
    if (!b) return -1;
    float f = 0;
    bool any = false;
    for (int i = 0; i < b->items.size(); i++) {
        sim::ItemStack& st = b->items.item(i);
        if (st.is_empty()) continue;
        any = true;
        f += float(st.count()) / float(std::min(b->items.max_stack_size(), st.max_stack_size()));
    }
    f /= float(b->items.size());
    return int(std::floor(f * 14.0f)) + (any ? 1 : 0);
}

void Game::play_sound(int dim, double x, double y, double z, const std::string& name, float volume, float pitch) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& kv : players_) {
        PState& p = kv.second;
        if (p.pos.dim != dim) continue;
        double dx = p.pos.x - x, dy = p.pos.y - y, dz = p.pos.z - z;
        if (dx * dx + dy * dy + dz * dz > 64.0 * 64.0) continue;
        GEvent e;
        e.kind = GEvent::Sound;
        e.text = name;
        e.bx = int(std::floor(x * 8.0)); e.by = int(std::floor(y * 8.0)); e.bz = int(std::floor(z * 8.0));
        e.health = volume; e.saturation = pitch;
        e.value = 4;   // SoundSource.BLOCKS
        send_event(p.id, e);
    }
}

// ---- hoppers, droppers and dispensers

namespace {

const int DX6[6] = {0, 0, 0, 0, -1, 1}, DY6[6] = {-1, 1, 0, 0, 0, 0}, DZ6[6] = {0, 0, -1, 1, 0, 0};
const char* const DN6[6] = {"down", "up", "north", "south", "west", "east"};
int dir6(const std::string& n) { for (int i = 0; i < 6; i++) if (n == DN6[i]) return i; return 0; }

// put as much of `stack` as fits into the container a neighbour pushes into (`travel` is the direction the item moves: 0 = down, into the top)
bool insert_stack(sim::SimpleContainer& c, const std::string& block, sim::ItemStack& stack, int travel) {
    if (stack.is_empty()) return false;
    int first = 0, last = c.size();
    bool furnace = c.size() == 3;
    if (furnace) {
        if (travel == 1) return false;           // nothing goes into a furnace from below
        if (travel == 0) last = 1;               // from above: the ingredient slot
        else first = 1, last = 2;                // from the side: the fuel slot
    }
    if (block.find("shulker_box") != std::string::npos && stack.item()->name.find("shulker_box") != std::string::npos) return false;
    bool moved = false;
    auto allowed = [&](int slot) {
        if (!furnace || slot != 1) return true;
        return sim::Recipes::get().is_fuel(stack) || (stack.item()->name == "minecraft:bucket" && c.item(1).is_empty());
    };
    for (int pass = 0; pass < 2 && !stack.is_empty(); pass++)   // first onto stacks that already hold the item, then into empty slots
        for (int i = first; i < last && !stack.is_empty(); i++) {
            sim::ItemStack& dst = c.item(i);
            if (!allowed(i)) continue;
            if (pass == 0) {
                if (dst.is_empty() || !sim::ItemStack::is_same_item_same_components(dst, stack)) continue;
                int room = std::min(c.max_stack_size(), dst.max_stack_size()) - dst.count();
                int n = std::min(room, stack.count());
                if (n <= 0) continue;
                dst.grow(n); stack.shrink(n); c.set_changed(); moved = true;
            } else if (dst.is_empty()) {
                int n = std::min(stack.count(), std::min(c.max_stack_size(), stack.max_stack_size()));
                c.set_item(i, stack.copy_with_count(n)); stack.shrink(n); moved = true;
            }
        }
    return moved;
}

}  // namespace

void Game::hopper_register(int dim, int x, int y, int z) {
    std::lock_guard<std::mutex> l(hop_mu_);
    uint64_t key = bec_key(dim, x, y, z);
    if (!hoppers_.count(key)) hoppers_[key] = {dim, x, y, z, game_time_ + 1};
}

void Game::queue_dispense(int dim, int x, int y, int z, int delay) {
    std::lock_guard<std::mutex> l(hop_mu_);
    dispenses_.push_back({dim, x, y, z, game_time_ + delay});
}

// HopperBlockEntity.tryMoveItems: push one item into what the hopper faces, pull one from the container above or take the item entities on top
bool Game::hopper_move_locked(int dim, int x, int y, int z, std::vector<std::function<void()>>& after) {
    (void)after;
    World& w = s_.dim(dim);
    uint16_t id = w.get_block(x, y, z);
    if (!block_flag(id, "enabled")) return false;
    auto self = bec_get_locked(dim, x, y, z);
    if (!self) return false;
    int facing = dir6(block_prop(id, "facing"));
    bool moved = false;
    int tx = x + DX6[facing], ty = y + DY6[facing], tz = z + DZ6[facing];
    if (w.chunk_if_loaded(tx >> 4, tz >> 4)) {
        if (auto target = bec_get_locked(dim, tx, ty, tz)) {
            for (int i = 0; i < 5 && !moved; i++) {
                sim::ItemStack& st = self->items.item(i);
                if (st.is_empty()) continue;
                sim::ItemStack one = st.copy_with_count(1);
                if (insert_stack(target->items, target->block, one, facing) && one.is_empty()) { st.shrink(1); self->items.set_changed(); moved = true; }
            }
        }
    }
    bool full = true;
    for (int i = 0; i < 5; i++) { sim::ItemStack& st = self->items.item(i); if (st.is_empty() || st.count() < st.max_stack_size()) full = false; }
    if (full) return moved;
    bool pulled = false;
    if (w.chunk_if_loaded(x >> 4, (z) >> 4)) {
        if (auto above = bec_get_locked(dim, x, y + 1, z)) {
            std::vector<int> slots;
            if (above->items.size() == 3) { slots.push_back(2); if (above->items.item(1).item()->name == "minecraft:bucket") slots.push_back(1); }
            else for (int i = 0; i < above->items.size(); i++) slots.push_back(i);
            for (int i : slots) {
                sim::ItemStack& st = above->items.item(i);
                if (st.is_empty()) continue;
                sim::ItemStack one = st.copy_with_count(1);
                if (insert_stack(self->items, self->block, one, 0) && one.is_empty()) { st.shrink(1); above->items.set_changed(); pulled = true; break; }
            }
        } else if (entity_managers_[dim]) {
            std::vector<sim::Entity*> found;
            entity_managers_[dim]->entities_in_box(sim::AABB(x, y + 0.6875, z, x + 1, y + 2, z + 1), nullptr, found);
            for (sim::Entity* e : found) {
                auto* item = dynamic_cast<sim::ItemEntity*>(e);
                if (!item || item->removed || item->item.is_empty()) continue;
                if (insert_stack(self->items, self->block, item->item, 0)) { pulled = true; if (item->item.is_empty()) item->discard(); break; }
            }
        }
    }
    return moved || pulled;
}

// DispenserBlock / DropperBlock.dispenseFrom
void Game::dispense_locked(int dim, int x, int y, int z, std::vector<std::function<void()>>& after) {
    World& w = s_.dim(dim);
    uint16_t id = w.get_block(x, y, z);
    const std::string& name = Blocks::get().info(id).name;
    bool dropper = name == "minecraft:dropper";
    if (!dropper && name != "minecraft:dispenser") return;
    auto self = bec_get_locked(dim, x, y, z);
    if (!self || !entity_managers_[dim]) return;
    sim::EntityManager& mgr = *entity_managers_[dim];
    std::vector<int> filled;
    for (int i = 0; i < 9; i++) if (!self->items.item(i).is_empty()) filled.push_back(i);
    if (filled.empty()) return;
    int slot = filled[size_t(mgr.random().next_int(int(filled.size())))];
    sim::ItemStack& st = self->items.item(slot);
    int dir = dir6(block_prop(id, "facing"));
    int fx = x + DX6[dir], fy = y + DY6[dir], fz = z + DZ6[dir];
    const std::string item = st.item()->name;
    if (dropper) {
        if (w.chunk_if_loaded(fx >> 4, fz >> 4))
            if (auto target = bec_get_locked(dim, fx, fy, fz)) {
                sim::ItemStack one = st.copy_with_count(1);
                if (insert_stack(target->items, target->block, one, dir) && one.is_empty()) { st.shrink(1); self->items.set_changed(); }
                return;   // a dropper facing a full container does nothing
            }
    } else {
        auto replace_with = [&](const char* to) {   // a bucket turns into another: in the slot when alone, else the new one goes into the dispenser
            const sim::ItemType* t = sim::Items::get().by_name(to);
            if (!t) return;
            sim::ItemStack n(t, 1);
            if (st.count() == 1) self->items.set_item(slot, n);
            else { st.shrink(1); self->items.set_changed(); if (!insert_stack(self->items, self->block, n, 5)) after.push_back([this, dim, x, y, z, n] { post_task([this, dim, x, y, z, n] { if (!entity_managers_[dim]) return; auto& m = *entity_managers_[dim]; auto e = std::make_unique<sim::ItemEntity>(&m, x + 0.5, y + 1.0, z + 0.5, n); e->set_default_pickup_delay(); m.add_entity(std::move(e)); }); }); }
        };
        if (item == "minecraft:water_bucket" || item == "minecraft:lava_bucket") {
            const BlockInfo& fb = Blocks::get().info(w.get_block(fx, fy, fz));
            int pour = item == "minecraft:water_bucket" ? 1 : 2;
            bool waterlogs = pour == 1 && fb.props.find("waterlogged=false") != std::string::npos;
            if (!waterlogs && ((!fb.air && !fb.replaceable && fb.name != "minecraft:water" && fb.name != "minecraft:lava") || fb.solid)) return;   // nowhere to pour: it stays
            after.push_back([this, dim, fx, fy, fz, pour] { dispense_fluid(dim, fx, fy, fz, pour); });
            replace_with("minecraft:bucket");
            return;
        }
        if (item == "minecraft:bucket") {
            int kind = 0;
            uint16_t at = w.get_block(fx, fy, fz);
            const BlockInfo& fb = Blocks::get().info(at);
            if ((fb.name == "minecraft:water" || fb.name == "minecraft:lava") && block_int(at, "level", 1) == 0) kind = fb.name == "minecraft:water" ? 1 : 2;
            if (kind) {
                after.push_back([this, dim, fx, fy, fz] { dispense_fluid(dim, fx, fy, fz, 0); });
                replace_with(kind == 1 ? "minecraft:water_bucket" : "minecraft:lava_bucket");
            }
            return;
        }
        if (item == "minecraft:flint_and_steel" || item == "minecraft:fire_charge") {
            after.push_back([this, dim, fx, fy, fz] { if (!set_fire(dim, fx, fy, fz)) redstone().ignite_tnt(dim, fx, fy, fz); });
            if (item == "minecraft:fire_charge") st.shrink(1);
            else if (st.damageable()) { int d = st.damage_value() + 1; if (d >= st.item()->max_damage) st.shrink(st.count()); else st.set_damage_value(d); }
            self->items.set_changed();
            return;
        }
        if (item == "minecraft:tnt") {
            st.shrink(1);
            self->items.set_changed();
            after.push_back([this, dim, fx, fy, fz] { add_entity(dim, std::make_unique<sim::PrimedTnt>(nullptr, fx + 0.5, fy, fz + 0.5)); });
            return;
        }
        if (item == "minecraft:arrow" || item == "minecraft:snowball" || item == "minecraft:egg") {
            st.shrink(1);
            self->items.set_changed();
            sim::Vec3 at(x + 0.5 + DX6[dir] * 0.7, y + 0.5 - 0.15625 + DY6[dir] * 0.7, z + 0.5 + DZ6[dir] * 0.7);
            sim::Vec3 vel(DX6[dir] * 1.1, DY6[dir] * 1.1 + 0.1, DZ6[dir] * 1.1);
            after.push_back([this, dim, item, at, vel] {
                if (item == "minecraft:arrow") add_entity(dim, std::make_unique<sim::ArrowEntity>(nullptr, "minecraft:player", at, vel));
                else add_entity(dim, std::make_unique<sim::ThrownEntity>(nullptr, item, at, vel, 0));
            });
            return;
        }
    }
    // the default: the item is dropped in front with a little spread
    sim::ItemStack out = st.copy_with_count(1);
    st.shrink(1);
    self->items.set_changed();
    double ox = x + 0.5 + DX6[dir] * 0.7, oy = y + 0.5 + DY6[dir] * 0.7 - (dir <= 1 ? 0.125 : 0.15625), oz = z + 0.5 + DZ6[dir] * 0.7;
    double speed = mgr.random().next_double() * 0.1 + 0.2;
    auto tri = [&](double center, double dev) { return center + dev * (mgr.random().next_double() - mgr.random().next_double()); };
    sim::Vec3 vel(tri(DX6[dir] * speed, 0.0172275 * 6), tri(0.2, 0.0172275 * 6), tri(DZ6[dir] * speed, 0.0172275 * 6));
    after.push_back([this, dim, ox, oy, oz, out, vel] {
        post_task([this, dim, ox, oy, oz, out, vel] {
            if (!entity_managers_[dim]) return;
            auto e = std::make_unique<sim::ItemEntity>(entity_managers_[dim].get(), ox, oy, oz, out);
            e->delta = vel;
            e->set_default_pickup_delay();
            entity_managers_[dim]->add_entity(std::move(e));
        });
    });
}

}  // namespace pn
