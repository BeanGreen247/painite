// Items and item stacks, following Mojang's net.minecraft.world.item.{Item,ItemStack}. Item facts come from the data generator's item component
// reports (data/game/<version>/reports/minecraft/components/item/*.json); a stack keeps its component patch as the network encodings of the changed
// components, so any stack a client or the world file holds can be handed back byte for byte.
#pragma once
#include "../vg/json.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn::sim {

using vg::Json;

struct ToolRule {
    std::string blocks;          // "#minecraft:mineable/pickaxe" or a block id
    bool has_speed = false;
    float speed = 1.0f;
    bool has_correct = false;
    bool correct_for_drops = false;
};

struct ItemType {
    int index = 0;               // position in the registry of the data version
    std::string name;            // "minecraft:diamond_pickaxe"
    Json components;             // the prototype components, as the data generator writes them
    int max_stack_size = 64;
    int max_damage = 0;          // 0: not damageable
    float default_mining_speed = 1.0f;
    int tool_damage_per_block = 1;
    std::vector<ToolRule> tool_rules;
    bool has_tool = false;
    bool has_food = false;
    int food_nutrition = 0;
    float food_saturation = 0;
    bool food_can_always_eat = false;
    bool has_consumable = false;
    float consume_seconds = 1.6f;
    std::string consume_animation = "eat";
    bool is_block_item = false;  // an item with the name of a block places it
    std::string equippable_slot; // minecraft:equippable.slot ("head", "offhand", ...); empty when the item cannot be worn
    bool has_component_default(const std::string& key) const { return components.find(key) != nullptr; }
    const Json* component(const std::string& key) const { return components.find(key); }
    bool damageable() const { return max_damage > 0 && max_stack_size == 1; }
};

// Names and protocol ids of the registries gameplay needs, from the data generator's registries.json
class Registries {
public:
    static const Registries& get();
    static bool load(const std::string& game_dir, std::string& err);
    int id(const std::string& registry, const std::string& name) const;   // -1 when unknown
    const std::string& name(const std::string& registry, int id) const;
    int size(const std::string& registry) const;
private:
    std::unordered_map<std::string, std::unordered_map<std::string, int>> ids_;
    std::unordered_map<std::string, std::vector<std::string>> names_;
    friend struct RegistriesLoader;
};

// All items of one data version in registry order. Loaded once; the pointers stay valid for the life of the process.
class Items {
public:
    static const Items& get();
    static bool load(const std::string& game_dir, std::string& err);   // data/game/<version>
    const ItemType* by_name(const std::string& name) const;
    const ItemType* by_index(int i) const { return i >= 0 && size_t(i) < list_.size() ? &list_[size_t(i)] : nullptr; }
    size_t size() const { return list_.size(); }
    const ItemType* air() const { return &list_[0]; }
private:
    std::vector<ItemType> list_;
    std::unordered_map<std::string, int> by_name_;
    friend struct ItemsLoader;
};

// DataComponentPatch: components added or changed (network encoding of each value) and prototype components removed
struct ComponentPatch {
    std::map<int, std::vector<uint8_t>> added;
    std::set<int> removed;
    bool empty() const { return added.empty() && removed.empty(); }
    bool operator==(const ComponentPatch& o) const { return added == o.added && removed == o.removed; }
};

class ItemStack {
public:
    ItemStack() = default;
    ItemStack(const ItemType* t, int n = 1) : item_(t), count_(n) {}
    static const ItemStack& empty_stack() { static ItemStack e; return e; }

    bool is_empty() const { return !item_ || item_->name == "minecraft:air" || count_ <= 0; }
    const ItemType* item() const { return item_ ? item_ : Items::get().air(); }
    int count() const { return count_; }
    int get_count() const { return count_; }
    void set_count(int n) { count_ = n; }
    void grow(int n) { count_ += n; }
    void shrink(int n) { count_ -= n; }
    bool is(const std::string& name) const { return !is_empty() && item_->name == name; }
    bool is(const ItemType* t) const { return !is_empty() && item_ == t; }
    // has a component, from the patch or the prototype
    bool has_component(const std::string& name) const;
    bool is_enchanted() const;
    bool is_stackable() const { return max_stack_size() > 1 && (!damageable() || !is_damaged()); }
    int max_stack_size() const;
    bool damageable() const;
    bool is_damaged() const { return damageable() && damage_value() > 0; }
    int damage_value() const;
    void set_damage_value(int v);
    int max_damage() const;
    ItemStack split(int amount) {
        int n = std::min(amount, count_);
        ItemStack out = copy_with_count(n);
        shrink(n);
        return out;
    }
    ItemStack copy_and_clear() {
        if (is_empty()) return ItemStack();
        ItemStack c = *this;
        *this = ItemStack();
        return c;
    }
    ItemStack copy() const { return *this; }
    ItemStack copy_with_count(int n) const { ItemStack c = *this; c.count_ = n; return c; }
    void limit_size(int max) { if (!is_empty() && count_ > max) count_ = max; }

    // ItemStack.isSameItem / isSameItemSameComponents / matches
    static bool is_same_item(const ItemStack& a, const ItemStack& b) { return a.item() == b.item(); }
    static bool is_same_item_same_components(const ItemStack& a, const ItemStack& b) {
        if (a.is_empty()) return b.is_empty();
        return a.item() == b.item() && !b.is_empty() && a.patch_ == b.patch_;
    }
    static bool matches(const ItemStack& a, const ItemStack& b) {
        if (a.is_empty() && b.is_empty()) return true;
        if (a.is_empty() != b.is_empty()) return false;
        return a.count_ == b.count_ && is_same_item_same_components(a, b);
    }

    ComponentPatch& patch() { return patch_; }
    const ComponentPatch& patch() const { return patch_; }
    // the component's network bytes: the patch value, or empty when the prototype's value applies
    const std::vector<uint8_t>* patch_value(int component_type) const { auto it = patch_.added.find(component_type); return it == patch_.added.end() ? nullptr : &it->second; }

    // item stack wire format (ItemStack.OPTIONAL_STREAM_CODEC): count, item id, component patch
    void write(std::vector<uint8_t>& out) const;
    // reads a stack the client sent (OPTIONAL_UNTRUSTED_STREAM_CODEC: every component value is length prefixed); false when the data is bad
    static bool read_untrusted(const uint8_t*& p, const uint8_t* end, ItemStack& out);

private:
    const ItemType* item_ = nullptr;
    int count_ = 0;
    ComponentPatch patch_;
};

}  // namespace pn::sim
