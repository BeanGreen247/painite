// Item stacks in player files: {Slot, id, count} like Mojang's ItemStackWithSlot. Component patches are kept in Painite's own `painite_patch`
// byte array (the network encoding), until the persistent component codecs are ported.
#pragma once
#include "../nbt.hpp"
#include "item.hpp"

namespace pn::sim {

Nbt item_to_nbt(const ItemStack& s, int slot = -1);
bool item_from_nbt(const Nbt& n, ItemStack& out, int* slot = nullptr);

}  // namespace pn::sim
