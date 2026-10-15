#include "noisegen.hpp"
#include <cmath>
#include <climits>

namespace pn::vg {

NoiseSettingsData NoiseSettingsData::from_json(const Json& s) {
    NoiseSettingsData d;
    const Json& n = s.at("noise");
    d.min_y = int(n.at("min_y").int_value());
    d.height = int(n.at("height").int_value());
    d.size_horizontal = int(n.at("size_horizontal").int_value());
    d.size_vertical = int(n.at("size_vertical").int_value());
    d.sea_level = int(s.at("sea_level").int_value());
    d.aquifers = s.get_bool("aquifers_enabled", true);
    d.ore_veins = s.get_bool("ore_veins_enabled", true);
    d.router = &s.at("noise_router");
    return d;
}

namespace {

inline int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline int floor_mod(int a, int b) { return a - floor_div(a, b) * b; }

struct FluidStatus {
    int level = 0;
    uint16_t type = 0;
    uint16_t at(int y, uint16_t air) const { return y < level ? type : air; }
    bool operator==(const FluidStatus& o) const { return level == o.level && type == o.type; }
};

}  // namespace

class Aquifer {
public:
    Aquifer(ChunkFill& cf, int chunk_x, int chunk_z, int min_block_y, int y_block_size, bool enabled) : cf_(cf), pal_(cf.pal_), enabled_(enabled) {
        const int sea = cf.ns_.sea_level;
        lava_ = {-54, pal_.lava};
        sea_ = {sea, pal_.default_fluid};
        if (!enabled) return;
        pos_random_ = &cf.state_.aquifer_random();
        min_grid_x_ = grid_x(chunk_x * 16 - 5) + 0;
        int max_grid_x = grid_x(chunk_x * 16 + 15 - 5) + 1;
        grid_size_x_ = max_grid_x - min_grid_x_ + 1;
        min_grid_y_ = grid_y(min_block_y + 1) - 1;
        int max_grid_y = grid_y(min_block_y + y_block_size + 1) + 1;
        int grid_size_y = max_grid_y - min_grid_y_ + 1;
        min_grid_z_ = grid_z(chunk_z * 16 - 5) + 0;
        int max_grid_z = grid_z(chunk_z * 16 + 15 - 5) + 1;
        grid_size_z_ = max_grid_z - min_grid_z_ + 1;
        size_t total = size_t(grid_size_x_) * size_t(grid_size_y) * size_t(grid_size_z_);
        status_.assign(total, FluidStatus());
        have_status_.assign(total, 0);
        loc_.assign(total, Loc{0, 0, 0, false});
        int max_adjusted = adjust(cf.max_preliminary_surface_level(from_grid_x(min_grid_x_, 0), from_grid_z(min_grid_z_, 0), from_grid_x(max_grid_x, 9), from_grid_z(max_grid_z, 9)));
        int skip_grid_y = grid_y(max_adjusted + 12) + 1;
        skip_above_y_ = from_grid_y(skip_grid_y, 11) - 1;
    }

    FluidStatus global_fluid(int, int y, int) const { return y < std::min(-54, cf_.ns_.sea_level) ? lava_ : sea_; }

    // returns false for "no override" (the default block), else the state in `out`
    bool compute_substance(int pos_x, int pos_y, int pos_z, double density, uint16_t& out) {
        const uint16_t air = pal_.air;
        if (!enabled_) {
            if (density > 0.0) return false;
            out = global_fluid(pos_x, pos_y, pos_z).at(pos_y, air);
            return true;
        }
        if (density > 0.0) { should_schedule_ = false; return false; }
        FluidStatus global = global_fluid(pos_x, pos_y, pos_z);
        if (pos_y > skip_above_y_) { should_schedule_ = false; out = global.at(pos_y, air); return true; }
        if (global.at(pos_y, air) == pal_.lava) { should_schedule_ = false; out = pal_.lava; return true; }
        int x_anchor = grid_x(pos_x - 5), y_anchor = grid_y(pos_y + 1), z_anchor = grid_z(pos_z - 5);
        int d1 = INT_MAX, d2 = INT_MAX, d3 = INT_MAX, d4 = INT_MAX;
        int c1 = 0, c2 = 0, c3 = 0, c4 = 0;
        for (int x1 = 0; x1 <= 1; x1++)
            for (int y1 = -1; y1 <= 1; y1++)
                for (int z1 = 0; z1 <= 1; z1++) {
                    int sx = x_anchor + x1, sy = y_anchor + y1, sz = z_anchor + z1;
                    int index = get_index(sx, sy, sz);
                    Loc& loc = loc_[size_t(index)];
                    if (!loc.set) {
                        auto r = pos_random_->at(sx, sy, sz);
                        int rx = r->next_int(10), ry = r->next_int(9), rz = r->next_int(10);
                        loc = Loc{from_grid_x(sx, rx), from_grid_y(sy, ry), from_grid_z(sz, rz), true};
                    }
                    int dx = loc.x - pos_x, dy = loc.y - pos_y, dz = loc.z - pos_z;
                    int nd = dx * dx + dy * dy + dz * dz;
                    if (d1 >= nd) { c4 = c3; c3 = c2; c2 = c1; c1 = index; d4 = d3; d3 = d2; d2 = d1; d1 = nd; }
                    else if (d2 >= nd) { c4 = c3; c3 = c2; c2 = index; d4 = d3; d3 = d2; d2 = nd; }
                    else if (d3 >= nd) { c4 = c3; c3 = index; d4 = d3; d3 = nd; }
                    else if (d4 >= nd) { c4 = index; d4 = nd; }
                }
        FluidStatus s1 = status_of(c1);
        double sim12 = similarity(d1, d2);
        uint16_t fluid_state = s1.at(pos_y, air);
        if (sim12 <= 0.0) {
            if (sim12 >= kFlowing) { FluidStatus s2 = status_of(c2); should_schedule_ = !(s1 == s2); }
            else should_schedule_ = false;
            out = fluid_state;
            return true;
        }
        if (fluid_state == pal_.water && global_fluid(pos_x, pos_y - 1, pos_z).at(pos_y - 1, air) == pal_.lava) { should_schedule_ = true; out = fluid_state; return true; }
        double barrier_value = NAN;
        FluidStatus s2 = status_of(c2);
        double barrier12 = sim12 * pressure(pos_x, pos_y, pos_z, barrier_value, s1, s2);
        if (density + barrier12 > 0.0) { should_schedule_ = false; return false; }
        FluidStatus s3 = status_of(c3);
        double sim13 = similarity(d1, d3);
        if (sim13 > 0.0) {
            double barrier13 = sim12 * sim13 * pressure(pos_x, pos_y, pos_z, barrier_value, s1, s3);
            if (density + barrier13 > 0.0) { should_schedule_ = false; return false; }
        }
        double sim23 = similarity(d2, d3);
        if (sim23 > 0.0) {
            double barrier23 = sim12 * sim23 * pressure(pos_x, pos_y, pos_z, barrier_value, s2, s3);
            if (density + barrier23 > 0.0) { should_schedule_ = false; return false; }
        }
        bool may12 = !(s1 == s2);
        bool may23 = sim23 >= kFlowing && !(s2 == s3);
        bool may13 = sim13 >= kFlowing && !(s1 == s3);
        if (!may12 && !may23 && !may13) should_schedule_ = sim13 >= kFlowing && similarity(d1, d4) >= kFlowing && !(s1 == status_of(c4));
        else should_schedule_ = true;
        out = fluid_state;
        return true;
    }
    bool should_schedule() const { return should_schedule_; }

private:
    struct Loc { int x, y, z; bool set; };
    static constexpr double kFlowing = 1.0 - (double(12 * 12) - double(10 * 10)) / 25.0;   // similarity(100, 144)
    static double similarity(int d1, int d2) { return 1.0 - double(d2 - d1) / 25.0; }
    static int grid_x(int b) { return b >> 4; }
    static int grid_y(int b) { return floor_div(b, 12); }
    static int grid_z(int b) { return b >> 4; }
    static int from_grid_x(int g, int off) { return (g << 4) + off; }
    static int from_grid_y(int g, int off) { return g * 12 + off; }
    static int from_grid_z(int g, int off) { return (g << 4) + off; }
    static int adjust(int prelim) { return prelim + 8; }
    int get_index(int gx, int gy, int gz) const { return ((gy - min_grid_y_) * grid_size_z_ + (gz - min_grid_z_)) * grid_size_x_ + (gx - min_grid_x_); }

    FluidStatus status_of(int index) {
        if (have_status_[size_t(index)]) return status_[size_t(index)];
        const Loc& l = loc_[size_t(index)];
        FluidStatus s = compute_fluid(l.x, l.y, l.z);
        status_[size_t(index)] = s;
        have_status_[size_t(index)] = 1;
        return s;
    }

    double pressure(int pos_x, int pos_y, int pos_z, double& barrier_value, const FluidStatus& a, const FluidStatus& b) {
        const uint16_t air = pal_.air;
        uint16_t t1 = a.at(pos_y, air), t2 = b.at(pos_y, air);
        if ((t1 != pal_.lava || t2 != pal_.water) && (t1 != pal_.water || t2 != pal_.lava)) {
            int y_diff = std::abs(a.level - b.level);
            if (y_diff == 0) return 0.0;
            double average_y = 0.5 * double(a.level + b.level);
            double above = double(pos_y) + 0.5 - average_y;
            double base = double(y_diff) / 2.0;
            double edge_toward_middle = base - std::fabs(above);
            double gradient;
            if (above > 0.0) {
                double center = 0.0 + edge_toward_middle;
                gradient = center > 0.0 ? center / 1.5 : center / 2.5;
            } else {
                double center = 3.0 + edge_toward_middle;
                gradient = center > 0.0 ? center / 3.0 : center / 10.0;
            }
            double noise_value;
            if (!(gradient < -2.0) && !(gradient > 2.0)) {
                if (std::isnan(barrier_value)) barrier_value = cf_.barrier_->compute(Ctx{pos_x, pos_y, pos_z, &cf_.cn_});
                noise_value = barrier_value;
            } else noise_value = 0.0;
            return 2.0 * (noise_value + gradient);
        }
        return 2.0;
    }

    FluidStatus compute_fluid(int x, int y, int z) {
        FluidStatus global = global_fluid(x, y, z);
        int lowest_prelim = INT_MAX;
        int top_of_cell = y + 12, bottom_of_cell = y - 12;
        bool surface_at_center_under_global = false;
        static const int offsets[13][2] = {{0, 0}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0}, {-1, 0}, {1, 0}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}};
        for (auto& off : offsets) {
            int sx = x + (off[0] << 4), sz = z + (off[1] << 4);
            int prelim = cf_.preliminary_surface_level(sx, sz);
            int adjusted = adjust(prelim);
            bool start = off[0] == 0 && off[1] == 0;
            if (start && bottom_of_cell > adjusted) return global;
            bool top_pokes = top_of_cell > adjusted;
            if (top_pokes || start) {
                FluidStatus at_surface = global_fluid(sx, adjusted, sz);
                if (at_surface.at(adjusted, pal_.air) != pal_.air) {
                    if (start) surface_at_center_under_global = true;
                    if (top_pokes) return at_surface;
                }
            }
            lowest_prelim = std::min(lowest_prelim, prelim);
        }
        int level = compute_surface_level(x, y, z, global, lowest_prelim, surface_at_center_under_global);
        return FluidStatus{level, compute_fluid_type(x, y, z, global, level)};
    }

    int compute_surface_level(int x, int y, int z, const FluidStatus& global, int lowest_prelim, bool center_under_global) {
        Ctx ctx{x, y, z, nullptr};
        double partially, fully;
        if (cf_.erosion_->compute(ctx) < double(-0.225f) && cf_.depth_->compute(ctx) > double(0.9f)) { partially = -1.0; fully = -1.0; }
        else {
            int below = lowest_prelim + 8 - y;
            double floodedness_factor = center_under_global ? clamped_map(double(below), 0.0, 64.0, 1.0, 0.0) : 0.0;
            double noise_value = clamp(cf_.flood_->compute(ctx), -1.0, 1.0);
            double fully_threshold = lerp(inverse_lerp(floodedness_factor, 1.0, 0.0), -0.3, 0.8);
            double partially_threshold = lerp(inverse_lerp(floodedness_factor, 1.0, 0.0), -0.8, 0.4);
            partially = noise_value - partially_threshold;
            fully = noise_value - fully_threshold;
        }
        if (fully > 0.0) return global.level;
        if (partially > 0.0) return randomized_level(x, y, z, lowest_prelim);
        return -32512;   // DimensionType.WAY_BELOW_MIN_Y
    }

    int randomized_level(int x, int y, int z, int lowest_prelim) {
        int cx = floor_div(x, 16), cy = floor_div(y, 40), cz = floor_div(z, 16);
        int middle_y = cy * 40 + 20;
        double spread = cf_.spread_->compute(Ctx{cx, cy, cz, nullptr}) * 10.0;
        int quantized = int(std::floor(spread / 3)) * 3;
        int target = middle_y + quantized;
        return std::min(lowest_prelim, target);
    }

    uint16_t compute_fluid_type(int x, int y, int z, const FluidStatus& global, int level) {
        uint16_t type = global.type;
        if (level <= -10 && level != -32512 && global.type != pal_.lava) {
            int cx = floor_div(x, 64), cy = floor_div(y, 40), cz = floor_div(z, 64);
            double lava_value = cf_.lava_noise_->compute(Ctx{cx, cy, cz, nullptr});
            if (std::fabs(lava_value) > 0.3) type = pal_.lava;
        }
        return type;
    }

    ChunkFill& cf_;
    const FillPalette& pal_;
    bool enabled_;
    const PosFactory* pos_random_ = nullptr;
    FluidStatus lava_, sea_;
    std::vector<FluidStatus> status_;
    std::vector<uint8_t> have_status_;
    std::vector<Loc> loc_;
    int min_grid_x_ = 0, min_grid_y_ = 0, min_grid_z_ = 0, grid_size_x_ = 0, grid_size_z_ = 0, skip_above_y_ = 0;
    bool should_schedule_ = false;
};

ChunkFill::~ChunkFill() = default;

ChunkFill::ChunkFill(const WorldState& state, const NoiseSettingsData& ns, const FillPalette& pal, int chunk_x, int chunk_z, int dim_min_y, int dim_height)
    : state_(state), ns_(ns), pal_(pal), chunk_x_(chunk_x), chunk_z_(chunk_z) {
    init(chunk_x, chunk_z, dim_min_y, dim_height, false, 0, 0);
}

ChunkFill::ChunkFill(const WorldState& state, const NoiseSettingsData& ns, const FillPalette& pal, int bx, int bz, int dim_min_y, int dim_height, ColumnTag)
    : state_(state), ns_(ns), pal_(pal), chunk_x_(bx >> 4), chunk_z_(bz >> 4) {
    init(bx >> 4, bz >> 4, dim_min_y, dim_height, true, bx, bz);
}

void ChunkFill::init(int chunk_x, int chunk_z, int dim_min_y, int dim_height, bool column, int bx, int bz) {
    const NoiseSettingsData& ns = ns_;
    // clampToHeightAccessor
    min_y_ = std::max(ns.min_y, dim_min_y);
    height_ = std::min(ns.min_y + ns.height, dim_min_y + dim_height) - min_y_;
    cn_.cell_width = ns.cell_width();
    cn_.cell_height = ns.cell_height();
    cn_.cell_count_xz = column ? 1 : 16 / cn_.cell_width;
    cn_.cell_count_y = floor_div(height_, cn_.cell_height);
    cn_.cell_noise_min_y = floor_div(min_y_, cn_.cell_height);
    cn_.first_cell_x = floor_div(column ? bx : chunk_x * 16, cn_.cell_width);
    cn_.first_cell_z = floor_div(column ? bz : chunk_z * 16, cn_.cell_width);
    cn_.first_noise_x = (cn_.first_cell_x * cn_.cell_width) >> 2;
    cn_.first_noise_z = (cn_.first_cell_z * cn_.cell_width) >> 2;
    cn_.noise_size_xz = (cn_.cell_count_xz * cn_.cell_width) >> 2;
    comp_ = std::make_unique<Compiler>(state_, &cn_);
    const Json& r = *ns.router;
    prelim_ = comp_->compile(r.at("preliminary_surface_level"));
    final_density_ = comp_->compile(r.at("final_density"));
    barrier_ = comp_->compile(r.at("barrier"));
    flood_ = comp_->compile(r.at("fluid_level_floodedness"));
    spread_ = comp_->compile(r.at("fluid_level_spread"));
    lava_noise_ = comp_->compile(r.at("lava"));
    erosion_ = comp_->compile(r.at("erosion"));
    depth_ = comp_->compile(r.at("depth"));
    vein_toggle_ = comp_->compile(r.at("vein_toggle"));
    vein_ridged_ = comp_->compile(r.at("vein_ridged"));
    vein_gap_ = comp_->compile(r.at("vein_gap"));
    // cache_all_in_cell(final_density + beardifier)
    {
        struct Add2 : Node {
            NodeP a, b;
            double compute(const Ctx& c) const override { return a->compute(c) + b->compute(c); }
        };
        auto add = std::make_shared<Add2>();
        add->a = final_density_;
        add->b = comp_->compile(Json::parse("{\"type\":\"minecraft:beardifier\"}"));
        full_ = std::make_shared<CacheAllInCellNode>(&cn_, add);
    }
    aquifer_ = std::make_unique<Aquifer>(*this, chunk_x, chunk_z, min_y_, height_, ns.aquifers);
}

int ChunkFill::column(int bx, int bz, const std::function<bool(uint16_t)>& tester, std::vector<uint16_t>* states) {
    if (cn_.cell_count_y <= 0) return min_y_;
    const int cw = cn_.cell_width, ch = cn_.cell_height;
    if (states) states->assign(size_t(height_), pal_.air);
    cn_.interpolating = true;
    auto fill_slice = [&](bool slice0, int cell_x) {
        cn_.cell_start_x = cell_x * cw;
        cn_.in_cell_x = 0;
        for (int zi = 0; zi < cn_.cell_count_xz + 1; zi++) {
            cn_.cell_start_z = (cn_.first_cell_z + zi) * cw;
            cn_.in_cell_z = 0;
            cn_.in_cell_y = 0;
            for (auto* it : cn_.interpolators) it->fill_slice(slice0, zi);
        }
    };
    fill_slice(true, cn_.first_cell_x);
    fill_slice(false, cn_.first_cell_x + 1);
    cn_.cell_start_x = cn_.first_cell_x * cw;
    const double factor_x = double(floor_mod(bx, cw)) / cw, factor_z = double(floor_mod(bz, cw)) / cw;
    int result = min_y_;
    bool found = false;
    for (int cell_y_index = cn_.cell_count_y - 1; cell_y_index >= 0 && !found; cell_y_index--) {
        for (auto* it : cn_.interpolators) it->select_cell_yz(cell_y_index, 0);
        cn_.filling_cell = true;
        cn_.cell_start_y = (cell_y_index + cn_.cell_noise_min_y) * ch;
        cn_.cell_start_z = cn_.first_cell_z * cw;
        for (auto* cc : cn_.cell_caches) cc->fill_cell();
        cn_.filling_cell = false;
        for (int y_in_cell = ch - 1; y_in_cell >= 0; y_in_cell--) {
            int pos_y = (cn_.cell_noise_min_y + cell_y_index) * ch + y_in_cell;
            cn_.in_cell_y = pos_y - cn_.cell_start_y;
            for (auto* it : cn_.interpolators) it->update_for_y(double(y_in_cell) / ch);
            cn_.in_cell_x = floor_mod(bx, cw);
            for (auto* it : cn_.interpolators) it->update_for_x(factor_x);
            cn_.in_cell_z = floor_mod(bz, cw);
            cn_.epoch++;
            for (auto* it : cn_.interpolators) it->update_for_z(factor_z);
            double density = full_->compute(cn_.here());
            uint16_t st = state_at(bx, pos_y, bz, density);
            if (st == 0xffff) st = pal_.default_block;
            if (states) (*states)[size_t(pos_y - min_y_)] = st;
            if (!found && tester && tester(st)) { result = pos_y + 1; found = true; if (!states) break; }
        }
    }
    cn_.interpolating = false;
    return result;
}

bool ChunkFill::aquifer_substance(int x, int y, int z, double density, uint16_t& out) { return aquifer_->compute_substance(x, y, z, density, out); }
bool ChunkFill::aquifer_should_schedule() const { return aquifer_->should_schedule(); }

NodeP ChunkFill::node(const char* name) const {
    std::string n = name;
    if (n == "erosion") return erosion_;
    if (n == "depth") return depth_;
    if (n == "prelim") return prelim_;
    return nullptr;
}

int ChunkFill::preliminary_surface_level(int sx, int sz) {
    int qx = (sx >> 2) << 2, qz = (sz >> 2) << 2;
    int64_t key = (int64_t(qx) << 32) | uint32_t(qz);
    auto it = prelim_cache_.find(key);
    if (it != prelim_cache_.end()) return it->second;
    int v = int(std::floor(prelim_->compute(Ctx{qx, 0, qz, nullptr})));
    prelim_cache_[key] = v;
    return v;
}

int ChunkFill::max_preliminary_surface_level(int min_x, int min_z, int max_x, int max_z) {
    int max_y = INT_MIN;
    for (int z = min_z; z <= max_z; z += 4)
        for (int x = min_x; x <= max_x; x += 4) max_y = std::max(max_y, preliminary_surface_level(x, z));
    return max_y;
}

uint16_t ChunkFill::state_at(int x, int y, int z, double density) {
    uint16_t out;
    if (aquifer_->compute_substance(x, y, z, density, out)) return out;
    if (ns_.ore_veins) {
        // OreVeinifier
        Ctx ctx{x, y, z, &cn_};
        double toggle = vein_toggle_->compute(ctx);
        bool copper = toggle > 0.0;
        int vmax = copper ? 50 : -8, vmin = copper ? 0 : -60;
        double ridged = std::fabs(toggle);
        int from_top = vmax - y, from_bottom = y - vmin;
        if (from_bottom >= 0 && from_top >= 0) {
            int from_edge = std::min(from_top, from_bottom);
            double round_off = clamped_map(double(from_edge), 0.0, 20.0, -0.2, 0.0);
            if (!(ridged + round_off < double(0.4f))) {
                auto r = state_.ore_random().at(x, y, z);
                if (!(r->next_float() > 0.7f)) {
                    if (!(vein_ridged_->compute(ctx) >= 0.0)) {
                        double richness = clamped_map(ridged, double(0.4f), double(0.6f), double(0.1f), double(0.3f));
                        if (r->next_float() < float(richness) && vein_gap_->compute(ctx) > double(-0.3f))
                            return r->next_float() < 0.02f ? (copper ? pal_.raw_copper_block : pal_.raw_iron_block) : (copper ? pal_.copper_ore : pal_.deepslate_iron_ore);
                        return copper ? pal_.granite : pal_.tuff;
                    }
                }
            }
        }
    }
    return 0xffff;   // no override: the default block
}

void ChunkFill::fill(std::vector<uint16_t>& blocks, std::vector<uint8_t>* marks) {
    blocks.assign(size_t(height_) * 256, pal_.air);
    if (marks) marks->assign(size_t(height_) * 256, 0);
    if (cn_.cell_count_y <= 0) return;
    const int cw = cn_.cell_width, ch = cn_.cell_height;
    // initializeForFirstCellX
    cn_.interpolating = true;
    auto fill_slice = [&](bool slice0, int cell_x) {
        cn_.cell_start_x = cell_x * cw;
        cn_.in_cell_x = 0;
        for (int zi = 0; zi < cn_.cell_count_xz + 1; zi++) {
            cn_.cell_start_z = (cn_.first_cell_z + zi) * cw;
            cn_.in_cell_z = 0;
            cn_.in_cell_y = 0;
            for (auto* it : cn_.interpolators) it->fill_slice(slice0, zi);
        }
    };
    fill_slice(true, cn_.first_cell_x);
    for (int cell_x_index = 0; cell_x_index < 16 / cw; cell_x_index++) {
        fill_slice(false, cn_.first_cell_x + cell_x_index + 1);
        cn_.cell_start_x = (cn_.first_cell_x + cell_x_index) * cw;
        for (int cell_z_index = 0; cell_z_index < 16 / cw; cell_z_index++) {
            for (int cell_y_index = cn_.cell_count_y - 1; cell_y_index >= 0; cell_y_index--) {
                // selectCellYZ
                for (auto* it : cn_.interpolators) it->select_cell_yz(cell_y_index, cell_z_index);
                cn_.filling_cell = true;
                cn_.cell_start_y = (cell_y_index + cn_.cell_noise_min_y) * ch;
                cn_.cell_start_z = (cn_.first_cell_z + cell_z_index) * cw;
                for (auto* cc : cn_.cell_caches) cc->fill_cell();
                cn_.filling_cell = false;
                for (int y_in_cell = ch - 1; y_in_cell >= 0; y_in_cell--) {
                    int pos_y = (cn_.cell_noise_min_y + cell_y_index) * ch + y_in_cell;
                    double factor_y = double(y_in_cell) / ch;
                    cn_.in_cell_y = pos_y - cn_.cell_start_y;
                    for (auto* it : cn_.interpolators) it->update_for_y(factor_y);
                    for (int x_in_cell = 0; x_in_cell < cw; x_in_cell++) {
                        int pos_x = chunk_x_ * 16 + cell_x_index * cw + x_in_cell;
                        double factor_x = double(x_in_cell) / cw;
                        cn_.in_cell_x = pos_x - cn_.cell_start_x;
                        for (auto* it : cn_.interpolators) it->update_for_x(factor_x);
                        for (int z_in_cell = 0; z_in_cell < cw; z_in_cell++) {
                            int pos_z = chunk_z_ * 16 + cell_z_index * cw + z_in_cell;
                            double factor_z = double(z_in_cell) / cw;
                            cn_.in_cell_z = pos_z - cn_.cell_start_z;
                            cn_.epoch++;
                            for (auto* it : cn_.interpolators) it->update_for_z(factor_z);
                            Ctx here = cn_.here();
                            double density = full_->compute(here);
                            uint16_t st = state_at(pos_x, pos_y, pos_z, density);
                            if (st == 0xffff) st = pal_.default_block;
                            if (st != pal_.air) {
                                size_t idx = size_t(pos_y - min_y_) * 256 + size_t(pos_z & 15) * 16 + size_t(pos_x & 15);
                                blocks[idx] = st;
                                if (marks && aquifer_->should_schedule() && (st == pal_.water || st == pal_.lava)) (*marks)[idx] = 1;
                            }
                        }
                    }
                }
            }
        }
        for (auto* it : cn_.interpolators) it->swap_slices();
    }
    cn_.interpolating = false;
}

}  // namespace pn::vg
