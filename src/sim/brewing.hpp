// Potion brewing (net.minecraft.world.item.alchemy.PotionBrewing): which ingredient turns which potion or bottle into what.
#pragma once
#include "item.hpp"
#include <string>

namespace pn::sim {

std::string potion_of(const ItemStack& s);                    // "minecraft:awkward", "" when the stack holds no potion
bool is_brewing_ingredient(const ItemStack& s);               // PotionBrewing.isIngredient: some recipe uses it
bool is_brewing_container(const ItemStack& s);                // potion, splash potion, lingering potion or glass bottle (what goes in the bottle slots)
bool is_brewing_potion(const ItemStack& s);                   // the same without the empty bottle
ItemStack brew_mix(const ItemStack& ingredient, const ItemStack& input);   // PotionBrewing.mix: the result, or the input itself when nothing happens
bool brew_changes(const ItemStack& ingredient, const ItemStack& input);

}  // namespace pn::sim
