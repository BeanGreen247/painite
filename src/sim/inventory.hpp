// Containers, slots and container menus, ported from Mojang's net.minecraft.world.{Container, SimpleContainer, inventory.*, entity.player.Inventory}.
// Stacks are values here: where Mojang's code hands a stack around by reference and then mutates it, the port takes a reference into the container.
#pragma once
#include "item.hpp"
#include <array>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pn::sim {

enum class EquipmentSlot { MAINHAND, OFFHAND, FEET, LEGS, CHEST, HEAD, BODY, SADDLE };
enum class EquipmentType { HAND, HUMANOID_ARMOR, ANIMAL_ARMOR, SADDLE };
EquipmentType equipment_type(EquipmentSlot s);
int equipment_index(EquipmentSlot s);                 // EquipmentSlot.getIndex(): FEET 0, LEGS 1, CHEST 2, HEAD 3
bool equipment_from_name(const std::string& name, EquipmentSlot& out);
const char* equipment_name(EquipmentSlot s);

// ContainerInput (the mode of the container click packet)
enum class ContainerInput { PICKUP = 0, QUICK_MOVE = 1, SWAP = 2, CLONE = 3, THROW = 4, QUICK_CRAFT = 5, PICKUP_ALL = 6 };
enum class ClickAction { PRIMARY, SECONDARY };

class Container {
public:
    virtual ~Container() = default;
    virtual int size() const = 0;
    virtual ItemStack& item(int slot) = 0;
    virtual void set_item(int slot, const ItemStack& stack) = 0;
    virtual void set_changed() {}
    virtual int max_stack_size() const { return 99; }
    int max_stack_size(const ItemStack& s) const { return std::min(max_stack_size(), s.max_stack_size()); }
    virtual ItemStack remove_item(int slot, int count);          // Container.removeItem
    virtual ItemStack remove_item_no_update(int slot);
    bool is_empty() { for (int i = 0; i < size(); i++) if (!item(i).is_empty()) return false; return true; }
    void clear_content() { for (int i = 0; i < size(); i++) set_item(i, ItemStack()); }
};

class SimpleContainer : public Container {
public:
    explicit SimpleContainer(int n) : items_(size_t(n)) {}
    int size() const override { return int(items_.size()); }
    ItemStack& item(int slot) override { static ItemStack none; if (slot < 0 || size_t(slot) >= items_.size()) { none = ItemStack(); return none; } return items_[size_t(slot)]; }
    void set_item(int slot, const ItemStack& s) override { if (slot >= 0 && size_t(slot) < items_.size()) { items_[size_t(slot)] = s; set_changed(); } }
    ItemStack remove_item(int slot, int count) override;
    ItemStack remove_item_no_update(int slot) override;
    std::vector<ItemStack>& items() { return items_; }
    std::function<void()> on_changed;
    void set_changed() override { if (on_changed) on_changed(); }
private:
    std::vector<ItemStack> items_;
};

// What a menu needs from the player that clicks in it
class MenuPlayer {
public:
    virtual ~MenuPlayer() = default;
    virtual void drop(const ItemStack& stack, bool throw_randomly) = 0;   // Player.drop: an item entity in front of the player
    virtual bool has_infinite_materials() const = 0;                       // creative mode
    virtual bool is_creative() const { return has_infinite_materials(); }
    virtual bool can_drop_items() const { return true; }
    virtual class PlayerInventory& inventory() = 0;
    virtual void on_equip_item(EquipmentSlot slot, const ItemStack& previous, const ItemStack& now) {}
};

// The player's own container: 36 slots, then feet 36, legs 37, chest 38, head 39, offhand 40, body 41, saddle 42
class PlayerInventory : public Container {
public:
    static constexpr int SLOT_OFFHAND = 40, SLOT_BODY_ARMOR = 41, SLOT_SADDLE = 42;
    explicit PlayerInventory(MenuPlayer* owner) : player(owner) {}
    MenuPlayer* player;
    int selected = 0;
    std::array<ItemStack, 36> items;
    std::array<ItemStack, 8> equipment;   // by EquipmentSlot
    int size() const override { return 43; }   // Inventory.getContainerSize: 36 items + 7 equipment slots
    ItemStack& item(int slot) override;
    void set_item(int slot, const ItemStack& s) override;
    ItemStack remove_item(int slot, int count) override;
    ItemStack remove_item_no_update(int slot) override;
    ItemStack& selected_item() { return items[size_t(selected)]; }
    ItemStack& equipment_at(EquipmentSlot s) { return equipment[size_t(s)]; }
    static bool is_hotbar_slot(int slot) { return slot >= 0 && slot < 9; }
    int free_slot() const;
    int slot_with_remaining_space(const ItemStack& s);
    bool has_remaining_space_for_item(const ItemStack& in_slot, const ItemStack& s) const;
    int suitable_hotbar_slot() const;
    bool add(ItemStack& stack) { return add(-1, stack); }
    bool add(int slot, ItemStack& stack);
    void place_item_back_in_inventory(ItemStack& stack);
    int find_slot_matching_item(const ItemStack& s) const;
    std::function<void(int)> on_slot_changed;   // the container index of a slot the server changed on its own (pickup, ...)
private:
    int add_resource(ItemStack& stack);
    int add_resource(int slot, ItemStack& stack);
};

// A 2x2 or 3x3 crafting grid
class CraftingContainer : public SimpleContainer {
public:
    CraftingContainer(int w, int h) : SimpleContainer(w * h), width(w), height(h) {}
    int width, height;
};

class Menu;

class Slot {
public:
    Slot(Container* c, int slot) : container(c), slot_(slot) {}
    virtual ~Slot() = default;
    Container* container;
    int index = 0;
    int container_slot() const { return slot_; }
    virtual void on_quick_craft(const ItemStack& picked, const ItemStack& original);
    virtual void on_quick_craft(const ItemStack&, int) {}
    virtual void on_swap_craft(int) {}
    virtual void on_take(MenuPlayer&, const ItemStack&) { set_changed(); }
    virtual bool may_place(const ItemStack&) const { return true; }
    ItemStack& get_item() { return container->item(slot_); }
    bool has_item() { return !get_item().is_empty(); }
    void set_by_player(const ItemStack& s) { set_by_player(s, get_item()); }
    virtual void set_by_player(const ItemStack& s, const ItemStack& previous) { (void)previous; set(s); }
    void set(const ItemStack& s) { container->set_item(slot_, s); set_changed(); }
    virtual void set_changed() { container->set_changed(); }
    virtual int max_stack_size() const { return container->max_stack_size(); }
    int max_stack_size(const ItemStack& s) const { return std::min(max_stack_size(), s.max_stack_size()); }
    virtual ItemStack remove(int amount) { return container->remove_item(slot_, amount); }
    virtual bool may_pickup(MenuPlayer&) { return true; }
    virtual bool is_active() const { return true; }
    bool try_remove(int amount, int max_amount, MenuPlayer& p, ItemStack& out);
    ItemStack safe_take(int amount, int max_amount, MenuPlayer& p);
    virtual ItemStack safe_clone(MenuPlayer&) { ItemStack i = get_item(); return i.copy_with_count(i.max_stack_size()); }
    ItemStack& safe_insert(ItemStack& stack) { return safe_insert(stack, stack.count()); }
    ItemStack& safe_insert(ItemStack& input, int amount);
    bool allow_modification(MenuPlayer& p) { return may_pickup(p) && may_place(get_item()); }
    virtual bool is_fake() const { return false; }
private:
    int slot_;
};

class ArmorSlot : public Slot {
public:
    ArmorSlot(Container* c, int slot, EquipmentSlot eq) : Slot(c, slot), eq_(eq) {}
    void set_by_player(const ItemStack& s, const ItemStack& previous) override;
    int max_stack_size() const override { return 1; }
    bool may_place(const ItemStack& s) const override;
    bool may_pickup(MenuPlayer& p) override;
    MenuPlayer* owner = nullptr;
private:
    EquipmentSlot eq_;
};

// Receives what the menu has to tell the client (ContainerSynchronizer)
class ContainerSynchronizer {
public:
    virtual ~ContainerSynchronizer() = default;
    virtual void send_initial_data(Menu& m, const std::vector<ItemStack>& items, const ItemStack& carried, const std::vector<int>& data) = 0;
    virtual void send_slot_change(Menu& m, int slot, const ItemStack& s) = 0;
    virtual void send_carried_change(Menu& m, const ItemStack& s) = 0;
    virtual void send_data_change(Menu& m, int id, int value) = 0;
};

// AbstractContainerMenu
class Menu {
public:
    Menu(int menu_type_id, int container_id) : type_id(menu_type_id), container_id(container_id) {}
    virtual ~Menu() = default;
    int type_id;       // protocol id of the menu type, -1 for the player's own menu
    int container_id;
    std::vector<std::unique_ptr<Slot>> slots;
    ContainerSynchronizer* synchronizer = nullptr;

    Slot* add_slot(std::unique_ptr<Slot> s);
    ItemStack& carried() { return carried_; }
    void set_carried(const ItemStack& s) { carried_ = s; }
    bool is_valid_slot_index(int i) const { return i == -1 || i == -999 || i < int(slots.size()); }
    void clicked(int slot_index, int button, ContainerInput input, MenuPlayer& player);
    virtual ItemStack quick_move_stack(MenuPlayer& player, int slot) = 0;
    virtual bool still_valid(MenuPlayer&) { return true; }
    virtual void removed(MenuPlayer& player);
    virtual void slots_changed(Container*) { broadcast_changes(); }
    virtual bool can_take_item_for_pick_all(const ItemStack&, Slot*) { return true; }
    virtual bool can_drag_to(Slot*) { return true; }
    virtual bool click_menu_button(MenuPlayer&, int) { return false; }
    bool move_item_stack_to(ItemStack& stack, int start, int end, bool backwards);
    void broadcast_changes();
    void broadcast_full_state();
    void send_all_data_to_remote();
    void set_synchronizer(ContainerSynchronizer* s);
    // the client's belief about a slot after its own prediction (we compare stacks without components; see handle_click_result)
    void set_remote_slot(int slot, const ItemStack& s) { if (slot >= 0 && size_t(slot) < remote_.size()) remote_[size_t(slot)] = s; }
    void set_remote_carried(const ItemStack& s) { remote_carried_ = s; }
    int state_id() const { return state_id_; }
    int increment_state_id() { state_id_ = (state_id_ + 1) & 0x7FFF; return state_id_; }
    void set_state_id(int s) { state_id_ = s; }
    static int quickcraft_type(int mask) { return mask >> 2 & 3; }
    static int quickcraft_header(int mask) { return mask & 3; }
    static bool can_item_quick_replace(Slot* slot, const ItemStack& s, bool ignore_size);
    static int quick_craft_place_count(int slots, int type, const ItemStack& s);
    std::vector<int> data_slots;   // values of the data slots (furnace progress, ...), set by subclasses
protected:
    void do_click(int slot_index, int button, ContainerInput input, MenuPlayer& player);
    void reset_quick_craft() { quickcraft_status_ = 0; quickcraft_slots_.clear(); }
    void drop_or_place_in_inventory(MenuPlayer& player, const ItemStack& carried);
    void clear_container(MenuPlayer& player, Container* c);
    void add_standard_inventory_slots(Container* inv, int, int) { add_inventory_extended(inv); add_inventory_hotbar(inv); }
    void add_inventory_extended(Container* inv) { for (int y = 0; y < 3; y++) for (int x = 0; x < 9; x++) add_slot(std::make_unique<Slot>(inv, x + (y + 1) * 9)); }
    void add_inventory_hotbar(Container* inv) { for (int x = 0; x < 9; x++) add_slot(std::make_unique<Slot>(inv, x)); }
    ItemStack carried_;
    std::vector<ItemStack> remote_;
    ItemStack remote_carried_;
    std::vector<int> remote_data_;
    int state_id_ = 0;
    int quickcraft_type_ = -1, quickcraft_status_ = 0;
    std::set<Slot*> quickcraft_slots_;
    bool suppress_remote_updates_ = false;
};

// Finds the recipe result of a crafting grid (ResultSlot and CraftingMenu.slotChangedCraftingGrid); set by the game once recipes are loaded
class CraftingProvider {
public:
    virtual ~CraftingProvider() = default;
    virtual ItemStack result(CraftingContainer& grid) = 0;
    virtual std::vector<ItemStack> remaining(CraftingContainer& grid) = 0;   // by grid slot
};

class ResultSlot : public Slot {
public:
    ResultSlot(MenuPlayer* p, CraftingContainer* craft, Container* result, CraftingProvider** provider) : Slot(result, 0), player(p), craft_(craft), provider_(provider) {}
    bool may_place(const ItemStack&) const override { return false; }
    ItemStack remove(int amount) override;
    void on_quick_craft(const ItemStack& picked, int count) override { remove_count_ += count; (void)picked; }
    void on_swap_craft(int count) override { remove_count_ += count; }
    void on_take(MenuPlayer& p, const ItemStack& carried) override;
    bool is_fake() const override { return true; }
    MenuPlayer* player;
private:
    CraftingContainer* craft_;
    CraftingProvider** provider_;
    int remove_count_ = 0;
};

// InventoryMenu: the player's own window (slot 0 result, 1-4 crafting grid, 5-8 armor, 9-35 inventory, 36-44 hotbar, 45 offhand)
class InventoryMenu : public Menu {
public:
    InventoryMenu(PlayerInventory* inv, MenuPlayer* owner);
    ItemStack quick_move_stack(MenuPlayer& player, int slot) override;
    void slots_changed(Container* c) override;
    void removed(MenuPlayer& player) override;
    bool can_take_item_for_pick_all(const ItemStack& carried, Slot* target) override { return target->container != &result_ && Menu::can_take_item_for_pick_all(carried, target); }
    CraftingContainer craft{2, 2};
    SimpleContainer result_{1};
    CraftingProvider* crafting = nullptr;
    MenuPlayer* owner_;
    EquipmentSlot equipment_slot_for(const ItemStack& s) const;
    void update_crafting_result();
};

// CraftingMenu: the crafting table's window (slot 0 result, 1-9 grid, 10-36 inventory, 37-45 hotbar)
class CraftingMenu : public Menu {
public:
    CraftingMenu(int menu_type_id, int container_id, PlayerInventory* inv, MenuPlayer* owner, CraftingProvider* provider);
    ItemStack quick_move_stack(MenuPlayer& player, int slot) override;
    void slots_changed(Container*) override { broadcast_changes(); }
    void removed(MenuPlayer& player) override;
    bool can_take_item_for_pick_all(const ItemStack& carried, Slot* target) override { return target->container != &result_ && Menu::can_take_item_for_pick_all(carried, target); }
    CraftingContainer craft{3, 3};
    SimpleContainer result_{1};
    CraftingProvider* crafting;
    void update_crafting_result();
};

// ChestMenu, ShulkerBoxMenu, DispenserMenu, HopperMenu: container slots first (one or two containers behind them), then the player's 27 + 9
class ContainerMenu : public Menu {
public:
    ContainerMenu(int menu_type_id, int container_id, PlayerInventory* inv, std::vector<Container*> parts, bool no_shulkers, std::shared_ptr<void> keep_alive);
    ~ContainerMenu() override { if (on_closed) on_closed(); }
    ItemStack quick_move_stack(MenuPlayer& player, int slot) override;
    void removed(MenuPlayer& player) override { Menu::removed(player); if (on_closed) { auto f = std::move(on_closed); on_closed = nullptr; f(); } }
    int container_slots = 0;
    std::function<void()> on_closed;   // the owner of the containers stops telling this menu about changes
    std::shared_ptr<void> keep;
};

// AbstractFurnaceMenu: 0 ingredient, 1 fuel, 2 result, then the player's 27 + 9; four data values (lit time left, lit total, cook progress, cook total)
class FurnaceMenu : public Menu {
public:
    FurnaceMenu(int menu_type_id, int container_id, PlayerInventory* inv, Container* furnace, const std::string& recipe_kind, std::shared_ptr<void> keep_alive);
    ~FurnaceMenu() override { if (on_closed) on_closed(); }
    ItemStack quick_move_stack(MenuPlayer& player, int slot) override;
    void removed(MenuPlayer& player) override { Menu::removed(player); if (on_closed) { auto f = std::move(on_closed); on_closed = nullptr; f(); } }
    std::string kind;
    std::function<void()> on_closed;
    std::function<void()> award;   // the player took from the result slot: hand out the experience the furnace stored
    std::shared_ptr<void> keep;
};

}  // namespace pn::sim
