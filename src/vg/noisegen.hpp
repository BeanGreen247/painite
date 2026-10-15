// The terrain fill of one chunk: Mojang's NoiseChunk (cells, interpolation), aquifers and ore veins, producing the base blocks of the dimension.
#pragma once
#include "density.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace pn::vg {

// Block states the fill needs, as the server's internal ids
struct FillPalette {
    uint16_t air = 0, default_block = 0, default_fluid = 0, lava = 0, water = 0;
    uint16_t copper_ore = 0, raw_copper_block = 0, granite = 0, deepslate_iron_ore = 0, raw_iron_block = 0, tuff = 0;
};

struct NoiseSettingsData {
    int min_y = -64, height = 384, size_horizontal = 1, size_vertical = 2, sea_level = 63;
    bool aquifers = true, ore_veins = true;
    const Json* router = nullptr;   // noise_router
    static NoiseSettingsData from_json(const Json& settings);
    int cell_width() const { return size_horizontal * 4; }
    int cell_height() const { return size_vertical * 4; }
};

class Aquifer;

class ChunkFill {
public:
    // dim_min_y/dim_height: the dimension's own range (clamps the noise range like clampToHeightAccessor)
    ChunkFill(const WorldState& state, const NoiseSettingsData& ns, const FillPalette& pal, int chunk_x, int chunk_z, int dim_min_y, int dim_height);
    // a one cell wide noise chunk around block column (bx, bz), as NoiseBasedChunkGenerator.iterateNoiseColumn builds it (the aquifer belongs to the column's chunk)
    struct ColumnTag {};
    ChunkFill(const WorldState& state, const NoiseSettingsData& ns, const FillPalette& pal, int bx, int bz, int dim_min_y, int dim_height, ColumnTag);
    // iterateNoiseColumn for the column given to the constructor: scans from the top, returns y + 1 of the first block `tester` accepts (min_y when none);
    // `states`, when given, receives every block of the column from min_y up
    int column(int bx, int bz, const std::function<bool(uint16_t)>& tester, std::vector<uint16_t>* states = nullptr);
    ~ChunkFill();
    // blocks[(y - min_y) * 256 + z * 16 + x], y over [min_y, min_y + height); air where nothing is placed
    void fill(std::vector<uint16_t>& blocks, std::vector<uint8_t>* fluid_update_marks = nullptr);
    void set_beardifier(const Beardifier* b) { cn_.beardifier = b; }
    int min_y() const { return min_y_; }
    int height() const { return height_; }
    int preliminary_surface_level(int x, int z);
    int max_preliminary_surface_level(int min_x, int min_z, int max_x, int max_z);
    // the aquifer of this chunk, for carvers: false = no override
    bool aquifer_substance(int x, int y, int z, double density, uint16_t& out);
    bool aquifer_should_schedule() const;
    NodeP node(const char* name) const;   // wrapped router functions: erosion, depth, ...
private:
    void init(int chunk_x, int chunk_z, int dim_min_y, int dim_height, bool column, int bx, int bz);
    friend class Aquifer;
    uint16_t state_at(int x, int y, int z, double density);
    const WorldState& state_;
    NoiseSettingsData ns_;
    FillPalette pal_;
    int chunk_x_, chunk_z_, min_y_, height_;
    ChunkNoise cn_;
    std::unique_ptr<Compiler> comp_;
    NodeP prelim_, final_density_, barrier_, flood_, spread_, lava_noise_, erosion_, depth_, vein_toggle_, vein_ridged_, vein_gap_;
    NodeP full_;   // cache_all_in_cell(final_density + beardifier)
    std::unique_ptr<Aquifer> aquifer_;
    std::map<int64_t, int> prelim_cache_;
    bool disabled_aquifer_ = false;
};

}  // namespace pn::vg
