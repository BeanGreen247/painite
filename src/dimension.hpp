// The three dimensions and what differs between them.
#pragma once
#include <string>

namespace pn {

constexpr int kDimensions = 3;   // 0 overworld, 1 the nether, 2 the end

struct DimInfo {
    const char* name;        // minecraft:the_nether
    const char* dir_name;    // the_nether (folder under dimensions/minecraft)
    int min_y, height;
    bool sky;                // has skylight
    int sea_level;
};

inline const DimInfo& dim_info(int d) {
    static const DimInfo infos[kDimensions] = {{"minecraft:overworld", "overworld", -64, 384, true, 63},
                                               {"minecraft:the_nether", "the_nether", 0, 256, false, 32},
                                               {"minecraft:the_end", "the_end", 0, 256, false, 0}};
    return infos[d < 0 || d >= kDimensions ? 0 : d];
}

inline int dim_from_name(const std::string& n) {
    if (n == "minecraft:the_nether" || n == "the_nether" || n == "nether") return 1;
    if (n == "minecraft:the_end" || n == "the_end" || n == "end") return 2;
    return 0;
}

}  // namespace pn
