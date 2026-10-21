// BlockState.canSurvive for the blocks features place: plants, fire, carpets, columns. The rule comes from the block's class, as in Mojang's code.
#include "feature.hpp"
#include <algorithm>

namespace pn::vg {

namespace {

bool has_class(const std::vector<std::string>& chain, const char* name) { return std::find(chain.begin(), chain.end(), name) != chain.end(); }

}  // namespace

bool state_can_survive(FeatureEnv& env, uint16_t state, const BlockPos& pos) {
    const BlockData& bd = env.bd();
    const Level& lv = env.level;
    const auto& cls = bd.classes(state);
    if (cls.empty()) return true;
    const std::string& name = bd.name(state);
    const BlockPos below = pos.below();
    const uint16_t below_state = lv.get(below);
    auto in_tag = [&](uint16_t s, const char* tag) { return bd.in_tag(s, tag); };
    auto sturdy_up = [&](uint16_t s) { return bd.sturdy(s, UP); };

    const std::string& c0 = cls[0];
    // blocks with their own rule, most specific first
    if (c0 == "TallSeagrassBlock" || c0 == "SeagrassBlock") {
        auto may_place_on = [&](uint16_t s) { return sturdy_up(s) && !in_tag(s, "minecraft:cannot_support_seagrass"); };
        if (c0 == "TallSeagrassBlock") {
            if (bd.prop(state, "half") == "upper") return bd.name(below_state) == name && bd.prop(below_state, "half") == "lower";
            uint16_t here = lv.get(pos);
            return may_place_on(below_state) && bd.water(here) && (bd.flags(here) & BF_SOURCE);
        }
        return may_place_on(below_state);
    }
    if (c0 == "KelpBlock" || c0 == "KelpPlantBlock") {
        if (in_tag(below_state, "minecraft:cannot_support_kelp")) return false;
        return bd.name(below_state) == "minecraft:kelp" || bd.name(below_state) == "minecraft:kelp_plant" || sturdy_up(below_state);
    }
    if (c0 == "CactusBlock") {
        for (int d = NORTH; d <= EAST; d++) {
            BlockPos n = pos + Dir(d);
            uint16_t ns = lv.get(n);
            if ((bd.flags(ns) & BF_SOLID) || bd.lava(ns)) return false;
        }
        return (name == bd.name(below_state) || in_tag(below_state, "minecraft:supports_cactus")) && !(bd.flags(lv.get(pos.above())) & BF_LIQUID);
    }
    if (c0 == "SugarCaneBlock") {
        if (bd.name(below_state) == name) return true;
        if (!in_tag(below_state, "minecraft:supports_sugar_cane")) return false;
        for (int d = NORTH; d <= EAST; d++) {
            uint16_t ns = lv.get(below + Dir(d));
            if (bd.water(ns) || in_tag(ns, "minecraft:supports_sugar_cane_adjacently")) return true;
        }
        return false;
    }
    if (c0 == "LilyPadBlock") {
        bool supports = (bd.water(below_state) && (bd.flags(below_state) & BF_SOURCE)) || in_tag(below_state, "minecraft:supports_lily_pad");
        return supports && !bd.fluid(lv.get(pos));
    }
    if (c0 == "MushroomBlock") {
        if (in_tag(below_state, "minecraft:overrides_mushroom_light_requirement")) return true;
        return (bd.flags(below_state) & BF_SOLID_RENDER) != 0;   // light is not computed during generation: raw brightness reads as dark
    }
    if (c0 == "MangrovePropaguleBlock") {
        if (bd.prop(state, "hanging") == "true") return in_tag(lv.get(pos.above()), "minecraft:supports_hanging_mangrove_propagule");
        return in_tag(below_state, "minecraft:supports_mangrove_propagule");
    }
    if (c0 == "AzaleaBlock") return in_tag(below_state, "minecraft:supports_azalea");
    if (c0 == "DryVegetationBlock") return in_tag(below_state, "minecraft:supports_dry_vegetation");
    if (c0 == "NetherSproutsBlock") return in_tag(below_state, "minecraft:supports_nether_sprouts");
    if (c0 == "NetherRootsBlock") return in_tag(below_state, name == "minecraft:crimson_roots" ? "minecraft:supports_crimson_roots" : "minecraft:supports_warped_roots");
    if (c0 == "LeafLitterBlock") return sturdy_up(below_state);
    if (c0 == "CactusFlowerBlock") return in_tag(below_state, "minecraft:support_override_cactus_flower") || sturdy_up(below_state);
    if (c0 == "MossyCarpetBlock") return bd.prop(state, "bottom") == "true" ? !bd.air(below_state) : bd.name(below_state) == name && bd.prop(below_state, "bottom") == "true";
    if (c0 == "CarpetBlock") return !bd.air(below_state);
    if (c0 == "SporeBlossomBlock") return bd.sturdy(lv.get(pos.above()), DOWN) && !bd.water(lv.get(pos));
    if (c0 == "SoulFireBlock") return in_tag(below_state, "minecraft:soul_fire_base_blocks");
    if (c0 == "FireBlock") {
        if (sturdy_up(below_state)) return true;
        for (int d = 0; d < 6; d++) if (bd.flags(lv.get(pos + Dir(d))) & BF_IGNITED_BY_LAVA) return true;   // burnable neighbours (flammable blocks)
        return false;
    }
    if (c0 == "SmallDripleafBlock") {
        if (bd.prop(state, "half") == "upper") return bd.name(below_state) == name && bd.prop(below_state, "half") == "lower";
        uint16_t here = lv.get(pos);
        return in_tag(below_state, "minecraft:supports_small_dripleaf") || (bd.water(here) && (bd.flags(here) & BF_SOURCE) && in_tag(below_state, "minecraft:supports_vegetation"));
    }
    if (has_class(cls, "DoublePlantBlock")) {
        if (bd.prop(state, "half") == "upper") return bd.name(below_state) == name && bd.prop(below_state, "half") == "lower";
        return in_tag(below_state, "minecraft:supports_vegetation");
    }
    if (has_class(cls, "VegetationBlock")) return in_tag(below_state, "minecraft:supports_vegetation");
    return true;
}

}  // namespace pn::vg
