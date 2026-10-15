// The part of a player the gameplay rules act on (inventory, open menu, game mode), independent of the network protocol.
#pragma once
#include "inventory.hpp"
#include <functional>
#include <map>

namespace pn::sim {

// A stack as the client describes it in a container click: item, count and a hash per changed component (HashedStack)
struct HashedLite {
    bool present = false;
    int item = 0;
    int count = 0;
    std::map<int, int32_t> added;
    std::set<int> removed;
    bool matches(const ItemStack& s) const;   // true when the stack has no component patch to compare and item and count agree
};

class PlayerSim : public MenuPlayer {
public:
    PlayerSim();
    PlayerInventory inv{this};
    std::unique_ptr<InventoryMenu> inventory_menu;
    Menu* menu = nullptr;           // the menu the player has open (the inventory menu when none)
    int gamemode = 0;               // 0 survival, 1 creative, 2 adventure, 3 spectator
    std::function<void(const ItemStack&, bool)> drop_hook;
    std::function<void(EquipmentSlot, const ItemStack&, const ItemStack&)> equip_hook;

    void drop(const ItemStack& stack, bool throw_randomly) override { if (drop_hook && !stack.is_empty()) drop_hook(stack, throw_randomly); }
    bool has_infinite_materials() const override { return gamemode == 1; }
    bool can_drop_items() const override { return gamemode != 3; }
    PlayerInventory& inventory() override { return inv; }
    void on_equip_item(EquipmentSlot s, const ItemStack& prev, const ItemStack& now) override { if (equip_hook) equip_hook(s, prev, now); }

    // ServerGamePacketListenerImpl.handleContainerClick
    void handle_container_click(int container_id, int state_id, int slot, int button, ContainerInput input, const std::map<int, HashedLite>& changed, const HashedLite& carried);
    void handle_container_close(int container_id);
    void handle_set_carried_item(int slot) { if (slot >= 0 && slot < 9) inv.selected = slot; }
    // ServerGamePacketListenerImpl.handleSetCreativeModeSlot (slot < 0 drops the item); false when it was refused
    bool handle_creative_slot(int slot, const ItemStack& stack);
    ItemStack& held_item() { return inv.selected_item(); }
};

}  // namespace pn::sim
