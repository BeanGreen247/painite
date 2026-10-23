#include "workstation.hpp"
#include "components.hpp"
#include "enchant.hpp"
#include <algorithm>
#include <cmath>

namespace pn::sim {

namespace {

class AnvilResultSlot : public Slot {
public:
    AnvilResultSlot(AnvilMenu* m, Container* c) : Slot(c, 0), menu(m) {}
    bool may_place(const ItemStack&) const override { return false; }
    bool may_pickup(MenuPlayer& p) override { int cost = menu->data_slots[0]; return (p.has_infinite_materials() || p.experience_level() >= cost) && cost > 0 && has_item(); }
    void on_take(MenuPlayer& p, const ItemStack&) override {
        if (!p.has_infinite_materials()) p.give_experience_levels(-menu->data_slots[0]);
        if (menu->repair_item_count_cost > 0) {
            ItemStack& add = menu->input.item(1);
            if (!add.is_empty() && add.count() > menu->repair_item_count_cost) { add.shrink(menu->repair_item_count_cost); menu->input.set_changed(); }
            else menu->input.set_item(1, ItemStack());
        } else menu->input.set_item(1, ItemStack());
        menu->input.set_item(0, ItemStack());
        menu->repair_item_count_cost = 0;
        menu->data_slots[0] = 0;
        if (menu->on_result_taken && !p.has_infinite_materials()) menu->on_result_taken();
        Slot::on_take(p, ItemStack());
    }
    AnvilMenu* menu;
};

class OneItemSlot : public Slot {
public:
    using Slot::Slot;
    int max_stack_size() const override { return 1; }
};

class LapisSlot : public Slot {
public:
    using Slot::Slot;
    bool may_place(const ItemStack& s) const override { return s.is("minecraft:lapis_lazuli"); }
};

}  // namespace

// ---- anvil

AnvilMenu::AnvilMenu(int menu_type_id, int container_id, PlayerInventory* inv, MenuPlayer* o) : Menu(menu_type_id, container_id), owner(o) {
    input.on_changed = [this] { create_result(); };
    add_slot(std::make_unique<Slot>(&input, 0));
    add_slot(std::make_unique<Slot>(&input, 1));
    add_slot(std::make_unique<AnvilResultSlot>(this, &result));
    add_standard_inventory_slots(inv, 8, 84);
    data_slots.assign(1, 0);
}

bool AnvilMenu::set_item_name(const std::string& name) {
    std::string n = name.substr(0, 50);
    if (n == item_name) return false;
    item_name = n;
    if (result.item(0).is_empty() == false) {
        ItemStack& r = result.item(0);
        bool blank = std::all_of(n.begin(), n.end(), [](char c) { return c == ' ' || c == '\t'; });
        if (blank) remove_component(r, "minecraft:custom_name"); else set_custom_name(r, n);
    }
    create_result();
    broadcast_changes();
    return true;
}

void AnvilMenu::create_result() {
    auto fail = [this] { result.items()[0] = ItemStack(); data_slots[0] = 0; };
    ItemStack in = input.item(0);
    data_slots[0] = 1;
    long base_cost = 0;
    int tax = 0, naming_cost = 0;
    repair_item_count_cost = 0;
    if (in.is_empty() || (!in.has_component("minecraft:enchantments") && !in.has_component("minecraft:stored_enchantments"))) { fail(); return; }
    ItemStack res = in;
    ItemStack add = input.item(1);
    const Enchants& E = Enchants::get();
    std::map<std::string, int> ench = get_enchantments(res, in.is("minecraft:enchanted_book") ? "minecraft:stored_enchantments" : "minecraft:enchantments");
    base_cost += get_repair_cost(in) + (add.is_empty() ? 0 : get_repair_cost(add));
    bool using_book = false;
    if (!add.is_empty()) {
        using_book = add.has_component("minecraft:stored_enchantments") && !get_enchantments(add, "minecraft:stored_enchantments").empty();
        if (res.damageable() && is_valid_repair_item(in, add)) {
            int amount = std::min(res.damage_value(), res.max_damage() / 4);
            if (amount <= 0) { fail(); return; }
            int count = 0;
            for (; amount > 0 && count < add.count(); count++) {
                res.set_damage_value(res.damage_value() - amount);
                tax++;
                amount = std::min(res.damage_value(), res.max_damage() / 4);
            }
            repair_item_count_cost = count;
        } else {
            if (!using_book && (!res.is(add.item()) || !res.damageable())) { fail(); return; }
            if (res.damageable() && !using_book) {
                int remaining1 = in.max_damage() - in.damage_value();
                int remaining2 = add.max_damage() - add.damage_value();
                int bonus = remaining2 + res.max_damage() * 12 / 100;
                int new_damage = std::max(0, res.max_damage() - (remaining1 + bonus));
                if (new_damage < res.damage_value()) { res.set_damage_value(new_damage); tax += 2; }
            }
            bool any_ok = false, any_bad = false;
            for (auto& kv : get_enchantments(add, using_book ? "minecraft:stored_enchantments" : "minecraft:enchantments")) {
                const EnchantDef* d = E.find(kv.first);
                if (!d) continue;
                int cur = ench.count(kv.first) ? ench[kv.first] : 0;
                int level = kv.second;
                level = cur == level ? level + 1 : std::max(level, cur);
                bool can = E.can_enchant(*d, in) || owner->has_infinite_materials() || in.is("minecraft:enchanted_book");
                for (auto& ex : ench) {
                    const EnchantDef* o = E.find(ex.first);
                    if (o && ex.first != kv.first && !E.compatible(*d, *o)) can = false;
                }
                if (!can) { any_bad = true; continue; }
                any_ok = true;
                level = std::min(level, d->max_level);
                ench[kv.first] = level;
                int fee = d->anvil_cost;
                if (using_book) fee = std::max(1, fee / 2);
                tax += fee * level;
                if (in.count() > 1) tax = 40;
            }
            if (any_bad && !any_ok) { fail(); return; }
        }
    }
    bool blank = std::all_of(item_name.begin(), item_name.end(), [](char c) { return c == ' ' || c == '\t'; });
    if (!blank) {
        if (item_name != get_custom_name(in)) { naming_cost = 1; tax += 1; set_custom_name(res, item_name); }
    } else if (!get_custom_name(in).empty() || in.has_component("minecraft:custom_name")) {
        naming_cost = 1; tax += 1;
        remove_component(res, "minecraft:custom_name");
    }
    long total = std::max(0L, std::min(base_cost + tax, 2147483647L));
    int cost = int(total);
    if (tax <= 0) res = ItemStack();
    if (naming_cost == tax && naming_cost > 0 && cost >= 40) cost = 39;
    if (cost >= 40 && !owner->has_infinite_materials()) res = ItemStack();
    if (!res.is_empty()) {
        int rc = get_repair_cost(res);
        if (!add.is_empty() && rc < get_repair_cost(add)) rc = get_repair_cost(add);
        if (naming_cost != tax || naming_cost == 0) rc = int(std::min(long(rc) * 2 + 1, 2147483647L));
        set_repair_cost(res, rc);
        set_enchantments(res, ench, in.is("minecraft:enchanted_book") ? "minecraft:stored_enchantments" : "minecraft:enchantments");
    }
    result.items()[0] = res;
    data_slots[0] = cost;
}

void AnvilMenu::removed(MenuPlayer& player) {
    Menu::removed(player);
    result.clear_content();
    clear_container(player, &input);
}

ItemStack AnvilMenu::quick_move_stack(MenuPlayer& player, int slot_index) {
    ItemStack clicked;
    Slot* slot = slots[size_t(slot_index)].get();
    if (slot->has_item()) {
        ItemStack& stack = slot->get_item();
        clicked = stack.copy();
        if (slot_index == 2) {
            if (!move_item_stack_to(stack, 3, 39, true)) return ItemStack();
            slot->on_quick_craft(stack, clicked);
        } else if (slot_index == 0 || slot_index == 1) {
            if (!move_item_stack_to(stack, 3, 39, false)) return ItemStack();
        } else if (slot_index >= 3 && slot_index < 39) {
            if (!move_item_stack_to(stack, 0, 2, false)) {
                if (slot_index < 30) { if (!move_item_stack_to(stack, 30, 39, false)) return ItemStack(); }
                else if (!move_item_stack_to(stack, 3, 30, false)) return ItemStack();
            }
        }
        if (stack.is_empty()) slot->set_by_player(ItemStack(), clicked);
        else slot->set_changed();
        if (stack.count() == clicked.count()) return ItemStack();
        slot->on_take(player, stack);
    }
    return clicked;
}

// ---- enchanting table

EnchantmentMenu::EnchantmentMenu(int menu_type_id, int container_id, PlayerInventory* inv, MenuPlayer* o, std::function<int()> shelves)
    : Menu(menu_type_id, container_id), owner(o), bookshelves(std::move(shelves)) {
    table.on_changed = [this] { update_offers(); };
    add_slot(std::make_unique<OneItemSlot>(&table, 0));
    add_slot(std::make_unique<LapisSlot>(&table, 1));
    add_standard_inventory_slots(inv, 8, 84);
    data_slots.assign(10, 0);
    data_slots[3] = owner->enchant_seed();
    update_offers();
}

void EnchantmentMenu::update_offers() {
    int seed = data_slots[3];
    ItemStack& item = table.item(0);
    for (int i = 0; i < 3; i++) { data_slots[size_t(i)] = 0; data_slots[size_t(4 + i)] = -1; data_slots[size_t(7 + i)] = -1; }
    if (item.is_empty() || Enchants::enchantability(item) <= 0 || item.is_enchanted()) {
        for (int i = 0; i < 3; i++) { data_slots[size_t(i)] = 0; data_slots[size_t(4 + i)] = -1; data_slots[size_t(7 + i)] = -1; }
        return;
    }
    int power = bookshelves ? bookshelves() : 0;
    vg::LegacyRandom random(seed);
    for (int i = 0; i < 3; i++) {
        data_slots[size_t(i)] = Enchants::cost(random, i, power, item);
        if (data_slots[size_t(i)] < i + 1) data_slots[size_t(i)] = 0;
    }
    for (int i = 0; i < 3; i++) {
        if (data_slots[size_t(i)] <= 0) continue;
        random.set_seed(int64_t(seed) + i);
        auto list = Enchants::get().select(random, item, data_slots[size_t(i)]);
        if (item.is("minecraft:book") && list.size() > 1) list.erase(list.begin() + random.next_int(int(list.size())));
        if (list.empty()) continue;
        const EnchantInstance& pick = list[size_t(random.next_int(int(list.size())))];
        data_slots[size_t(4 + i)] = Registries::get().id("minecraft:enchantment", pick.def->name);
        data_slots[size_t(7 + i)] = pick.level;
    }
}

bool EnchantmentMenu::click_menu_button(MenuPlayer& player, int id) {
    if (id < 0 || id >= 3) return false;
    ItemStack& item = table.item(0);
    ItemStack& lapis = table.item(1);
    int requirement = id + 1;
    bool creative = player.has_infinite_materials();
    if ((lapis.is_empty() || lapis.count() < requirement) && !creative) return false;
    int cost = data_slots[size_t(id)];
    if (cost <= 0 || item.is_empty() || !(creative || (player.experience_level() >= requirement && player.experience_level() >= cost))) return false;
    int seed = data_slots[3];
    vg::LegacyRandom random(int64_t(seed) + id);
    auto list = Enchants::get().select(random, item, cost);
    if (item.is("minecraft:book") && list.size() > 1) list.erase(list.begin() + random.next_int(int(list.size())));
    if (list.empty()) return false;
    if (!creative) player.give_experience_levels(-requirement);
    player.new_enchant_seed();
    ItemStack res = item;
    bool book = item.is("minecraft:book");
    if (book) {
        res = ItemStack(Items::get().by_name("minecraft:enchanted_book"), 1);
        res.patch() = item.patch();
    }
    std::map<std::string, int> m = get_enchantments(res, book ? "minecraft:stored_enchantments" : "minecraft:enchantments");
    for (auto& e : list) m[e.def->name] = e.level;
    set_enchantments(res, m, book ? "minecraft:stored_enchantments" : "minecraft:enchantments");
    table.items()[0] = res;
    if (!creative) { lapis.shrink(requirement); if (lapis.is_empty()) lapis = ItemStack(); }
    data_slots[3] = player.enchant_seed();
    table.set_changed();
    broadcast_changes();
    return true;
}

void EnchantmentMenu::removed(MenuPlayer& player) {
    Menu::removed(player);
    clear_container(player, &table);
}

ItemStack EnchantmentMenu::quick_move_stack(MenuPlayer& player, int slot_index) {
    ItemStack clicked;
    Slot* slot = slots[size_t(slot_index)].get();
    if (slot->has_item()) {
        ItemStack& stack = slot->get_item();
        clicked = stack.copy();
        if (slot_index == 0 || slot_index == 1) {
            if (!move_item_stack_to(stack, 2, 38, true)) return ItemStack();
        } else if (stack.is("minecraft:lapis_lazuli")) {
            if (!move_item_stack_to(stack, 1, 2, true)) return ItemStack();
        } else {
            if (slots[0]->has_item()) return ItemStack();
            ItemStack one = stack.copy_with_count(1);
            stack.shrink(1);
            slots[0]->set_by_player(one);
        }
        if (stack.is_empty()) slot->set_by_player(ItemStack(), clicked);
        else slot->set_changed();
        if (stack.count() == clicked.count()) return ItemStack();
        slot->on_take(player, stack);
    }
    return clicked;
}

}  // namespace pn::sim
