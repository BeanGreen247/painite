// The climate points of the overworld and nether biome sources (Mojang's OverworldBiomeBuilder and the nether preset), in Mojang's insertion order.
#pragma once
#include "climate.hpp"

namespace pn::vg {

// sulfur_caves came with 26.2: with_sulfur_caves=false reproduces 26.1
ClimateData overworld_climate(bool with_sulfur_caves = true);
ClimateData nether_climate();

}  // namespace pn::vg
