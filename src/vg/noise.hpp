// Mojang's noise classes: improved Perlin noise, octave sums, the two-sampler "normal" noise, the old blended terrain noise and simplex noise.
#pragma once
#include "random.hpp"
#include <memory>
#include <vector>

namespace pn::vg {

class ImprovedNoise {
public:
    explicit ImprovedNoise(Rng& random);
    double noise(double x, double y, double z) const { return noise(x, y, z, 0.0, 0.0); }
    double noise(double x, double y, double z, double y_scale, double y_fudge) const;
    double xo, yo, zo;
private:
    double sample_and_lerp(int x, int y, int z, double xr, double yr, double zr, double yr_original) const;
    int p(int x) const { return perm_[x & 0xff]; }
    uint8_t perm_[256];
};

struct NoiseParameters {
    int first_octave = 0;
    std::vector<double> amplitudes;
};

class PerlinNoise {
public:
    static std::unique_ptr<PerlinNoise> create(Rng& random, int first_octave, const std::vector<double>& amplitudes);        // new (hash seeded) initialisation
    static std::unique_ptr<PerlinNoise> create_legacy(Rng& random, int first_octave, const std::vector<double>& amplitudes);   // sequential seeding (blended noise, legacy nether biomes)
    static std::unique_ptr<PerlinNoise> create_legacy_range(Rng& random, int lo, int hi);                                       // octave set lo..hi, all amplitude 1
    double get_value(double x, double y, double z) const { return get_value(x, y, z, 0.0, 0.0); }
    double get_value(double x, double y, double z, double y_scale, double y_fudge) const;
    double max_value() const { return max_value_; }
    double max_broken_value(double y_scale) const { return edge_value(y_scale + 2.0); }
    const ImprovedNoise* octave_noise(int i) const { return levels_[levels_.size() - 1 - size_t(i)].get(); }
    static double wrap(double x);
private:
    PerlinNoise() = default;
    void init_derived();
    double edge_value(double v) const;
    std::vector<std::unique_ptr<ImprovedNoise>> levels_;
    int first_octave_ = 0;
    std::vector<double> amplitudes_;
    double lowest_freq_value_factor_ = 0, lowest_freq_input_factor_ = 0, max_value_ = 0;
};

class NormalNoise {
public:
    NormalNoise(Rng& random, const NoiseParameters& p, bool new_init = true);
    double get_value(double x, double y, double z) const;
    double max_value() const { return max_value_; }
private:
    std::unique_ptr<PerlinNoise> first_, second_;
    double value_factor_ = 0, max_value_ = 0;
};

class BlendedNoise {
public:
    BlendedNoise(Rng& random, double xz_scale, double y_scale, double xz_factor, double y_factor, double smear_scale_multiplier);
    double compute(double block_x, double block_y, double block_z) const;
    double max_value() const { return max_value_; }
private:
    std::unique_ptr<PerlinNoise> min_limit_, max_limit_, main_;
    double xz_multiplier_, y_multiplier_, xz_factor_, y_factor_, smear_, max_value_;
};

class SimplexNoise {
public:
    explicit SimplexNoise(Rng& random);
    double get_value(double x, double y) const;
    double get_value(double x, double y, double z) const;
    double xo, yo, zo;
private:
    int p(int x) const { return p_[x & 0xff]; }
    double corner(int index, double x, double y, double z, double base) const;
    int p_[512];
};

// octave sum of simplex noise (biome temperature, frozen biome patches); octaves is the set of octave numbers like [-2, -1, 0]
class PerlinSimplexNoise {
public:
    PerlinSimplexNoise(Rng& random, const std::vector<int>& octave_set);
    double get_value(double x, double y, bool use_noise_start) const;
private:
    std::vector<std::unique_ptr<SimplexNoise>> levels_;
    double highest_freq_value_factor_ = 0, highest_freq_input_factor_ = 0;
};

extern const int kGradient[16][3];
inline int mth_floor(double v) { return int(__builtin_floor(v)); }
inline int64_t mth_lfloor(double v) { return int64_t(__builtin_floor(v)); }
inline double lerp(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
inline double lerp2(double a1, double a2, double x00, double x10, double x01, double x11) { return lerp(a2, lerp(a1, x00, x10), lerp(a1, x01, x11)); }
inline double lerp3(double a1, double a2, double a3, double x000, double x100, double x010, double x110, double x001, double x101, double x011, double x111) {
    return lerp(a3, lerp2(a1, a2, x000, x100, x010, x110), lerp2(a1, a2, x001, x101, x011, x111));
}
inline double smoothstep(double x) { return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
inline double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
inline double clamped_lerp(double f, double lo, double hi) { return f < 0.0 ? lo : f > 1.0 ? hi : lerp(f, lo, hi); }
inline double inverse_lerp(double v, double lo, double hi) { return (v - lo) / (hi - lo); }
inline double clamped_map(double v, double fmin, double fmax, double tmin, double tmax) { return clamped_lerp(inverse_lerp(v, fmin, fmax), tmin, tmax); }

}  // namespace pn::vg
