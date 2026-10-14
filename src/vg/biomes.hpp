// The climate points of the overworld and nether biome sources (Mojang's OverworldBiomeBuilder and the nether preset), in Mojang's insertion order.
#pragma once
#include "climate.hpp"

namespace pn::vg {

ClimateData overworld_climate();
ClimateData nether_climate();

}  // namespace pn::vg
