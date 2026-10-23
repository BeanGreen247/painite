#include "brewing.hpp"
#include "components.hpp"
#include <vector>

namespace pn::sim {

namespace {

struct Mix { const char* from; const char* ingredient; const char* to; };

// PotionBrewing.addVanillaMixes
const std::vector<Mix>& mixes() {
    static const std::vector<Mix> m = {
        {"water", "glistering_melon_slice", "mundane"}, {"water", "ghast_tear", "mundane"}, {"water", "rabbit_foot", "mundane"}, {"water", "blaze_powder", "mundane"},
        {"water", "spider_eye", "mundane"}, {"water", "sugar", "mundane"}, {"water", "magma_cream", "mundane"}, {"water", "redstone", "mundane"},
        {"water", "glowstone_dust", "thick"}, {"water", "nether_wart", "awkward"}, {"water", "fermented_spider_eye", "weakness"},
        {"weakness", "redstone", "long_weakness"},
        {"awkward", "golden_carrot", "night_vision"}, {"night_vision", "redstone", "long_night_vision"}, {"night_vision", "fermented_spider_eye", "invisibility"},
        {"long_night_vision", "fermented_spider_eye", "long_invisibility"}, {"invisibility", "redstone", "long_invisibility"},
        {"awkward", "rabbit_foot", "leaping"}, {"leaping", "redstone", "long_leaping"}, {"leaping", "glowstone_dust", "strong_leaping"},
        {"leaping", "fermented_spider_eye", "slowness"}, {"long_leaping", "fermented_spider_eye", "long_slowness"},
        {"slowness", "redstone", "long_slowness"}, {"slowness", "glowstone_dust", "strong_slowness"},
        {"awkward", "magma_cream", "fire_resistance"}, {"fire_resistance", "redstone", "long_fire_resistance"},
        {"fire_resistance", "fermented_spider_eye", "slowness"}, {"long_fire_resistance", "fermented_spider_eye", "long_slowness"},
        {"awkward", "sugar", "swiftness"}, {"swiftness", "redstone", "long_swiftness"}, {"swiftness", "glowstone_dust", "strong_swiftness"},
        {"swiftness", "fermented_spider_eye", "slowness"}, {"long_swiftness", "fermented_spider_eye", "long_slowness"},
        {"awkward", "pufferfish", "water_breathing"}, {"water_breathing", "redstone", "long_water_breathing"},
        {"water_breathing", "fermented_spider_eye", "harming"}, {"long_water_breathing", "fermented_spider_eye", "harming"},
        {"awkward", "glistering_melon_slice", "healing"}, {"healing", "glowstone_dust", "strong_healing"},
        {"healing", "fermented_spider_eye", "harming"}, {"strong_healing", "fermented_spider_eye", "strong_harming"}, {"harming", "glowstone_dust", "strong_harming"},
        {"awkward", "spider_eye", "poison"}, {"poison", "redstone", "long_poison"}, {"poison", "glowstone_dust", "strong_poison"},
        {"poison", "fermented_spider_eye", "harming"}, {"long_poison", "fermented_spider_eye", "harming"}, {"strong_poison", "fermented_spider_eye", "strong_harming"},
        {"awkward", "ghast_tear", "regeneration"}, {"regeneration", "redstone", "long_regeneration"}, {"regeneration", "glowstone_dust", "strong_regeneration"},
        {"awkward", "blaze_powder", "strength"}, {"strength", "redstone", "long_strength"}, {"strength", "glowstone_dust", "strong_strength"},
        {"awkward", "turtle_helmet", "turtle_master"}, {"turtle_master", "redstone", "long_turtle_master"}, {"turtle_master", "glowstone_dust", "strong_turtle_master"},
        {"awkward", "phantom_membrane", "slow_falling"}, {"slow_falling", "redstone", "long_slow_falling"},
        {"awkward", "breeze_rod", "wind_charged"}, {"awkward", "cobweb", "weaving"}, {"awkward", "slime_block", "oozing"}, {"awkward", "stone", "infested"},
    };
    return m;
}

std::string short_name(const ItemStack& s) { std::string n = s.item()->name; return n.rfind("minecraft:", 0) == 0 ? n.substr(10) : n; }

}  // namespace

std::string potion_of(const ItemStack& s) {
    if (s.is_empty()) return "";
    static int cid = Registries::get().id("minecraft:data_component_type", "minecraft:potion_contents");
    const auto* v = s.patch_value(cid);
    if (!v || v->size() < 2 || (*v)[0] != 1) return "";
    const uint8_t* p = v->data() + 1;
    int32_t id = 0;
    if (!get_varint_bytes(p, v->data() + v->size(), id)) return "";
    return Registries::get().name("minecraft:potion", id);
}

bool is_brewing_potion(const ItemStack& s) { return s.is("minecraft:potion") || s.is("minecraft:splash_potion") || s.is("minecraft:lingering_potion"); }
bool is_brewing_container(const ItemStack& s) { return is_brewing_potion(s) || s.is("minecraft:glass_bottle"); }

bool is_brewing_ingredient(const ItemStack& s) {
    if (s.is_empty()) return false;
    if (s.is("minecraft:gunpowder") || s.is("minecraft:dragon_breath")) return true;
    std::string n = short_name(s);
    for (const Mix& m : mixes()) if (n == m.ingredient) return true;
    return false;
}

ItemStack brew_mix(const ItemStack& ingredient, const ItemStack& input) {
    if (ingredient.is_empty() || input.is_empty()) return input;
    // container recipes: gunpowder makes a potion splash, dragon's breath makes a splash potion linger
    const char* to = nullptr;
    if (ingredient.is("minecraft:gunpowder") && input.is("minecraft:potion")) to = "minecraft:splash_potion";
    else if (ingredient.is("minecraft:dragon_breath") && input.is("minecraft:splash_potion")) to = "minecraft:lingering_potion";
    if (to) {
        ItemStack out(Items::get().by_name(to), input.count());
        out.patch() = input.patch();
        return out;
    }
    if (!is_brewing_potion(input)) return input;
    std::string from = potion_of(input), name = short_name(ingredient);
    if (from.rfind("minecraft:", 0) == 0) from = from.substr(10);
    for (const Mix& m : mixes())
        if (from == m.from && name == m.ingredient) {
            ItemStack out = input;
            set_potion(out, m.to);
            return out;
        }
    return input;
}

bool brew_changes(const ItemStack& ingredient, const ItemStack& input) {
    ItemStack out = brew_mix(ingredient, input);
    return !ItemStack::is_same_item_same_components(out, input);
}

}  // namespace pn::sim
