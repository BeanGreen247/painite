// Typed access to the item components Painite's rules read or write. Values are kept as network encodings in the stack's patch (see item.hpp), so each
// accessor here knows the wire format of its component.
#pragma once
#include "item.hpp"
#include <map>
#include <string>
#include <vector>

namespace pn::sim {

// enchantments and stored_enchantments: map of enchantment id ("minecraft:efficiency") to level
std::map<std::string, int> get_enchantments(const ItemStack& s, const char* component = "minecraft:enchantments");
void set_enchantments(ItemStack& s, const std::map<std::string, int>& e, const char* component = "minecraft:enchantments");
int enchantment_level(const ItemStack& s, const std::string& enchantment);
// potion_contents with just a potion
void set_potion(ItemStack& s, const std::string& potion);
// a text component given as JSON (as data packs write it) in its network form (NBT)
std::vector<uint8_t> text_component_nbt(const vg::Json& j);
void set_text_component(ItemStack& s, const char* component, const vg::Json& j);
void set_int_component(ItemStack& s, const char* component, int value);
void set_varint_component(ItemStack& s, const char* component, int value);

void put_varint_bytes(std::vector<uint8_t>& o, int32_t v);
bool get_varint_bytes(const uint8_t*& p, const uint8_t* end, int32_t& v);

}  // namespace pn::sim
