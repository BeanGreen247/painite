#include "components.hpp"
#include <cstring>

namespace pn::sim {

void put_varint_bytes(std::vector<uint8_t>& o, int32_t v) {
    uint32_t u = uint32_t(v);
    while (u & ~0x7Fu) { o.push_back(uint8_t((u & 0x7F) | 0x80)); u >>= 7; }
    o.push_back(uint8_t(u));
}
bool get_varint_bytes(const uint8_t*& p, const uint8_t* end, int32_t& v) {
    uint32_t r = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        r |= uint32_t(b & 0x7F) << shift;
        if (!(b & 0x80)) { v = int32_t(r); return true; }
        shift += 7;
        if (shift > 35) return false;
    }
    return false;
}

static int component_id(const char* name) { return Registries::get().id("minecraft:data_component_type", name); }

std::map<std::string, int> get_enchantments(const ItemStack& s, const char* component) {
    std::map<std::string, int> out;
    if (s.is_empty()) return out;
    int cid = component_id(component);
    const auto* v = s.patch_value(cid);
    if (v) {
        const uint8_t* p = v->data();
        const uint8_t* end = p + v->size();
        int32_t n = 0;
        if (!get_varint_bytes(p, end, n)) return out;
        for (int i = 0; i < n; i++) {
            int32_t id, level;
            if (!get_varint_bytes(p, end, id) || !get_varint_bytes(p, end, level)) break;
            const std::string& name = Registries::get().name("minecraft:enchantment", id);
            if (!name.empty()) out[name] = level;
        }
        return out;
    }
    if (s.patch().removed.count(cid)) return out;
    if (const vg::Json* e = s.item()->components.find(component))
        if (e->is_obj()) {
            const vg::Json* lv = e->find("levels") ? e->find("levels") : e;
            for (auto& kv : lv->obj) out[kv.first.find(':') == std::string::npos ? "minecraft:" + kv.first : kv.first] = int(kv.second.int_value());
        }
    return out;
}

void set_enchantments(ItemStack& s, const std::map<std::string, int>& e, const char* component) {
    int cid = component_id(component);
    std::vector<uint8_t> b;
    std::vector<std::pair<int, int>> list;
    for (auto& kv : e) {
        int id = Registries::get().id("minecraft:enchantment", kv.first);
        if (id >= 0 && kv.second > 0) list.emplace_back(id, kv.second);
    }
    put_varint_bytes(b, int(list.size()));
    for (auto& pr : list) { put_varint_bytes(b, pr.first); put_varint_bytes(b, pr.second); }
    s.patch().added[cid] = std::move(b);
    s.patch().removed.erase(cid);
}

int enchantment_level(const ItemStack& s, const std::string& enchantment) {
    auto m = get_enchantments(s);
    auto it = m.find(enchantment.find(':') == std::string::npos ? "minecraft:" + enchantment : enchantment);
    return it == m.end() ? 0 : it->second;
}

void set_potion(ItemStack& s, const std::string& potion) {
    int cid = component_id("minecraft:potion_contents");
    int pid = Registries::get().id("minecraft:potion", potion.find(':') == std::string::npos ? "minecraft:" + potion : potion);
    if (pid < 0) return;
    std::vector<uint8_t> b;
    b.push_back(1);                  // optional potion: present
    put_varint_bytes(b, pid);
    b.push_back(0);                  // custom color: none
    put_varint_bytes(b, 0);          // custom effects: none
    b.push_back(0);                  // custom name: none
    s.patch().added[cid] = std::move(b);
    s.patch().removed.erase(cid);
}

static void nbt_put_string(std::vector<uint8_t>& o, const std::string& s) {
    o.push_back(uint8_t(s.size() >> 8));
    o.push_back(uint8_t(s.size()));
    o.insert(o.end(), s.begin(), s.end());
}
static void nbt_payload(std::vector<uint8_t>& o, const vg::Json& j, uint8_t& type);
static void nbt_named(std::vector<uint8_t>& o, const std::string& name, const vg::Json& j) {
    std::vector<uint8_t> payload;
    uint8_t type = 0;
    nbt_payload(payload, j, type);
    o.push_back(type);
    nbt_put_string(o, name);
    o.insert(o.end(), payload.begin(), payload.end());
}
// the JSON of a text component as NBT: strings stay strings, objects become compounds, lists lists; booleans are bytes
static void nbt_payload(std::vector<uint8_t>& o, const vg::Json& j, uint8_t& type) {
    if (j.is_str()) { type = 8; nbt_put_string(o, j.str); return; }
    if (j.is_bool()) { type = 1; o.push_back(j.b ? 1 : 0); return; }
    if (j.is_num()) {
        if (j.is_int) { type = 3; uint32_t v = uint32_t(j.integer); for (int i = 3; i >= 0; i--) o.push_back(uint8_t(v >> (8 * i))); }
        else { type = 6; double d = j.num; uint64_t u; memcpy(&u, &d, 8); for (int i = 7; i >= 0; i--) o.push_back(uint8_t(u >> (8 * i))); }
        return;
    }
    if (j.is_arr()) {
        type = 9;
        uint8_t et = j.arr.empty() ? 0 : 10;   // lists of components are lists of compounds (strings inside get wrapped as {text:...})
        std::vector<uint8_t> items;
        for (auto& e : j.arr) {
            if (e.is_str()) { vg::Json w; w.type = vg::Json::Object; vg::Json t; t.type = vg::Json::String; t.str = e.str; w.obj.emplace_back("text", t); std::vector<uint8_t> tmp; uint8_t tt; nbt_payload(tmp, w, tt); items.insert(items.end(), tmp.begin(), tmp.end()); }
            else { std::vector<uint8_t> tmp; uint8_t tt; nbt_payload(tmp, e, tt); et = tt; items.insert(items.end(), tmp.begin(), tmp.end()); }
        }
        o.push_back(et);
        uint32_t n = uint32_t(j.arr.size());
        for (int i = 3; i >= 0; i--) o.push_back(uint8_t(n >> (8 * i)));
        o.insert(o.end(), items.begin(), items.end());
        return;
    }
    type = 10;
    for (auto& kv : j.obj) nbt_named(o, kv.first, kv.second);
    o.push_back(0);
}
std::vector<uint8_t> text_component_nbt(const vg::Json& j) {
    std::vector<uint8_t> out;
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    nbt_payload(payload, j, type);
    out.push_back(type);   // network NBT: type byte, no name
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
void set_text_component(ItemStack& s, const char* component, const vg::Json& j) {
    int cid = component_id(component);
    s.patch().added[cid] = text_component_nbt(j);
    s.patch().removed.erase(cid);
}
void set_int_component(ItemStack& s, const char* component, int v) {
    int cid = component_id(component);
    std::vector<uint8_t> b;
    for (int i = 3; i >= 0; i--) b.push_back(uint8_t(uint32_t(v) >> (8 * i)));
    s.patch().added[cid] = std::move(b);
}
void set_varint_component(ItemStack& s, const char* component, int v) {
    int cid = component_id(component);
    std::vector<uint8_t> b;
    put_varint_bytes(b, v);
    s.patch().added[cid] = std::move(b);
}

}  // namespace pn::sim
