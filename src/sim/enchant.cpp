#include "enchant.hpp"
#include "components.hpp"
#include "data.hpp"
#include <algorithm>
#include <cmath>
#include <dirent.h>

namespace pn::sim {

Enchants& Enchants::get() { static Enchants e; return e; }

static std::string spec_of(const vg::Json* j) {
    if (!j) return "";
    if (j->is_str()) return j->str;
    std::string out;
    if (j->is_arr()) for (auto& e : j->arr) { if (!out.empty()) out += ","; out += e.str; }
    return out;
}

bool Enchants::load(const std::string& game_dir) {
    dir_ = game_dir;
    defs_.clear();
    std::string path = game_dir + "/data/minecraft/enchantment";
    DIR* dp = opendir(path.c_str());
    if (!dp) return false;
    while (dirent* de = readdir(dp)) {
        std::string f = de->d_name;
        if (f.size() < 6 || f.substr(f.size() - 5) != ".json") continue;
        vg::Json j;
        try { j = vg::Json::parse_file(path + "/" + f); } catch (const std::exception&) { continue; }
        EnchantDef d;
        d.name = "minecraft:" + f.substr(0, f.size() - 5);
        d.max_level = int(j.get_int("max_level", 1));
        d.weight = int(j.get_int("weight", 1));
        d.anvil_cost = int(j.get_int("anvil_cost", 1));
        if (const vg::Json* c = j.find("min_cost")) { d.min_base = int(c->get_int("base", 1)); d.min_per = int(c->get_int("per_level_above_first", 0)); }
        if (const vg::Json* c = j.find("max_cost")) { d.max_base = int(c->get_int("base", 1)); d.max_per = int(c->get_int("per_level_above_first", 0)); }
        d.supported = spec_of(j.find("supported_items"));
        d.primary = spec_of(j.find("primary_items"));
        d.exclusive = spec_of(j.find("exclusive_set"));
        defs_[d.name] = d;
    }
    closedir(dp);
    return !defs_.empty();
}

const EnchantDef* Enchants::find(const std::string& name) const {
    auto it = defs_.find(name);
    return it == defs_.end() ? nullptr : &it->second;
}

bool Enchants::can_enchant(const EnchantDef& d, const ItemStack& s) const {
    return !s.is_empty() && Tags::get().matches("item", d.supported, s.item()->name);
}

bool Enchants::is_primary(const EnchantDef& d, const ItemStack& s) const {
    if (s.is_empty()) return false;
    return Tags::get().matches("item", d.primary.empty() ? d.supported : d.primary, s.item()->name);
}

bool Enchants::compatible(const EnchantDef& a, const EnchantDef& b) const {
    if (&a == &b || a.name == b.name) return false;
    if (!a.exclusive.empty() && Tags::get().matches("enchantment", a.exclusive, b.name)) return false;
    if (!b.exclusive.empty() && Tags::get().matches("enchantment", b.exclusive, a.name)) return false;
    return true;
}

int Enchants::enchantability(const ItemStack& s) {
    if (s.is_empty()) return 0;
    const vg::Json* e = s.item()->component("minecraft:enchantable");
    return e ? int(e->get_int("value", 0)) : 0;
}

int Enchants::cost(vg::Rng& random, int slot, int power, const ItemStack& item) {
    if (enchantability(item) <= 0) return 0;
    if (power > 15) power = 15;
    int selected = random.next_int(8) + 1 + (power >> 1) + random.next_int(power + 1);
    if (slot == 0) return std::max(selected / 3, 1);
    return slot == 1 ? selected * 2 / 3 + 1 : std::max(selected, power * 2);
}

static const EnchantInstance* pick_weighted(vg::Rng& random, const std::vector<EnchantInstance>& list) {
    int total = 0;
    for (auto& e : list) total += e.def->weight;
    if (total <= 0) return nullptr;
    int r = random.next_int(total);
    for (auto& e : list) {
        r -= e.def->weight;
        if (r < 0) return &e;
    }
    return nullptr;
}

std::vector<EnchantInstance> Enchants::select(vg::Rng& random, const ItemStack& item, int level) const {
    std::vector<EnchantInstance> results;
    int enchantable = enchantability(item);
    if (enchantable <= 0) return results;
    level += 1 + random.next_int(enchantable / 4 + 1) + random.next_int(enchantable / 4 + 1);
    float bonus = (random.next_float() + random.next_float() - 1.0f) * 0.15f;
    level = std::max(1, int(std::lround(float(level) + float(level) * bonus)));   // Mth.clamp(Math.round(..), 1, MAX)
    bool book = item.is("minecraft:book");
    std::vector<EnchantInstance> available;
    for (const std::string& n : Tags::get().ordered("enchantment", "minecraft:in_enchanting_table")) {
        const EnchantDef* d = find(n);
        if (!d || (!is_primary(*d, item) && !book)) continue;
        for (int l = d->max_level; l >= 1; l--)
            if (level >= d->min_cost(l) && level <= d->max_cost(l)) { available.push_back({d, l}); break; }
    }
    if (available.empty()) return results;
    if (const EnchantInstance* p = pick_weighted(random, available)) results.push_back(*p);
    while (random.next_int(50) <= level) {
        if (!results.empty()) {
            const EnchantDef* last = results.back().def;
            available.erase(std::remove_if(available.begin(), available.end(), [&](const EnchantInstance& e) { return !compatible(*e.def, *last); }), available.end());
        }
        if (available.empty()) break;
        if (const EnchantInstance* p = pick_weighted(random, available)) results.push_back(*p);
        level /= 2;
    }
    return results;
}

// ---- components

static int cid(const char* name) { return Registries::get().id("minecraft:data_component_type", name); }

int get_repair_cost(const ItemStack& s) {
    if (s.is_empty()) return 0;
    if (const auto* v = s.patch_value(cid("minecraft:repair_cost"))) {
        const uint8_t* p = v->data();
        int32_t n = 0;
        return get_varint_bytes(p, p + v->size(), n) ? n : 0;
    }
    return 0;
}
void set_repair_cost(ItemStack& s, int v) { set_varint_component(s, "minecraft:repair_cost", v); }

std::string get_custom_name(const ItemStack& s) {
    if (s.is_empty()) return "";
    const auto* v = s.patch_value(cid("minecraft:custom_name"));
    if (!v || v->size() < 3 || (*v)[0] != 8) return "";
    size_t n = size_t((*v)[1] << 8 | (*v)[2]);
    if (v->size() < 3 + n) return "";
    return std::string(v->begin() + 3, v->begin() + 3 + long(n));
}
void set_custom_name(ItemStack& s, const std::string& name) {
    vg::Json j;
    j.type = vg::Json::String;
    j.str = name;
    set_text_component(s, "minecraft:custom_name", j);
}
void remove_component(ItemStack& s, const char* component) {
    int id = cid(component);
    s.patch().added.erase(id);
    if (s.item()->components.find(component)) s.patch().removed.insert(id);
}

bool is_valid_repair_item(const ItemStack& item, const ItemStack& material) {
    if (item.is_empty() || material.is_empty()) return false;
    const vg::Json* r = item.item()->component("minecraft:repairable");
    if (!r) return false;
    return Tags::get().matches("item", spec_of(r->find("items")), material.item()->name);
}

}  // namespace pn::sim
