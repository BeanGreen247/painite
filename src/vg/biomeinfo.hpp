// What the biome class offers world generation: temperature (with the height and frozen-ocean adjustments) and precipitation.
#pragma once
#include "noise.hpp"
#include <string>

namespace pn::vg {

struct BiomeInfo {
    std::string name;            // minecraft:plains
    float temperature = 0.8f;
    bool frozen = false;         // temperature_modifier: frozen
    bool has_precipitation = true;
};

// Biome.getHeightAdjustedTemperature
inline float biome_temperature(const BiomeInfo& b, int x, int y, int z, int sea_level) {
    float t = b.temperature;
    if (b.frozen) {
        double large = biome_frozen_noise().get_value(x * 0.05, z * 0.05, false) * 7.0;
        double edge = biome_info_noise().get_value(x * 0.2, z * 0.2, false);
        if (large + edge < 0.3) {
            double small = biome_info_noise().get_value(x * 0.09, z * 0.09, false);
            if (small < 0.8) t = 0.2f;
        }
    }
    int snow_level = sea_level + 17;
    if (y > snow_level) {
        float v = float(biome_temperature_noise().get_value(double(float(x) / 8.0f), double(float(z) / 8.0f), false) * 8.0);
        return t - (v + float(y) - float(snow_level)) * 0.05f / 40.0f;
    }
    return t;
}
inline bool biome_warm_enough_to_rain(const BiomeInfo& b, int x, int y, int z, int sea) { return biome_temperature(b, x, y, z, sea) >= 0.15f; }
inline bool biome_cold_enough_to_snow(const BiomeInfo& b, int x, int y, int z, int sea) { return !biome_warm_enough_to_rain(b, x, y, z, sea); }

}  // namespace pn::vg
