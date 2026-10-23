// Recipes from data/game/<version>/data/minecraft/recipe: shaped and shapeless crafting, smelting, blasting, smoking and campfire cooking.
// The crafting grid is matched the way ShapedRecipe and ShapelessRecipe do it (shapes may be mirrored, anything outside the shape must be empty).
#pragma once
#include "inventory.hpp"
#include <set>
#include <string>
#include <vector>

namespace pn::sim {

struct Ingredient {
    std::set<int> items;   // item indices that fit
    bool empty = true;     // a space in a shaped pattern
    bool matches(const ItemStack& s) const { return !s.is_empty() && items.count(s.item()->index) != 0; }
};

struct CraftingRecipe {
    bool shaped = false;
    int width = 0, height = 0;           // shaped: size of the pattern
    std::vector<Ingredient> ingredients; // shaped: width * height cells; shapeless: the list
    ItemStack result;
    std::string name;
    bool matches(CraftingContainer& grid) const;
};

struct CookingRecipe {
    std::string kind;                    // smelting, blasting, smoking, campfire_cooking
    Ingredient ingredient;
    ItemStack result;
    float experience = 0;
    int time = 200;
    std::string name;
};

class Recipes {
public:
    static Recipes& get();
    bool load(const std::string& game_dir, std::string& err);   // after Items and Tags are set up
    size_t crafting_count() const { return crafting_.size(); }
    const CraftingRecipe* find_crafting(CraftingContainer& grid) const;
    const CookingRecipe* find_cooking(const std::string& kind, const ItemStack& input) const;
    bool is_fuel(const ItemStack& s) const { return fuel_ticks(s) > 0; }
    int fuel_ticks(const ItemStack& s) const;   // burn time of an item as furnace fuel (FuelValues), 0 when it is not fuel
private:
    std::vector<CraftingRecipe> crafting_;
    std::vector<CookingRecipe> cooking_;
};

// Hands the recipes to menus: ResultSlot and the crafting menus ask it what a grid makes and what stays behind
class RecipeCrafting : public CraftingProvider {
public:
    ItemStack result(CraftingContainer& grid) override;
    std::vector<ItemStack> remaining(CraftingContainer& grid) override;
    static RecipeCrafting& get();
};

}  // namespace pn::sim
