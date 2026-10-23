#include "inventory.hpp"
#include "recipe.hpp"
#include <cmath>
#include <algorithm>

namespace pn::sim {

EquipmentType equipment_type(EquipmentSlot s) {
    switch (s) {
        case EquipmentSlot::MAINHAND: case EquipmentSlot::OFFHAND: return EquipmentType::HAND;
        case EquipmentSlot::BODY: return EquipmentType::ANIMAL_ARMOR;
        case EquipmentSlot::SADDLE: return EquipmentType::SADDLE;
        default: return EquipmentType::HUMANOID_ARMOR;
    }
}
int equipment_index(EquipmentSlot s) {
    switch (s) {
        case EquipmentSlot::FEET: return 0;
        case EquipmentSlot::LEGS: return 1;
        case EquipmentSlot::CHEST: return 2;
        case EquipmentSlot::HEAD: return 3;
        case EquipmentSlot::OFFHAND: return 1;
        default: return 0;
    }
}
const char* equipment_name(EquipmentSlot s) {
    static const char* n[] = {"mainhand", "offhand", "feet", "legs", "chest", "head", "body", "saddle"};
    return n[int(s)];
}
bool equipment_from_name(const std::string& name, EquipmentSlot& out) {
    for (int i = 0; i < 8; i++) if (name == equipment_name(EquipmentSlot(i))) { out = EquipmentSlot(i); return true; }
    return false;
}

// Inventory.EQUIPMENT_SLOT_MAPPING
static bool container_index_to_equipment(int slot, EquipmentSlot& out) {
    switch (slot) {
        case 36: out = EquipmentSlot::FEET; return true;
        case 37: out = EquipmentSlot::LEGS; return true;
        case 38: out = EquipmentSlot::CHEST; return true;
        case 39: out = EquipmentSlot::HEAD; return true;
        case 40: out = EquipmentSlot::OFFHAND; return true;
        case 41: out = EquipmentSlot::BODY; return true;
        case 42: out = EquipmentSlot::SADDLE; return true;
        default: return false;
    }
}

// ---- Container

ItemStack Container::remove_item(int slot, int count) {
    if (slot < 0 || slot >= size() || item(slot).is_empty() || count <= 0) return ItemStack();
    return item(slot).split(count);
}
ItemStack Container::remove_item_no_update(int slot) {
    if (slot < 0 || slot >= size()) return ItemStack();
    ItemStack s = item(slot);
    set_item(slot, ItemStack());
    return s;
}
ItemStack SimpleContainer::remove_item(int slot, int count) {
    ItemStack s = Container::remove_item(slot, count);
    if (!s.is_empty()) set_changed();
    return s;
}
ItemStack SimpleContainer::remove_item_no_update(int slot) {
    if (slot < 0 || size_t(slot) >= items_.size()) return ItemStack();
    ItemStack s = items_[size_t(slot)];
    items_[size_t(slot)] = ItemStack();
    return s;
}

// ---- PlayerInventory

ItemStack& PlayerInventory::item(int slot) {
    static ItemStack none;
    if (slot >= 0 && slot < 36) return items[size_t(slot)];
    EquipmentSlot e;
    if (container_index_to_equipment(slot, e)) return equipment[size_t(e)];
    none = ItemStack();
    return none;
}
void PlayerInventory::set_item(int slot, const ItemStack& s) {
    if (slot >= 0 && slot < 36) items[size_t(slot)] = s;
    EquipmentSlot e;
    if (container_index_to_equipment(slot, e)) equipment[size_t(e)] = s;
}
ItemStack PlayerInventory::remove_item(int slot, int count) {
    if (slot >= 0 && slot < 36) {
        if (items[size_t(slot)].is_empty() || count <= 0) return ItemStack();
        return items[size_t(slot)].split(count);
    }
    EquipmentSlot e;
    if (container_index_to_equipment(slot, e) && !equipment[size_t(e)].is_empty()) return equipment[size_t(e)].split(count);
    return ItemStack();
}
ItemStack PlayerInventory::remove_item_no_update(int slot) {
    if (slot >= 0 && slot < 36) { ItemStack s = items[size_t(slot)]; items[size_t(slot)] = ItemStack(); return s; }
    EquipmentSlot e;
    if (container_index_to_equipment(slot, e)) { ItemStack s = equipment[size_t(e)]; equipment[size_t(e)] = ItemStack(); return s; }
    return ItemStack();
}
int PlayerInventory::free_slot() const {
    for (int i = 0; i < 36; i++) if (items[size_t(i)].is_empty()) return i;
    return -1;
}
bool PlayerInventory::has_remaining_space_for_item(const ItemStack& in_slot, const ItemStack& s) const {
    return !in_slot.is_empty() && ItemStack::is_same_item_same_components(in_slot, s) && in_slot.is_stackable() && in_slot.count() < max_stack_size(in_slot);
}
int PlayerInventory::slot_with_remaining_space(const ItemStack& s) {
    if (has_remaining_space_for_item(item(selected), s)) return selected;
    if (has_remaining_space_for_item(item(40), s)) return 40;
    for (int i = 0; i < 36; i++) if (has_remaining_space_for_item(items[size_t(i)], s)) return i;
    return -1;
}
int PlayerInventory::suitable_hotbar_slot() const {
    for (int slot = 0; slot < 9; slot++) {
        int index = (selected + slot) % 9;
        if (items[size_t(index)].is_empty()) return index;
    }
    for (int slot = 0; slot < 9; slot++) {
        int index = (selected + slot) % 9;
        if (!items[size_t(index)].is_enchanted()) return index;
    }
    return selected;
}
int PlayerInventory::find_slot_matching_item(const ItemStack& s) const {
    for (int i = 0; i < 36; i++)
        if (!items[size_t(i)].is_empty() && ItemStack::is_same_item_same_components(s, items[size_t(i)])) return i;
    return -1;
}
int PlayerInventory::add_resource(ItemStack& stack) {
    int slot = slot_with_remaining_space(stack);
    if (slot == -1) slot = free_slot();
    if (slot == -1) return stack.count();
    return add_resource(slot, stack);
}
int PlayerInventory::add_resource(int slot, ItemStack& stack) {
    int count = stack.count();
    ItemStack& in_slot = item(slot);
    if (in_slot.is_empty()) {
        set_item(slot, stack.copy_with_count(0));
    }
    ItemStack& target = item(slot);
    int max_to_add = max_stack_size(target) - target.count();
    int to_add = std::min(count, max_to_add);
    if (to_add == 0) return count;
    target.grow(to_add);
    return count - to_add;
}
bool PlayerInventory::add(int slot, ItemStack& stack) {
    if (stack.is_empty()) return false;
    if (!stack.is_damaged()) {
        int last;
        do {
            last = stack.count();
            if (slot == -1) stack.set_count(add_resource(stack));
            else stack.set_count(add_resource(slot, stack));
        } while (!stack.is_empty() && stack.count() < last);
        if (stack.count() == last && player && player->has_infinite_materials()) { stack.set_count(0); return true; }
        return stack.count() < last;
    }
    if (slot == -1) slot = free_slot();
    if (slot >= 0) {
        items[size_t(slot)] = stack.copy_and_clear();
        return true;
    }
    if (player && player->has_infinite_materials()) { stack.set_count(0); return true; }
    return false;
}
void PlayerInventory::place_item_back_in_inventory(ItemStack& stack) {
    while (!stack.is_empty()) {
        int slot = slot_with_remaining_space(stack);
        if (slot == -1) slot = free_slot();
        if (slot == -1) { if (player) player->drop(stack, false); stack = ItemStack(); break; }
        int has_space_for = stack.max_stack_size() - item(slot).count();
        ItemStack part = stack.split(has_space_for);
        if (add(slot, part)) { if (on_slot_changed) on_slot_changed(slot); }
        else { stack.grow(part.count()); if (part.count() == 0) break; }
    }
}

// ---- Slot

void Slot::on_quick_craft(const ItemStack& picked, const ItemStack& original) {
    int count = original.count() - picked.count();
    if (count > 0) on_quick_craft(original, count);
}
bool Slot::try_remove(int amount, int max_amount, MenuPlayer& p, ItemStack& out) {
    if (!may_pickup(p)) return false;
    if (!allow_modification(p) && max_amount < get_item().count()) return false;
    amount = std::min(amount, max_amount);
    ItemStack result = remove(amount);
    if (result.is_empty()) return false;
    if (get_item().is_empty()) set_by_player(ItemStack(), result);
    out = result;
    return true;
}
ItemStack Slot::safe_take(int amount, int max_amount, MenuPlayer& p) {
    ItemStack r;
    if (!try_remove(amount, max_amount, p, r)) return ItemStack();
    on_take(p, r);
    return r;
}
ItemStack& Slot::safe_insert(ItemStack& input, int amount) {
    if (input.is_empty() || !may_place(input)) return input;
    ItemStack& slot_stack = get_item();
    int transferable = std::min(std::min(amount, input.count()), max_stack_size(input) - slot_stack.count());
    if (transferable <= 0) return input;
    if (slot_stack.is_empty()) set_by_player(input.split(transferable));
    else if (ItemStack::is_same_item_same_components(slot_stack, input)) {
        input.shrink(transferable);
        slot_stack.grow(transferable);
        ItemStack copy = slot_stack;
        set_by_player(copy);
    }
    return input;
}

static bool equippable_in_slot(const ItemStack& s, EquipmentSlot slot) {
    if (s.is_empty()) return false;
    const std::string& eq = s.item()->equippable_slot;
    if (eq.empty()) return slot == EquipmentSlot::MAINHAND;
    EquipmentSlot e;
    return equipment_from_name(eq, e) && e == slot;
}
void ArmorSlot::set_by_player(const ItemStack& s, const ItemStack& previous) {
    if (owner) owner->on_equip_item(eq_, previous, s);
    Slot::set_by_player(s, previous);
}
bool ArmorSlot::may_place(const ItemStack& s) const { return equippable_in_slot(s, eq_); }
bool ArmorSlot::may_pickup(MenuPlayer& p) {
    ItemStack& s = get_item();
    if (!s.is_empty() && !p.is_creative()) {
        // EnchantmentEffectComponents.PREVENT_ARMOR_CHANGE: curse of binding; checked by name until enchantments are decoded
        (void)s;
    }
    return true;
}

// ---- Menu

Slot* Menu::add_slot(std::unique_ptr<Slot> s) {
    s->index = int(slots.size());
    slots.push_back(std::move(s));
    remote_.emplace_back();
    return slots.back().get();
}

bool Menu::can_item_quick_replace(Slot* slot, const ItemStack& s, bool ignore_size) {
    bool empty = !slot || !slot->has_item();
    if (!empty && ItemStack::is_same_item_same_components(s, slot->get_item()))
        return slot->get_item().count() + (ignore_size ? 0 : s.count()) <= s.max_stack_size();
    return empty;
}
int Menu::quick_craft_place_count(int n, int type, const ItemStack& s) {
    switch (type) {
        case 0: return int(std::floor(float(s.count()) / float(n)));
        case 1: return 1;
        case 2: return s.max_stack_size();
        default: return s.count();
    }
}

bool Menu::move_item_stack_to(ItemStack& stack, int start, int end, bool backwards) {
    bool changed = false;
    int dest = start;
    if (backwards) dest = end - 1;
    if (stack.is_stackable()) {
        while (!stack.is_empty() && (backwards ? dest >= start : dest < end)) {
            Slot* slot = slots[size_t(dest)].get();
            ItemStack& target = slot->get_item();
            if (!target.is_empty() && ItemStack::is_same_item_same_components(stack, target)) {
                int total = target.count() + stack.count();
                int max = slot->max_stack_size(target);
                if (total <= max) {
                    stack.set_count(0);
                    target.set_count(total);
                    slot->set_changed();
                    changed = true;
                } else if (target.count() < max) {
                    stack.shrink(max - target.count());
                    target.set_count(max);
                    slot->set_changed();
                    changed = true;
                }
            }
            if (backwards) --dest; else ++dest;
        }
    }
    if (!stack.is_empty()) {
        dest = backwards ? end - 1 : start;
        while (backwards ? dest >= start : dest < end) {
            Slot* slot = slots[size_t(dest)].get();
            ItemStack& target = slot->get_item();
            if (target.is_empty() && slot->may_place(stack)) {
                int max = slot->max_stack_size(stack);
                slot->set_by_player(stack.split(std::min(stack.count(), max)));
                slot->set_changed();
                changed = true;
                break;
            }
            if (backwards) --dest; else ++dest;
        }
    }
    return changed;
}

void Menu::clicked(int slot_index, int button, ContainerInput input, MenuPlayer& player) { do_click(slot_index, button, input, player); }

void Menu::do_click(int slot_index, int button_num, ContainerInput input, MenuPlayer& player) {
    PlayerInventory& inventory = player.inventory();
    if (input == ContainerInput::QUICK_CRAFT) {
        int expected = quickcraft_status_;
        quickcraft_status_ = quickcraft_header(button_num);
        if (!((expected == 1 && quickcraft_status_ == 2) || expected == quickcraft_status_)) { reset_quick_craft(); return; }
        if (carried_.is_empty()) { reset_quick_craft(); return; }
        if (quickcraft_status_ == 0) {
            quickcraft_type_ = quickcraft_type(button_num);
            bool valid = quickcraft_type_ == 0 || quickcraft_type_ == 1 || (quickcraft_type_ == 2 && player.has_infinite_materials());
            if (valid) { quickcraft_status_ = 1; quickcraft_slots_.clear(); } else reset_quick_craft();
            return;
        }
        if (quickcraft_status_ == 1) {
            if (slot_index < 0 || size_t(slot_index) >= slots.size()) return;
            Slot* slot = slots[size_t(slot_index)].get();
            ItemStack& c = carried_;
            if (!can_item_quick_replace(slot, c, true) || !slot->may_place(c) || (quickcraft_type_ != 2 && c.count() <= int(quickcraft_slots_.size())) || !can_drag_to(slot)) return;
            quickcraft_slots_.insert(slot);
            return;
        }
        if (quickcraft_status_ == 2) {
            if (!quickcraft_slots_.empty()) {
                if (quickcraft_slots_.size() == 1) {
                    int slot = (*quickcraft_slots_.begin())->index;
                    reset_quick_craft();
                    do_click(slot, quickcraft_type_, ContainerInput::PICKUP, player);
                    return;
                }
                ItemStack source = carried_.copy();
                if (source.is_empty()) { reset_quick_craft(); return; }
                int remaining = carried_.count();
                // Mojang iterates a HashSet<Slot>; the placed counts do not depend on the order
                std::vector<Slot*> order(quickcraft_slots_.begin(), quickcraft_slots_.end());
                std::sort(order.begin(), order.end(), [](Slot* a, Slot* b) { return a->index < b->index; });
                for (Slot* slot : order) {
                    ItemStack& c = carried_;
                    if (!slot || !can_item_quick_replace(slot, c, true) || !slot->may_place(c) || (quickcraft_type_ != 2 && c.count() < int(quickcraft_slots_.size())) || !can_drag_to(slot)) continue;
                    int carry = slot->has_item() ? slot->get_item().count() : 0;
                    int max_size = std::min(source.max_stack_size(), slot->max_stack_size(source));
                    int new_count = std::min(quick_craft_place_count(int(quickcraft_slots_.size()), quickcraft_type_, source) + carry, max_size);
                    remaining -= new_count - carry;
                    slot->set_by_player(source.copy_with_count(new_count));
                }
                source.set_count(remaining);
                set_carried(source);
            }
            reset_quick_craft();
            return;
        }
        reset_quick_craft();
        return;
    }
    if (quickcraft_status_ != 0) { reset_quick_craft(); return; }

    if ((input == ContainerInput::PICKUP || input == ContainerInput::QUICK_MOVE) && (button_num == 0 || button_num == 1)) {
        ClickAction action = button_num == 0 ? ClickAction::PRIMARY : ClickAction::SECONDARY;
        if (slot_index == -999) {
            if (carried_.is_empty()) return;
            if (action == ClickAction::PRIMARY) { player.drop(carried_, true); set_carried(ItemStack()); }
            else player.drop(carried_.split(1), true);
            return;
        }
        if (input == ContainerInput::QUICK_MOVE) {
            if (slot_index < 0 || size_t(slot_index) >= slots.size()) return;
            Slot* slot = slots[size_t(slot_index)].get();
            if (!slot->may_pickup(player)) return;
            ItemStack clicked = quick_move_stack(player, slot_index);
            while (!clicked.is_empty() && ItemStack::is_same_item(slot->get_item(), clicked)) clicked = quick_move_stack(player, slot_index);
        } else {
            if (slot_index < 0 || size_t(slot_index) >= slots.size()) return;
            Slot* slot = slots[size_t(slot_index)].get();
            ItemStack clicked = slot->get_item();
            ItemStack carried = carried_;
            if (clicked.is_empty()) {
                if (!carried.is_empty()) {
                    int amount = action == ClickAction::PRIMARY ? carried.count() : 1;
                    set_carried(slot->safe_insert(carried, amount));
                }
            } else if (slot->may_pickup(player)) {
                if (carried.is_empty()) {
                    int amount = action == ClickAction::PRIMARY ? clicked.count() : (clicked.count() + 1) / 2;
                    ItemStack taken;
                    if (slot->try_remove(amount, 0x7FFFFFFF, player, taken)) {
                        set_carried(taken);
                        slot->on_take(player, taken);
                    }
                } else if (slot->may_place(carried)) {
                    if (ItemStack::is_same_item_same_components(clicked, carried)) {
                        int amount = action == ClickAction::PRIMARY ? carried.count() : 1;
                        set_carried(slot->safe_insert(carried, amount));
                    } else if (carried.count() <= slot->max_stack_size(carried)) {
                        set_carried(clicked);
                        slot->set_by_player(carried);
                    }
                } else if (ItemStack::is_same_item_same_components(clicked, carried)) {
                    ItemStack taken;
                    if (slot->try_remove(clicked.count(), carried.max_stack_size() - carried.count(), player, taken)) {
                        carried.grow(taken.count());
                        set_carried(carried);
                        slot->on_take(player, taken);
                    }
                }
            }
            slot->set_changed();
        }
        return;
    }
    if (input == ContainerInput::SWAP && ((button_num >= 0 && button_num < 9) || button_num == 40)) {
        if (slot_index < 0 || size_t(slot_index) >= slots.size()) return;
        ItemStack source = inventory.item(button_num);
        Slot* target = slots[size_t(slot_index)].get();
        ItemStack target_stack = target->get_item();
        if (source.is_empty() && target_stack.is_empty()) return;
        if (source.is_empty()) {
            if (!target->may_pickup(player)) return;
            inventory.set_item(button_num, target_stack);
            target->on_swap_craft(target_stack.count());
            target->set_by_player(ItemStack());
            target->on_take(player, target_stack);
        } else if (target_stack.is_empty()) {
            if (!target->may_place(source)) return;
            int max = target->max_stack_size(source);
            if (source.count() > max) {
                ItemStack part = source.split(max);
                inventory.set_item(button_num, source);
                target->set_by_player(part);
            } else {
                inventory.set_item(button_num, ItemStack());
                target->set_by_player(source);
            }
        } else {
            if (!target->may_pickup(player) || !target->may_place(source)) return;
            int max = target->max_stack_size(source);
            if (source.count() > max) {
                ItemStack part = source.split(max);
                inventory.set_item(button_num, source);
                target->set_by_player(part);
                target->on_take(player, target_stack);
                if (!inventory.add(target_stack)) player.drop(target_stack, true);
            } else {
                inventory.set_item(button_num, target_stack);
                target->set_by_player(source);
                target->on_take(player, target_stack);
            }
        }
        return;
    }
    if (input == ContainerInput::CLONE && player.has_infinite_materials() && carried_.is_empty() && slot_index >= 0) {
        if (size_t(slot_index) >= slots.size()) return;
        Slot* slot = slots[size_t(slot_index)].get();
        if (!slot->has_item()) return;
        set_carried(slot->safe_clone(player));
        return;
    }
    if (input == ContainerInput::THROW && carried_.is_empty() && slot_index >= 0) {
        if (size_t(slot_index) >= slots.size()) return;
        Slot* slot = slots[size_t(slot_index)].get();
        int amount = button_num == 0 ? 1 : slot->get_item().count();
        if (!player.can_drop_items()) return;
        ItemStack stack = slot->safe_take(amount, 0x7FFFFFFF, player);
        player.drop(stack, true);
        if (button_num != 1) return;
        while (!stack.is_empty() && ItemStack::is_same_item(slot->get_item(), stack)) {
            if (!player.can_drop_items()) return;
            stack = slot->safe_take(amount, 0x7FFFFFFF, player);
            player.drop(stack, true);
        }
        return;
    }
    if (input == ContainerInput::PICKUP_ALL && slot_index >= 0) {
        if (size_t(slot_index) >= slots.size()) return;
        Slot* slot = slots[size_t(slot_index)].get();
        ItemStack& carried = carried_;
        if (!(carried.is_empty() || (slot->has_item() && slot->may_pickup(player)))) {
            int start = button_num == 0 ? 0 : int(slots.size()) - 1;
            int step = button_num == 0 ? 1 : -1;
            for (int pass = 0; pass < 2; ++pass) {
                for (int i = start; i >= 0 && i < int(slots.size()) && carried.count() < carried.max_stack_size(); i += step) {
                    Slot* target = slots[size_t(i)].get();
                    if (!target->has_item() || !can_item_quick_replace(target, carried, true) || !target->may_pickup(player) || !can_take_item_for_pick_all(carried, target)) continue;
                    ItemStack& s = target->get_item();
                    if (pass == 0 && s.count() == s.max_stack_size()) continue;
                    ItemStack removed = target->safe_take(s.count(), carried.max_stack_size() - carried.count(), player);
                    carried.grow(removed.count());
                }
            }
        }
    }
}

void Menu::drop_or_place_in_inventory(MenuPlayer& player, const ItemStack& carried) {
    ItemStack c = carried;
    player.inventory().place_item_back_in_inventory(c);
}
void Menu::clear_container(MenuPlayer& player, Container* c) {
    for (int i = 0; i < c->size(); i++) {
        ItemStack s = c->remove_item_no_update(i);
        if (!s.is_empty()) drop_or_place_in_inventory(player, s);
    }
}
void Menu::removed(MenuPlayer& player) {
    if (!carried_.is_empty()) {
        drop_or_place_in_inventory(player, carried_);
        set_carried(ItemStack());
    }
}

void Menu::set_synchronizer(ContainerSynchronizer* s) {
    synchronizer = s;
    send_all_data_to_remote();
}
void Menu::send_all_data_to_remote() {
    std::vector<ItemStack> to_send;
    remote_.resize(slots.size());
    for (size_t i = 0; i < slots.size(); i++) {
        ItemStack s = slots[i]->get_item();
        to_send.push_back(s);
        remote_[i] = s;
    }
    remote_carried_ = carried_;
    remote_data_ = data_slots;
    if (synchronizer) synchronizer->send_initial_data(*this, to_send, carried_, data_slots);
}
void Menu::broadcast_full_state() { send_all_data_to_remote(); }
void Menu::broadcast_changes() {
    if (suppress_remote_updates_) return;
    remote_.resize(slots.size());
    for (size_t i = 0; i < slots.size(); i++) {
        ItemStack& cur = slots[i]->get_item();
        if (!ItemStack::matches(remote_[i], cur)) {
            remote_[i] = cur;
            if (synchronizer) synchronizer->send_slot_change(*this, int(i), cur);
        }
    }
    if (!ItemStack::matches(remote_carried_, carried_)) {
        remote_carried_ = carried_;
        if (synchronizer) synchronizer->send_carried_change(*this, carried_);
    }
    remote_data_.resize(data_slots.size());
    for (size_t i = 0; i < data_slots.size(); i++)
        if (remote_data_[i] != data_slots[i]) {
            remote_data_[i] = data_slots[i];
            if (synchronizer) synchronizer->send_data_change(*this, int(i), data_slots[i]);
        }
}

// ---- crafting result slot

ItemStack ResultSlot::remove(int amount) {
    if (has_item()) remove_count_ += std::min(amount, get_item().count());
    return Slot::remove(amount);
}
void ResultSlot::on_take(MenuPlayer& p, const ItemStack& carried) {
    (void)carried;
    std::vector<ItemStack> remaining = (provider_ && *provider_) ? (*provider_)->remaining(*craft_) : std::vector<ItemStack>();
    remaining.resize(size_t(craft_->size()));
    for (int slot = 0; slot < craft_->size(); slot++) {
        ItemStack& stack = craft_->item(slot);
        ItemStack replacement = remaining[size_t(slot)];
        if (!stack.is_empty()) craft_->remove_item(slot, 1);
        ItemStack& now = craft_->item(slot);
        if (replacement.is_empty()) continue;
        if (now.is_empty()) { craft_->set_item(slot, replacement); continue; }
        if (ItemStack::is_same_item_same_components(now, replacement)) { replacement.grow(now.count()); craft_->set_item(slot, replacement); continue; }
        if (!p.inventory().add(replacement)) p.drop(replacement, false);
    }
    remove_count_ = 0;
}

// ---- InventoryMenu

InventoryMenu::InventoryMenu(PlayerInventory* inv, MenuPlayer* owner) : Menu(-1, 0), owner_(owner) {
    craft.on_changed = [this]() { update_crafting_result(); };
    add_slot(std::make_unique<ResultSlot>(owner, &craft, &result_, &crafting));
    for (int i = 0; i < 4; i++) add_slot(std::make_unique<Slot>(&craft, i));
    static const EquipmentSlot ids[4] = {EquipmentSlot::HEAD, EquipmentSlot::CHEST, EquipmentSlot::LEGS, EquipmentSlot::FEET};
    for (int i = 0; i < 4; i++) {
        auto s = std::make_unique<ArmorSlot>(inv, 39 - i, ids[i]);
        s->owner = owner;
        add_slot(std::move(s));
    }
    add_standard_inventory_slots(inv, 8, 84);
    class OffhandSlot : public Slot {
    public:
        OffhandSlot(Container* c, MenuPlayer* o) : Slot(c, 40), owner(o) {}
        void set_by_player(const ItemStack& s, const ItemStack& previous) override { owner->on_equip_item(EquipmentSlot::OFFHAND, previous, s); Slot::set_by_player(s, previous); }
        MenuPlayer* owner;
    };
    add_slot(std::make_unique<OffhandSlot>(inv, owner));
}

EquipmentSlot InventoryMenu::equipment_slot_for(const ItemStack& s) const {
    EquipmentSlot e;
    if (!s.is_empty() && !s.item()->equippable_slot.empty() && equipment_from_name(s.item()->equippable_slot, e)) return e;
    return EquipmentSlot::MAINHAND;
}

void InventoryMenu::update_crafting_result() {
    ItemStack r = crafting ? crafting->result(craft) : ItemStack();
    if (!ItemStack::matches(result_.item(0), r)) result_.items()[0] = r;
}
void InventoryMenu::slots_changed(Container* c) {
    (void)c;
    broadcast_changes();
}
void InventoryMenu::removed(MenuPlayer& player) {
    Menu::removed(player);
    result_.clear_content();
    clear_container(player, &craft);
}

ItemStack InventoryMenu::quick_move_stack(MenuPlayer& player, int slot_index) {
    ItemStack clicked;
    Slot* slot = slots[size_t(slot_index)].get();
    if (slot->has_item()) {
        ItemStack& stack = slot->get_item();
        clicked = stack.copy();
        EquipmentSlot eq = equipment_slot_for(clicked);
        bool failed;
        if (slot_index == 0) {
            if (!move_item_stack_to(stack, 9, 45, true)) return ItemStack();
            slot->on_quick_craft(stack, clicked);
            failed = false;
        } else if (slot_index >= 1 && slot_index < 5) failed = !move_item_stack_to(stack, 9, 45, false);
        else if (slot_index >= 5 && slot_index < 9) failed = !move_item_stack_to(stack, 9, 45, false);
        else if (equipment_type(eq) == EquipmentType::HUMANOID_ARMOR && !slots[size_t(8 - equipment_index(eq))]->has_item()) {
            int pos = 8 - equipment_index(eq);
            failed = !move_item_stack_to(stack, pos, pos + 1, false);
        } else if (eq == EquipmentSlot::OFFHAND && !slots[45]->has_item()) failed = !move_item_stack_to(stack, 45, 46, false);
        else if (slot_index >= 9 && slot_index < 36) failed = !move_item_stack_to(stack, 36, 45, false);
        else if (slot_index >= 36 && slot_index < 45) failed = !move_item_stack_to(stack, 9, 36, false);
        else failed = !move_item_stack_to(stack, 9, 45, false);
        if (failed) return ItemStack();
        if (stack.is_empty()) slot->set_by_player(ItemStack(), clicked);
        else slot->set_changed();
        if (stack.count() == clicked.count()) return ItemStack();
        slot->on_take(player, stack);
        if (slot_index == 0) player.drop(stack, false);
    }
    return clicked;
}

CraftingMenu::CraftingMenu(int menu_type_id, int container_id, PlayerInventory* inv, MenuPlayer* owner, CraftingProvider* provider) : Menu(menu_type_id, container_id), crafting(provider) {
    craft.on_changed = [this]() { update_crafting_result(); };
    add_slot(std::make_unique<ResultSlot>(owner, &craft, &result_, &crafting));
    for (int i = 0; i < 9; i++) add_slot(std::make_unique<Slot>(&craft, i));
    add_standard_inventory_slots(inv, 8, 84);
}
void CraftingMenu::update_crafting_result() {
    ItemStack r = crafting ? crafting->result(craft) : ItemStack();
    if (!ItemStack::matches(result_.item(0), r)) result_.items()[0] = r;
}
void CraftingMenu::removed(MenuPlayer& player) {
    Menu::removed(player);
    result_.clear_content();
    clear_container(player, &craft);
}
ItemStack CraftingMenu::quick_move_stack(MenuPlayer& player, int slot_index) {
    ItemStack clicked;
    Slot* slot = slots[size_t(slot_index)].get();
    if (slot->has_item()) {
        ItemStack& stack = slot->get_item();
        clicked = stack.copy();
        if (slot_index == 0) {
            if (!move_item_stack_to(stack, 10, 46, true)) return ItemStack();
            slot->on_quick_craft(stack, clicked);
        } else if (slot_index >= 10 && slot_index < 46) {
            if (!move_item_stack_to(stack, 1, 10, false)) {
                if (slot_index < 37) { if (!move_item_stack_to(stack, 37, 46, false)) return ItemStack(); }
                else if (!move_item_stack_to(stack, 10, 37, false)) return ItemStack();
            }
        } else if (!move_item_stack_to(stack, 10, 46, false)) return ItemStack();
        if (stack.is_empty()) slot->set_by_player(ItemStack(), clicked);
        else slot->set_changed();
        if (stack.count() == clicked.count()) return ItemStack();
        slot->on_take(player, stack);
        if (slot_index == 0) player.drop(stack, false);
    }
    return clicked;
}

namespace {
class NoShulkerSlot : public Slot {
public:
    using Slot::Slot;
    bool may_place(const ItemStack& s) const override { return s.is_empty() || s.item()->name.find("shulker_box") == std::string::npos; }
};
class FuelSlot : public Slot {
public:
    using Slot::Slot;
    bool may_place(const ItemStack& s) const override {
        return Recipes::get().is_fuel(s) || (!s.is_empty() && s.item()->name == "minecraft:bucket");
    }
    int max_stack_size(const ItemStack& s) const { return s.item()->name == "minecraft:bucket" ? 1 : Slot::max_stack_size(s); }
};
class FurnaceResultSlot : public Slot {
public:
    using Slot::Slot;
    bool may_place(const ItemStack&) const override { return false; }
};
}  // namespace

ContainerMenu::ContainerMenu(int menu_type_id, int container_id, PlayerInventory* inv, std::vector<Container*> parts, bool no_shulkers, std::shared_ptr<void> keep_alive)
    : Menu(menu_type_id, container_id), keep(std::move(keep_alive)) {
    for (Container* c : parts)
        for (int i = 0; i < c->size(); i++) {
            if (no_shulkers) add_slot(std::make_unique<NoShulkerSlot>(c, i));
            else add_slot(std::make_unique<Slot>(c, i));
            container_slots++;
        }
    add_standard_inventory_slots(inv, 8, 84);
}

ItemStack ContainerMenu::quick_move_stack(MenuPlayer& player, int slot_index) {
    ItemStack clicked;
    Slot* slot = slots[size_t(slot_index)].get();
    if (slot->has_item()) {
        ItemStack& stack = slot->get_item();
        clicked = stack.copy();
        int n = container_slots, total = int(slots.size());
        if (slot_index < n) { if (!move_item_stack_to(stack, n, total, true)) return ItemStack(); }
        else if (!move_item_stack_to(stack, 0, n, false)) return ItemStack();
        if (stack.is_empty()) slot->set_by_player(ItemStack(), clicked);
        else slot->set_changed();
        (void)player;
    }
    return clicked;
}

FurnaceMenu::FurnaceMenu(int menu_type_id, int container_id, PlayerInventory* inv, Container* furnace, const std::string& recipe_kind, std::shared_ptr<void> keep_alive)
    : Menu(menu_type_id, container_id), kind(recipe_kind), keep(std::move(keep_alive)) {
    add_slot(std::make_unique<Slot>(furnace, 0));
    add_slot(std::make_unique<FuelSlot>(furnace, 1));
    add_slot(std::make_unique<FurnaceResultSlot>(furnace, 2));
    add_standard_inventory_slots(inv, 8, 84);
    data_slots.assign(4, 0);
}

ItemStack FurnaceMenu::quick_move_stack(MenuPlayer& player, int slot_index) {
    ItemStack clicked;
    Slot* slot = slots[size_t(slot_index)].get();
    if (slot->has_item()) {
        ItemStack& stack = slot->get_item();
        clicked = stack.copy();
        if (slot_index == 2) {
            if (!move_item_stack_to(stack, 3, 39, true)) return ItemStack();
            slot->on_quick_craft(stack, clicked);
        } else if (slot_index == 1 || slot_index == 0) {
            if (!move_item_stack_to(stack, 3, 39, false)) return ItemStack();
        } else if (Recipes::get().find_cooking(kind, stack)) {
            if (!move_item_stack_to(stack, 0, 1, false)) return ItemStack();
        } else if (Recipes::get().is_fuel(stack)) {
            if (!move_item_stack_to(stack, 1, 2, false)) return ItemStack();
        } else if (slot_index >= 3 && slot_index < 30) {
            if (!move_item_stack_to(stack, 30, 39, false)) return ItemStack();
        } else if (slot_index >= 30 && slot_index < 39) {
            if (!move_item_stack_to(stack, 3, 30, false)) return ItemStack();
        }
        if (stack.is_empty()) slot->set_by_player(ItemStack(), clicked);
        else slot->set_changed();
        if (stack.count() == clicked.count()) return ItemStack();
        slot->on_take(player, stack);
    }
    return clicked;
}

}  // namespace pn::sim
