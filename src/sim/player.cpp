#include "player.hpp"

namespace pn::sim {

bool HashedLite::matches(const ItemStack& s) const {
    if (!present) return s.is_empty();
    if (s.is_empty() || s.item()->index != item || s.count() != count) return false;
    return added.empty() && removed.empty() && s.patch().empty();
}

PlayerSim::PlayerSim() {
    inventory_menu = std::make_unique<InventoryMenu>(&inv, this);
    menu = inventory_menu.get();
}

void PlayerSim::handle_container_click(int container_id, int state_id, int slot, int button, ContainerInput input, const std::map<int, HashedLite>& changed, const HashedLite& carried) {
    if (menu->container_id != container_id) return;
    if (gamemode == 3) { menu->send_all_data_to_remote(); return; }
    if (!menu->still_valid(*this)) return;
    if (!menu->is_valid_slot_index(slot)) return;
    bool full_resync = state_id != menu->state_id();
    menu->clicked(slot, button, input, *this);
    // the client predicted a result and sent what it expects to see: what agrees with ours needs no update
    for (auto& kv : changed) {
        int idx = kv.first;
        if (idx < 0 || size_t(idx) >= menu->slots.size()) continue;
        if (kv.second.matches(menu->slots[size_t(idx)]->get_item())) menu->set_remote_slot(idx, menu->slots[size_t(idx)]->get_item());
    }
    if (carried.matches(menu->carried())) menu->set_remote_carried(menu->carried());
    if (full_resync) menu->broadcast_full_state(); else menu->broadcast_changes();
}

void PlayerSim::handle_container_close(int container_id) {
    if (menu->container_id != container_id) return;
    menu->removed(*this);
    menu = inventory_menu.get();
}

bool PlayerSim::handle_creative_slot(int slot, const ItemStack& stack) {
    if (!has_infinite_materials()) return false;
    bool drop_it = slot < 0;
    bool valid_slot = slot >= 1 && slot <= 45;
    bool valid_data = stack.is_empty() || stack.count() <= stack.max_stack_size();
    if (valid_slot && valid_data) {
        inventory_menu->slots[size_t(slot)]->set_by_player(stack);
        inventory_menu->set_remote_slot(slot, stack);
        inventory_menu->broadcast_changes();
        return true;
    }
    if (drop_it && valid_data) { drop(stack, true); return true; }
    return false;
}

}  // namespace pn::sim
