#include "item_nbt.hpp"

namespace pn::sim {

Nbt item_to_nbt(const ItemStack& s, int slot) {
    Nbt n = Nbt::compound();
    if (slot >= 0) n.put("Slot", Nbt::make(Nbt::Byte, slot));
    n.put("id", Nbt::str(s.item()->name));
    n.put("count", Nbt::make(Nbt::Int, s.count()));
    if (!s.patch().empty()) {
        std::vector<uint8_t> raw;
        s.write_delimited(raw);
        Nbt b;
        b.type = Nbt::ByteArray;
        b.ba = std::move(raw);
        n.put("painite_patch", std::move(b));
    }
    return n;
}

bool item_from_nbt(const Nbt& n, ItemStack& out, int* slot) {
    if (n.type != Nbt::Compound) return false;
    if (slot) *slot = int(n.geti("Slot", -1));
    const ItemType* t = Items::get().by_name(n.gets("id"));
    int count = int(n.geti("count", n.geti("Count", 1)));
    if (!t || count <= 0 || t->name == "minecraft:air") { out = ItemStack(); return true; }
    if (const Nbt* p = n.get("painite_patch")) {
        if (p->type == Nbt::ByteArray) {
            const uint8_t* b = p->ba.data();
            ItemStack s;
            if (ItemStack::read_untrusted(b, b + p->ba.size(), s) && !s.is_empty()) { out = s; out.set_count(count); return true; }
        }
    }
    out = ItemStack(t, count);
    return true;
}

}  // namespace pn::sim
