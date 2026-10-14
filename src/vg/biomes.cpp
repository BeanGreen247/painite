#include "biomes.hpp"
#include <map>

namespace pn::vg {

namespace {

using P = Parameter;
P span(float a, float b) { return {quantize_coord(a), quantize_coord(b)}; }
P span(const P& a, const P& b) { return {a.min, b.max}; }
P point(float v) { return span(v, v); }
using Name = const char*;

struct Builder {
    std::vector<ParameterPoint> points;
    std::vector<std::string> names;

    const P full = span(-1.0f, 1.0f);
    const P temperatures[5] = {span(-1.0f, -0.45f), span(-0.45f, -0.15f), span(-0.15f, 0.2f), span(0.2f, 0.55f), span(0.55f, 1.0f)};
    const P humidities[5] = {span(-1.0f, -0.35f), span(-0.35f, -0.1f), span(-0.1f, 0.1f), span(0.1f, 0.3f), span(0.3f, 1.0f)};
    const P erosions[7] = {span(-1.0f, -0.78f), span(-0.78f, -0.375f), span(-0.375f, -0.2225f), span(-0.2225f, 0.05f), span(0.05f, 0.45f), span(0.45f, 0.55f), span(0.55f, 1.0f)};
    const P frozen = temperatures[0];
    const P unfrozen = span(temperatures[1], temperatures[4]);
    const P mushroom = span(-1.2f, -1.05f), deep_ocean = span(-1.05f, -0.455f), ocean = span(-0.455f, -0.19f), coast = span(-0.19f, -0.11f), inland = span(-0.11f, 0.55f),
            near_inland = span(-0.11f, 0.03f), mid_inland = span(0.03f, 0.3f), far_inland = span(0.3f, 1.0f);
    const Name oceans[2][5] = {{"deep_frozen_ocean", "deep_cold_ocean", "deep_ocean", "deep_lukewarm_ocean", "warm_ocean"}, {"frozen_ocean", "cold_ocean", "ocean", "lukewarm_ocean", "warm_ocean"}};
    const Name middle[5][5] = {{"snowy_plains", "snowy_plains", "snowy_plains", "snowy_taiga", "taiga"},
                               {"plains", "plains", "forest", "taiga", "old_growth_spruce_taiga"},
                               {"flower_forest", "plains", "forest", "birch_forest", "dark_forest"},
                               {"savanna", "savanna", "forest", "jungle", "jungle"},
                               {"desert", "desert", "desert", "desert", "desert"}};
    const Name middle_variant[5][5] = {{"ice_spikes", nullptr, "snowy_taiga", nullptr, nullptr},
                                       {nullptr, nullptr, nullptr, nullptr, "old_growth_pine_taiga"},
                                       {"sunflower_plains", nullptr, nullptr, "old_growth_birch_forest", nullptr},
                                       {nullptr, nullptr, "plains", "sparse_jungle", "bamboo_jungle"},
                                       {nullptr, nullptr, nullptr, nullptr, nullptr}};
    const Name plateau[5][5] = {{"snowy_plains", "snowy_plains", "snowy_plains", "snowy_taiga", "snowy_taiga"},
                                {"meadow", "meadow", "forest", "taiga", "old_growth_spruce_taiga"},
                                {"meadow", "meadow", "meadow", "meadow", "pale_garden"},
                                {"savanna_plateau", "savanna_plateau", "forest", "forest", "jungle"},
                                {"badlands", "badlands", "badlands", "wooded_badlands", "wooded_badlands"}};
    const Name plateau_variant[5][5] = {{"ice_spikes", nullptr, nullptr, nullptr, nullptr},
                                        {"cherry_grove", nullptr, "meadow", "meadow", "old_growth_pine_taiga"},
                                        {"cherry_grove", "cherry_grove", "forest", "birch_forest", nullptr},
                                        {nullptr, nullptr, nullptr, nullptr, nullptr},
                                        {"eroded_badlands", "eroded_badlands", nullptr, nullptr, nullptr}};
    const Name shattered[5][5] = {{"windswept_gravelly_hills", "windswept_gravelly_hills", "windswept_hills", "windswept_forest", "windswept_forest"},
                                  {"windswept_gravelly_hills", "windswept_gravelly_hills", "windswept_hills", "windswept_forest", "windswept_forest"},
                                  {"windswept_hills", "windswept_hills", "windswept_hills", "windswept_forest", "windswept_forest"},
                                  {nullptr, nullptr, nullptr, nullptr, nullptr},
                                  {nullptr, nullptr, nullptr, nullptr, nullptr}};

    void add(const P& t, const P& h, const P& c, const P& e, const P& d, const P& w, float offset, Name biome) {
        ParameterPoint pt;
        pt.temperature = t; pt.humidity = h; pt.continentalness = c; pt.erosion = e; pt.depth = d; pt.weirdness = w;
        pt.offset = quantize_coord(offset);
        points.push_back(pt);
        names.push_back(biome);
    }
    void surface(const P& t, const P& h, const P& c, const P& e, const P& w, float offset, Name biome) {
        add(t, h, c, e, point(0.0f), w, offset, biome);
        add(t, h, c, e, point(1.0f), w, offset, biome);
    }
    void underground(const P& t, const P& h, const P& c, const P& e, const P& w, float offset, Name biome) { add(t, h, c, e, span(0.2f, 0.9f), w, offset, biome); }
    void bottom(const P& t, const P& h, const P& c, const P& e, const P& w, float offset, Name biome) { add(t, h, c, e, point(1.1f), w, offset, biome); }

    static bool neg(const P& w) { return w.max < 0; }

    Name pick_middle(int t, int h, const P& w) const {
        if (neg(w)) return middle[t][h];
        return middle_variant[t][h] ? middle_variant[t][h] : middle[t][h];
    }
    Name pick_badlands(int h, const P& w) const {
        if (h < 2) return neg(w) ? "badlands" : "eroded_badlands";
        return h < 3 ? "badlands" : "wooded_badlands";
    }
    Name pick_middle_or_badlands(int t, int h, const P& w) const { return t == 4 ? pick_badlands(h, w) : pick_middle(t, h, w); }
    Name pick_plateau(int t, int h, const P& w) const {
        if (w.max >= 0 && plateau_variant[t][h]) return plateau_variant[t][h];
        return plateau[t][h];
    }
    Name pick_slope(int t, int h, const P& w) const {
        if (t >= 3) return pick_plateau(t, h, w);
        return h <= 1 ? "snowy_slopes" : "grove";
    }
    Name pick_middle_or_badlands_or_slope(int t, int h, const P& w) const { return t == 0 ? pick_slope(t, h, w) : pick_middle_or_badlands(t, h, w); }
    Name pick_peak(int t, int h, const P& w) const {
        if (t <= 2) return neg(w) ? "jagged_peaks" : "frozen_peaks";
        return t == 3 ? "stony_peaks" : pick_badlands(h, w);
    }
    Name pick_shattered(int t, int h, const P& w) const { return shattered[t][h] ? shattered[t][h] : pick_middle(t, h, w); }
    Name maybe_windswept_savanna(int t, int h, const P& w, Name under) const { return t > 1 && h < 4 && w.max >= 0 ? "windswept_savanna" : under; }
    static Name pick_beach(int t) { return t == 0 ? "snowy_beach" : (t == 4 ? "desert" : "beach"); }
    Name pick_shattered_coast(int t, int h, const P& w) const { return maybe_windswept_savanna(t, h, w, w.max >= 0 ? pick_middle(t, h, w) : pick_beach(t)); }

    void off_coast() {
        surface(full, full, mushroom, full, full, 0.0f, "mushroom_fields");
        for (int i = 0; i < 5; i++) {
            surface(temperatures[i], full, deep_ocean, full, full, 0.0f, oceans[0][i]);
            surface(temperatures[i], full, ocean, full, full, 0.0f, oceans[1][i]);
        }
    }

    void inland_all() {
        mid_slice(span(-1.0f, -0.93333334f));
        high_slice(span(-0.93333334f, -0.7666667f));
        peaks(span(-0.7666667f, -0.56666666f));
        high_slice(span(-0.56666666f, -0.4f));
        mid_slice(span(-0.4f, -0.26666668f));
        low_slice(span(-0.26666668f, -0.05f));
        valleys(span(-0.05f, 0.05f));
        low_slice(span(0.05f, 0.26666668f));
        mid_slice(span(0.26666668f, 0.4f));
        high_slice(span(0.4f, 0.56666666f));
        peaks(span(0.56666666f, 0.7666667f));
        high_slice(span(0.7666667f, 0.93333334f));
        mid_slice(span(0.93333334f, 1.0f));
    }

    void peaks(const P& w) {
        for (int ti = 0; ti < 5; ti++) {
            const P& t = temperatures[ti];
            for (int hi = 0; hi < 5; hi++) {
                const P& h = humidities[hi];
                Name middle_b = pick_middle(ti, hi, w), middle_hot = pick_middle_or_badlands(ti, hi, w), middle_hot_slope = pick_middle_or_badlands_or_slope(ti, hi, w);
                Name plateau_b = pick_plateau(ti, hi, w), shattered_b = pick_shattered(ti, hi, w), shattered_sav = maybe_windswept_savanna(ti, hi, w, shattered_b), peak_b = pick_peak(ti, hi, w);
                surface(t, h, span(coast, far_inland), erosions[0], w, 0.0f, peak_b);
                surface(t, h, span(coast, near_inland), erosions[1], w, 0.0f, middle_hot_slope);
                surface(t, h, span(mid_inland, far_inland), erosions[1], w, 0.0f, peak_b);
                surface(t, h, span(coast, near_inland), span(erosions[2], erosions[3]), w, 0.0f, middle_b);
                surface(t, h, span(mid_inland, far_inland), erosions[2], w, 0.0f, plateau_b);
                surface(t, h, mid_inland, erosions[3], w, 0.0f, middle_hot);
                surface(t, h, far_inland, erosions[3], w, 0.0f, plateau_b);
                surface(t, h, span(coast, far_inland), erosions[4], w, 0.0f, middle_b);
                surface(t, h, span(coast, near_inland), erosions[5], w, 0.0f, shattered_sav);
                surface(t, h, span(mid_inland, far_inland), erosions[5], w, 0.0f, shattered_b);
                surface(t, h, span(coast, far_inland), erosions[6], w, 0.0f, middle_b);
            }
        }
    }

    void high_slice(const P& w) {
        for (int ti = 0; ti < 5; ti++) {
            const P& t = temperatures[ti];
            for (int hi = 0; hi < 5; hi++) {
                const P& h = humidities[hi];
                Name middle_b = pick_middle(ti, hi, w), middle_hot = pick_middle_or_badlands(ti, hi, w), middle_hot_slope = pick_middle_or_badlands_or_slope(ti, hi, w);
                Name plateau_b = pick_plateau(ti, hi, w), shattered_b = pick_shattered(ti, hi, w), middle_sav = maybe_windswept_savanna(ti, hi, w, middle_b);
                Name slope_b = pick_slope(ti, hi, w), peak_b = pick_peak(ti, hi, w);
                surface(t, h, coast, span(erosions[0], erosions[1]), w, 0.0f, middle_b);
                surface(t, h, near_inland, erosions[0], w, 0.0f, slope_b);
                surface(t, h, span(mid_inland, far_inland), erosions[0], w, 0.0f, peak_b);
                surface(t, h, near_inland, erosions[1], w, 0.0f, middle_hot_slope);
                surface(t, h, span(mid_inland, far_inland), erosions[1], w, 0.0f, slope_b);
                surface(t, h, span(coast, near_inland), span(erosions[2], erosions[3]), w, 0.0f, middle_b);
                surface(t, h, span(mid_inland, far_inland), erosions[2], w, 0.0f, plateau_b);
                surface(t, h, mid_inland, erosions[3], w, 0.0f, middle_hot);
                surface(t, h, far_inland, erosions[3], w, 0.0f, plateau_b);
                surface(t, h, span(coast, far_inland), erosions[4], w, 0.0f, middle_b);
                surface(t, h, span(coast, near_inland), erosions[5], w, 0.0f, middle_sav);
                surface(t, h, span(mid_inland, far_inland), erosions[5], w, 0.0f, shattered_b);
                surface(t, h, span(coast, far_inland), erosions[6], w, 0.0f, middle_b);
            }
        }
    }

    void swamps(const P& w) {
        surface(full, full, coast, span(erosions[0], erosions[2]), w, 0.0f, "stony_shore");
        surface(span(temperatures[1], temperatures[2]), full, span(near_inland, far_inland), erosions[6], w, 0.0f, "swamp");
        surface(span(temperatures[3], temperatures[4]), full, span(near_inland, far_inland), erosions[6], w, 0.0f, "mangrove_swamp");
    }

    void mid_slice(const P& w) {
        swamps(w);
        for (int ti = 0; ti < 5; ti++) {
            const P& t = temperatures[ti];
            for (int hi = 0; hi < 5; hi++) {
                const P& h = humidities[hi];
                Name middle_b = pick_middle(ti, hi, w), middle_hot = pick_middle_or_badlands(ti, hi, w), middle_hot_slope = pick_middle_or_badlands_or_slope(ti, hi, w);
                Name shattered_b = pick_shattered(ti, hi, w), plateau_b = pick_plateau(ti, hi, w), beach = pick_beach(ti), middle_sav = maybe_windswept_savanna(ti, hi, w, middle_b);
                Name shattered_coast = pick_shattered_coast(ti, hi, w), slope_b = pick_slope(ti, hi, w);
                surface(t, h, span(near_inland, far_inland), erosions[0], w, 0.0f, slope_b);
                surface(t, h, span(near_inland, mid_inland), erosions[1], w, 0.0f, middle_hot_slope);
                surface(t, h, far_inland, erosions[1], w, 0.0f, ti == 0 ? slope_b : plateau_b);
                surface(t, h, near_inland, erosions[2], w, 0.0f, middle_b);
                surface(t, h, mid_inland, erosions[2], w, 0.0f, middle_hot);
                surface(t, h, far_inland, erosions[2], w, 0.0f, plateau_b);
                surface(t, h, span(coast, near_inland), erosions[3], w, 0.0f, middle_b);
                surface(t, h, span(mid_inland, far_inland), erosions[3], w, 0.0f, middle_hot);
                if (neg(w)) {
                    surface(t, h, coast, erosions[4], w, 0.0f, beach);
                    surface(t, h, span(near_inland, far_inland), erosions[4], w, 0.0f, middle_b);
                } else {
                    surface(t, h, span(coast, far_inland), erosions[4], w, 0.0f, middle_b);
                }
                surface(t, h, coast, erosions[5], w, 0.0f, shattered_coast);
                surface(t, h, near_inland, erosions[5], w, 0.0f, middle_sav);
                surface(t, h, span(mid_inland, far_inland), erosions[5], w, 0.0f, shattered_b);
                if (neg(w)) surface(t, h, coast, erosions[6], w, 0.0f, beach);
                else surface(t, h, coast, erosions[6], w, 0.0f, middle_b);
                if (ti == 0) surface(t, h, span(near_inland, far_inland), erosions[6], w, 0.0f, middle_b);
            }
        }
    }

    void low_slice(const P& w) {
        swamps(w);
        for (int ti = 0; ti < 5; ti++) {
            const P& t = temperatures[ti];
            for (int hi = 0; hi < 5; hi++) {
                const P& h = humidities[hi];
                Name middle_b = pick_middle(ti, hi, w), middle_hot = pick_middle_or_badlands(ti, hi, w), middle_hot_slope = pick_middle_or_badlands_or_slope(ti, hi, w);
                Name beach = pick_beach(ti), middle_sav = maybe_windswept_savanna(ti, hi, w, middle_b), shattered_coast = pick_shattered_coast(ti, hi, w);
                surface(t, h, near_inland, span(erosions[0], erosions[1]), w, 0.0f, middle_hot);
                surface(t, h, span(mid_inland, far_inland), span(erosions[0], erosions[1]), w, 0.0f, middle_hot_slope);
                surface(t, h, near_inland, span(erosions[2], erosions[3]), w, 0.0f, middle_b);
                surface(t, h, span(mid_inland, far_inland), span(erosions[2], erosions[3]), w, 0.0f, middle_hot);
                surface(t, h, coast, span(erosions[3], erosions[4]), w, 0.0f, beach);
                surface(t, h, span(near_inland, far_inland), erosions[4], w, 0.0f, middle_b);
                surface(t, h, coast, erosions[5], w, 0.0f, shattered_coast);
                surface(t, h, near_inland, erosions[5], w, 0.0f, middle_sav);
                surface(t, h, span(mid_inland, far_inland), erosions[5], w, 0.0f, middle_b);
                surface(t, h, coast, erosions[6], w, 0.0f, beach);
                if (ti == 0) surface(t, h, span(near_inland, far_inland), erosions[6], w, 0.0f, middle_b);
            }
        }
    }

    void valleys(const P& w) {
        const bool n = neg(w);
        surface(frozen, full, coast, span(erosions[0], erosions[1]), w, 0.0f, n ? "stony_shore" : "frozen_river");
        surface(unfrozen, full, coast, span(erosions[0], erosions[1]), w, 0.0f, n ? "stony_shore" : "river");
        surface(frozen, full, near_inland, span(erosions[0], erosions[1]), w, 0.0f, "frozen_river");
        surface(unfrozen, full, near_inland, span(erosions[0], erosions[1]), w, 0.0f, "river");
        surface(frozen, full, span(coast, far_inland), span(erosions[2], erosions[5]), w, 0.0f, "frozen_river");
        surface(unfrozen, full, span(coast, far_inland), span(erosions[2], erosions[5]), w, 0.0f, "river");
        surface(frozen, full, coast, erosions[6], w, 0.0f, "frozen_river");
        surface(unfrozen, full, coast, erosions[6], w, 0.0f, "river");
        surface(span(temperatures[1], temperatures[2]), full, span(inland, far_inland), erosions[6], w, 0.0f, "swamp");
        surface(span(temperatures[3], temperatures[4]), full, span(inland, far_inland), erosions[6], w, 0.0f, "mangrove_swamp");
        surface(frozen, full, span(inland, far_inland), erosions[6], w, 0.0f, "frozen_river");
        for (int ti = 0; ti < 5; ti++)
            for (int hi = 0; hi < 5; hi++)
                surface(temperatures[ti], humidities[hi], span(mid_inland, far_inland), span(erosions[0], erosions[1]), w, 0.0f, pick_middle_or_badlands(ti, hi, w));
    }

    void underground_all() {
        underground(full, full, span(0.8f, 1.0f), full, full, 0.0f, "dripstone_caves");
        underground(full, span(0.7f, 1.0f), full, full, full, 0.0f, "lush_caves");
        underground(full, full, span(coast, inland), span(erosions[5], erosions[6]), span(-1.1f, -0.85f), 0.0f, "sulfur_caves");
        bottom(full, full, full, span(erosions[0], erosions[1]), full, 0.0f, "deep_dark");
    }

    ClimateData finish() {
        ClimateData out;
        std::map<std::string, int> index;
        std::vector<int> values;
        for (auto& n : names) {
            auto it = index.find(n);
            if (it == index.end()) { it = index.emplace(n, int(out.biomes.size())).first; out.biomes.push_back(n); }
            values.push_back(it->second);
        }
        out.list = std::make_unique<ParameterList>(points, values);
        return out;
    }
};

}  // namespace

ClimateData overworld_climate() {
    Builder b;
    b.off_coast();
    b.inland_all();
    b.underground_all();
    return b.finish();
}

ClimateData nether_climate() {
    Builder b;
    P z = point(0.0f);
    b.add(z, z, z, z, z, z, 0.0f, "nether_wastes");
    b.add(z, point(-0.5f), z, z, z, z, 0.0f, "soul_sand_valley");
    b.add(point(0.4f), z, z, z, z, z, 0.0f, "crimson_forest");
    b.add(z, point(0.5f), z, z, z, z, 0.375f, "warped_forest");
    b.add(point(-0.5f), z, z, z, z, z, 0.175f, "basalt_deltas");
    return b.finish();
}

}  // namespace pn::vg
