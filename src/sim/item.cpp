#include "item.hpp"
#include <cstring>
#include <stdexcept>

namespace pn::sim {

namespace {
Items g_items;
Registries g_regs;
bool g_loaded = false;

void put_varint(std::vector<uint8_t>& o, int32_t v) {
    uint32_t u = uint32_t(v);
    while (u & ~0x7Fu) { o.push_back(uint8_t((u & 0x7F) | 0x80)); u >>= 7; }
    o.push_back(uint8_t(u));
}
bool get_varint(const uint8_t*& p, const uint8_t* end, int32_t& v) {
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
}  // namespace

struct RegistriesLoader {
    static bool run(const std::string& dir, std::string& err) {
        try {
            Json j = Json::parse_file(dir + "/reports/registries.json");
            for (auto& reg : j.obj) {
                auto& ids = g_regs.ids_[reg.first];
                auto& names = g_regs.names_[reg.first];
                const Json* entries = reg.second.find("entries");
                if (!entries) continue;
                for (auto& e : entries->obj) {
                    int id = int(e.second.get_int("protocol_id", -1));
                    if (id < 0) continue;
                    ids[e.first] = id;
                    if (size_t(id) >= names.size()) names.resize(size_t(id) + 1);
                    names[size_t(id)] = e.first;
                }
            }
            return true;
        } catch (const std::exception& e) { err = e.what(); return false; }
    }
};

const Registries& Registries::get() { return g_regs; }
void Registries::set_dynamic(const std::map<std::string, std::vector<std::string>>& regs) {
    for (auto& kv : regs) {
        auto& ids = g_regs.ids_[kv.first];
        auto& names = g_regs.names_[kv.first];
        ids.clear();
        names = kv.second;
        for (size_t i = 0; i < names.size(); i++) ids[names[i]] = int(i);
    }
}
bool Registries::load(const std::string& dir, std::string& err) { return RegistriesLoader::run(dir, err); }
int Registries::id(const std::string& registry, const std::string& name) const {
    auto r = ids_.find(registry);
    if (r == ids_.end()) return -1;
    auto it = r->second.find(name);
    return it == r->second.end() ? -1 : it->second;
}
const std::string& Registries::name(const std::string& registry, int id) const {
    static const std::string none;
    auto r = names_.find(registry);
    if (r == names_.end() || id < 0 || size_t(id) >= r->second.size()) return none;
    return r->second[size_t(id)];
}
int Registries::size(const std::string& registry) const { auto r = names_.find(registry); return r == names_.end() ? 0 : int(r->second.size()); }

struct ItemsLoader {
    static bool run(const std::string& dir, std::string& err) {
        try {
            if (!g_loaded) {
                if (!Registries::load(dir, err)) return false;
            }
            const Registries& R = Registries::get();
            int n = R.size("minecraft:item");
            g_items.list_.assign(size_t(n), ItemType());
            g_items.by_name_.clear();
            for (int i = 0; i < n; i++) {
                ItemType& t = g_items.list_[size_t(i)];
                t.index = i;
                t.name = R.name("minecraft:item", i);
                g_items.by_name_[t.name] = i;
                std::string key = t.name.substr(t.name.find(':') + 1);
                std::string path = dir + "/reports/minecraft/components/item/" + key + ".json";
                try {
                    Json j = Json::parse_file(path);
                    if (const Json* c = j.find("components")) t.components = *c;
                } catch (const std::exception&) { continue; }
                const Json& c = t.components;
                if (const Json* eq = c.find("minecraft:equippable")) t.equippable_slot = eq->get_string("slot", "");
                t.max_stack_size = int(c.get_int("minecraft:max_stack_size", 64));
                t.max_damage = int(c.get_int("minecraft:max_damage", 0));
                if (const Json* tool = c.find("minecraft:tool")) {
                    t.has_tool = true;
                    t.default_mining_speed = tool->find("default_mining_speed") ? tool->at("default_mining_speed").float_value() : 1.0f;
                    t.tool_damage_per_block = int(tool->get_int("damage_per_block", 1));
                    if (const Json* rules = tool->find("rules"))
                        for (auto& r : rules->arr) {
                            ToolRule tr;
                            const Json* b = r.find("blocks");
                            if (b) {
                                if (b->is_str()) tr.blocks = b->str;
                                else if (b->is_arr() && !b->arr.empty()) {   // a list of block ids: stored joined by commas
                                    for (auto& e : b->arr) tr.blocks += (tr.blocks.empty() ? "" : ",") + e.str;
                                }
                            }
                            if (const Json* sp = r.find("speed")) { tr.has_speed = true; tr.speed = sp->float_value(); }
                            if (const Json* cd = r.find("correct_for_drops")) { tr.has_correct = true; tr.correct_for_drops = cd->boolean(); }
                            t.tool_rules.push_back(tr);
                        }
                }
                if (const Json* f = c.find("minecraft:food")) {
                    t.has_food = true;
                    t.food_nutrition = int(f->get_int("nutrition", 0));
                    t.food_saturation = f->find("saturation") ? f->at("saturation").float_value() : 0.0f;
                    t.food_can_always_eat = f->get_bool("can_always_eat", false);
                }
                if (const Json* co = c.find("minecraft:consumable")) {
                    t.has_consumable = true;
                    t.consume_seconds = co->find("consume_seconds") ? co->at("consume_seconds").float_value() : 1.6f;
                    t.consume_animation = co->get_string("animation", "eat");
                }
            }
            g_loaded = true;
            return true;
        } catch (const std::exception& e) { err = e.what(); return false; }
    }
};

const Items& Items::get() { return g_items; }
bool Items::load(const std::string& dir, std::string& err) { return ItemsLoader::run(dir, err); }
const ItemType* Items::by_name(const std::string& name) const {
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &list_[size_t(it->second)];
}

bool ItemStack::has_component(const std::string& name) const {
    if (is_empty()) return false;
    int id = Registries::get().id("minecraft:data_component_type", name);
    if (id >= 0) {
        if (patch_.added.count(id)) return true;
        if (patch_.removed.count(id)) return false;
    }
    return item_->components.find(name) != nullptr;
}
bool ItemStack::is_enchanted() const {
    static int id = Registries::get().id("minecraft:data_component_type", "minecraft:enchantments");
    const auto* v = patch_value(id);
    if (v) { const uint8_t* p = v->data(); int32_t n = 0; return get_varint(p, p + v->size(), n) && n > 0; }
    const Json* e = is_empty() ? nullptr : item_->components.find("minecraft:enchantments");
    return e && e->is_obj() && !e->obj.empty() && !patch_.removed.count(id);
}

int ItemStack::max_stack_size() const {
    if (is_empty()) return 64;
    return item_->max_stack_size;   // a changed max_stack_size component is not applied yet
}
bool ItemStack::damageable() const { return !is_empty() && item_->damageable(); }
int ItemStack::max_damage() const { return is_empty() ? 0 : item_->max_damage; }
int ItemStack::damage_value() const {
    static int damage_id = Registries::get().id("minecraft:data_component_type", "minecraft:damage");
    const auto* v = patch_value(damage_id);
    if (!v) return 0;
    const uint8_t* p = v->data();
    int32_t d = 0;
    if (!get_varint(p, p + v->size(), d)) return 0;
    return std::max(0, std::min(d, max_damage()));
}
void ItemStack::set_damage_value(int d) {
    static int damage_id = Registries::get().id("minecraft:data_component_type", "minecraft:damage");
    d = std::max(0, std::min(d, max_damage()));
    if (d == 0) { patch_.added.erase(damage_id); return; }
    std::vector<uint8_t> b;
    put_varint(b, d);
    patch_.added[damage_id] = std::move(b);
    patch_.removed.erase(damage_id);
}

void ItemStack::write(std::vector<uint8_t>& out) const {
    if (is_empty()) { put_varint(out, 0); return; }
    put_varint(out, count_);
    put_varint(out, item_->index);
    put_varint(out, int(patch_.added.size()));
    put_varint(out, int(patch_.removed.size()));
    for (auto& kv : patch_.added) { put_varint(out, kv.first); out.insert(out.end(), kv.second.begin(), kv.second.end()); }
    for (int r : patch_.removed) put_varint(out, r);
}

void ItemStack::write_delimited(std::vector<uint8_t>& out) const {
    if (is_empty()) { put_varint(out, 0); return; }
    put_varint(out, count_);
    put_varint(out, item_->index);
    put_varint(out, int(patch_.added.size()));
    put_varint(out, int(patch_.removed.size()));
    for (auto& kv : patch_.added) { put_varint(out, kv.first); put_varint(out, int(kv.second.size())); out.insert(out.end(), kv.second.begin(), kv.second.end()); }
    for (int r : patch_.removed) put_varint(out, r);
}

bool ItemStack::read_untrusted(const uint8_t*& p, const uint8_t* end, ItemStack& out) {
    out = ItemStack();
    int32_t count;
    if (!get_varint(p, end, count)) return false;
    if (count <= 0) return true;
    int32_t item;
    if (!get_varint(p, end, item)) return false;
    const ItemType* t = Items::get().by_index(item);
    if (!t) return false;
    int32_t pos, neg;
    if (!get_varint(p, end, pos) || !get_varint(p, end, neg) || pos < 0 || neg < 0 || pos > 4096 || neg > 4096) return false;
    ItemStack s(t, count);
    for (int i = 0; i < pos; i++) {
        int32_t type, len;
        if (!get_varint(p, end, type) || !get_varint(p, end, len) || len < 0 || p + len > end) return false;
        s.patch_.added[type] = std::vector<uint8_t>(p, p + len);
        p += len;
    }
    for (int i = 0; i < neg; i++) {
        int32_t type;
        if (!get_varint(p, end, type)) return false;
        s.patch_.removed.insert(type);
    }
    out = std::move(s);
    return true;
}

}  // namespace pn::sim
