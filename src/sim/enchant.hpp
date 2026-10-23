// Enchantment rules ported from Mojang's EnchantmentHelper / Enchantment: the definitions come from the data pack
// (data/game/<ver>/data/minecraft/enchantment/*.json); the table's offers and the anvil's costs follow the vanilla formulas.
#pragma once
#include "../vg/random.hpp"
#include "item.hpp"
#include <map>
#include <string>
#include <vector>

namespace pn::sim {

struct EnchantDef {
    std::string name;                 // "minecraft:sharpness"
    int max_level = 1, weight = 1, anvil_cost = 1;
    int min_base = 1, min_per = 0, max_base = 1, max_per = 0;
    std::string supported, primary;   // item specs: "#tag", an id or a comma separated list
    std::string exclusive;            // enchantment spec the same way, empty when none
    int min_cost(int level) const { return min_base + min_per * (level - 1); }
    int max_cost(int level) const { return max_base + max_per * (level - 1); }
};

struct EnchantInstance {
    const EnchantDef* def = nullptr;
    int level = 0;
};

class Enchants {
public:
    static Enchants& get();
    bool load(const std::string& game_dir);
    const EnchantDef* find(const std::string& name) const;
    bool can_enchant(const EnchantDef& d, const ItemStack& s) const;        // Enchantment.canEnchant: the item is a supported item
    bool is_primary(const EnchantDef& d, const ItemStack& s) const;         // Enchantment.isPrimaryItem
    bool compatible(const EnchantDef& a, const EnchantDef& b) const;        // Enchantment.areCompatible
    // EnchantmentHelper.selectEnchantment with the enchantments of #minecraft:in_enchanting_table
    std::vector<EnchantInstance> select(vg::Rng& random, const ItemStack& item, int level) const;
    static int cost(vg::Rng& random, int slot, int power, const ItemStack& item);   // EnchantmentHelper.getEnchantmentCost
    static int enchantability(const ItemStack& s);                          // the enchantable component, 0 when the item cannot be enchanted
private:
    std::map<std::string, EnchantDef> defs_;
    std::string dir_;
};

// item components the anvil reads and writes
int get_repair_cost(const ItemStack& s);
void set_repair_cost(ItemStack& s, int v);
std::string get_custom_name(const ItemStack& s);   // plain string names only, empty otherwise
void set_custom_name(ItemStack& s, const std::string& name);
void remove_component(ItemStack& s, const char* component);
bool is_valid_repair_item(const ItemStack& item, const ItemStack& material);   // the repairable component of the item names the material

}  // namespace pn::sim
