// Menus of the workstations that change items: the anvil and the enchanting table (net.minecraft.world.inventory.{AnvilMenu, EnchantmentMenu}).
#pragma once
#include "inventory.hpp"

namespace pn::sim {

// slots 0 and 1 the inputs, 2 the result, then the player's 27 + 9; data slot 0 is the level cost
class AnvilMenu : public Menu {
public:
    AnvilMenu(int menu_type_id, int container_id, PlayerInventory* inv, MenuPlayer* owner);
    ItemStack quick_move_stack(MenuPlayer& player, int slot) override;
    void slots_changed(Container*) override { broadcast_changes(); }
    void removed(MenuPlayer& player) override;
    bool set_item_name(const std::string& name);   // the rename box
    void create_result();
    SimpleContainer input{2}, result{1};
    std::function<void()> on_result_taken;         // the anvil may wear out
    int repair_item_count_cost = 0;
    std::string item_name;
    MenuPlayer* owner;
};

// slot 0 the item, 1 the lapis, then the player's 27 + 9; ten data slots (costs 0-2, seed, enchantment clues 4-6, level clues 7-9)
class EnchantmentMenu : public Menu {
public:
    EnchantmentMenu(int menu_type_id, int container_id, PlayerInventory* inv, MenuPlayer* owner, std::function<int()> bookshelves);
    ItemStack quick_move_stack(MenuPlayer& player, int slot) override;
    void slots_changed(Container*) override { broadcast_changes(); }
    bool click_menu_button(MenuPlayer& player, int id) override;
    void removed(MenuPlayer& player) override;
    void update_offers();
    SimpleContainer table{2};
    MenuPlayer* owner;
    std::function<int()> bookshelves;
};

}  // namespace pn::sim
