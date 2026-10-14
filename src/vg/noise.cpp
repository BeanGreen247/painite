#include <algorithm>
#include "noise.hpp"
#include <cmath>
#include <stdexcept>

namespace pn::vg {

const int kGradient[16][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
                              {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}, {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1}};

static inline double grad_dot(int hash, double x, double y, double z) {
    const int* g = kGradient[hash & 15];
    return g[0] * x + g[1] * y + g[2] * z;
}

// ---- ImprovedNoise -------------------------------------------------------------------------------------------

ImprovedNoise::ImprovedNoise(Rng& random) {
    xo = random.next_double() * 256.0;
    yo = random.next_double() * 256.0;
    zo = random.next_double() * 256.0;
    for (int i = 0; i < 256; i++) perm_[i] = uint8_t(i);
    for (int i = 0; i < 256; i++) {
        int offset = random.next_int(256 - i);
        uint8_t tmp = perm_[i];
        perm_[i] = perm_[i + offset];
        perm_[i + offset] = tmp;
    }
}

double ImprovedNoise::noise(double _x, double _y, double _z, double y_scale, double y_fudge) const {
    double x = _x + xo, y = _y + yo, z = _z + zo;
    int xf = mth_floor(x), yf = mth_floor(y), zf = mth_floor(z);
    double xr = x - xf, yr = y - yf, zr = z - zf;
    double yr_fudge;
    if (y_scale != 0.0) {
        double fudge_limit = (y_fudge >= 0.0 && y_fudge < yr) ? y_fudge : yr;
        yr_fudge = mth_floor(fudge_limit / y_scale + double(1.0E-7f)) * y_scale;
    } else yr_fudge = 0.0;
    return sample_and_lerp(xf, yf, zf, xr, yr - yr_fudge, zr, yr);
}

double ImprovedNoise::sample_and_lerp(int x, int y, int z, double xr, double yr, double zr, double yr_original) const {
    int x0 = p(x), x1 = p(x + 1);
    int xy00 = p(x0 + y), xy01 = p(x0 + y + 1), xy10 = p(x1 + y), xy11 = p(x1 + y + 1);
    double d000 = grad_dot(p(xy00 + z), xr, yr, zr);
    double d100 = grad_dot(p(xy10 + z), xr - 1.0, yr, zr);
    double d010 = grad_dot(p(xy01 + z), xr, yr - 1.0, zr);
    double d110 = grad_dot(p(xy11 + z), xr - 1.0, yr - 1.0, zr);
    double d001 = grad_dot(p(xy00 + z + 1), xr, yr, zr - 1.0);
    double d101 = grad_dot(p(xy10 + z + 1), xr - 1.0, yr, zr - 1.0);
    double d011 = grad_dot(p(xy01 + z + 1), xr, yr - 1.0, zr - 1.0);
    double d111 = grad_dot(p(xy11 + z + 1), xr - 1.0, yr - 1.0, zr - 1.0);
    double xa = smoothstep(xr), ya = smoothstep(yr_original), za = smoothstep(zr);
    return lerp3(xa, ya, za, d000, d100, d010, d110, d001, d101, d011, d111);
}

// ---- PerlinNoise ---------------------------------------------------------------------------------------------

double PerlinNoise::wrap(double x) { return x - double(mth_lfloor(x / 3.3554432E7 + 0.5)) * 3.3554432E7; }

void PerlinNoise::init_derived() {
    int octaves = int(amplitudes_.size());
    int zero_index = -first_octave_;
    lowest_freq_input_factor_ = std::pow(2.0, -zero_index);
    lowest_freq_value_factor_ = std::pow(2.0, octaves - 1) / (std::pow(2.0, octaves) - 1.0);
    max_value_ = edge_value(2.0);
}

std::unique_ptr<PerlinNoise> PerlinNoise::create(Rng& random, int first_octave, const std::vector<double>& amplitudes) {
    std::unique_ptr<PerlinNoise> n(new PerlinNoise());
    n->first_octave_ = first_octave;
    n->amplitudes_ = amplitudes;
    size_t octaves = amplitudes.size();
    n->levels_.resize(octaves);
    auto positional = random.fork_positional();
    for (size_t i = 0; i < octaves; i++)
        if (amplitudes[i] != 0.0) {
            auto r = positional->from_hash_of("octave_" + std::to_string(first_octave + int(i)));
            n->levels_[i] = std::make_unique<ImprovedNoise>(*r);
        }
    n->init_derived();
    return n;
}

std::unique_ptr<PerlinNoise> PerlinNoise::create_legacy(Rng& random, int first_octave, const std::vector<double>& amplitudes) {
    std::unique_ptr<PerlinNoise> n(new PerlinNoise());
    n->first_octave_ = first_octave;
    n->amplitudes_ = amplitudes;
    int octaves = int(amplitudes.size());
    int zero_index = -first_octave;
    n->levels_.resize(size_t(octaves));
    auto zero = std::make_unique<ImprovedNoise>(random);
    if (zero_index >= 0 && zero_index < octaves && amplitudes[size_t(zero_index)] != 0.0) n->levels_[size_t(zero_index)] = std::move(zero);
    for (int i = zero_index - 1; i >= 0; i--) {
        if (i < octaves) {
            if (amplitudes[size_t(i)] != 0.0) n->levels_[size_t(i)] = std::make_unique<ImprovedNoise>(random);
            else random.consume_count(262);
        } else random.consume_count(262);
    }
    n->init_derived();
    return n;
}

std::unique_ptr<PerlinNoise> PerlinNoise::create_legacy_range(Rng& random, int lo, int hi) {
    int low_freq = -lo, high_freq = hi;
    int octaves = low_freq + high_freq + 1;
    std::vector<double> amps(size_t(octaves), 0.0);
    for (int o = lo; o <= hi; o++) amps[size_t(o + low_freq)] = 1.0;
    return create_legacy(random, -low_freq, amps);
}

double PerlinNoise::edge_value(double v) const {
    double value = 0.0, vf = lowest_freq_value_factor_;
    for (size_t i = 0; i < levels_.size(); i++) {
        if (levels_[i]) value += amplitudes_[i] * v * vf;
        vf /= 2.0;
    }
    return value;
}

double PerlinNoise::get_value(double x, double y, double z, double y_scale, double y_fudge) const {
    double value = 0.0, factor = lowest_freq_input_factor_, vf = lowest_freq_value_factor_;
    for (size_t i = 0; i < levels_.size(); i++) {
        if (levels_[i]) {
            double nv = levels_[i]->noise(wrap(x * factor), wrap(y * factor), wrap(z * factor), y_scale * factor, y_fudge * factor);
            value += amplitudes_[i] * nv * vf;
        }
        factor *= 2.0;
        vf /= 2.0;
    }
    return value;
}

// ---- NormalNoise ---------------------------------------------------------------------------------------------

NormalNoise::NormalNoise(Rng& random, const NoiseParameters& p, bool new_init) {
    if (new_init) {
        first_ = PerlinNoise::create(random, p.first_octave, p.amplitudes);
        second_ = PerlinNoise::create(random, p.first_octave, p.amplitudes);
    } else {
        first_ = PerlinNoise::create_legacy(random, p.first_octave, p.amplitudes);
        second_ = PerlinNoise::create_legacy(random, p.first_octave, p.amplitudes);
    }
    int min_o = 0x7fffffff, max_o = -0x7fffffff - 1;
    for (size_t i = 0; i < p.amplitudes.size(); i++)
        if (p.amplitudes[i] != 0.0) { min_o = std::min(min_o, int(i)); max_o = std::max(max_o, int(i)); }
    double dev = 0.1 * (1.0 + 1.0 / double(max_o - min_o + 1));
    value_factor_ = 0.16666666666666666 / dev;
    max_value_ = (first_->max_value() + second_->max_value()) * value_factor_;
}

double NormalNoise::get_value(double x, double y, double z) const {
    double x2 = x * 1.0181268882175227, y2 = y * 1.0181268882175227, z2 = z * 1.0181268882175227;
    return (first_->get_value(x, y, z) + second_->get_value(x2, y2, z2)) * value_factor_;
}

// ---- BlendedNoise --------------------------------------------------------------------------------------------

BlendedNoise::BlendedNoise(Rng& random, double xz_scale, double y_scale, double xz_factor, double y_factor, double smear) : xz_factor_(xz_factor), y_factor_(y_factor), smear_(smear) {
    min_limit_ = PerlinNoise::create_legacy_range(random, -15, 0);
    max_limit_ = PerlinNoise::create_legacy_range(random, -15, 0);
    main_ = PerlinNoise::create_legacy_range(random, -7, 0);
    xz_multiplier_ = 684.412 * xz_scale;
    y_multiplier_ = 684.412 * y_scale;
    max_value_ = min_limit_->max_broken_value(y_multiplier_);
}

double BlendedNoise::compute(double bx, double by, double bz) const {
    double limit_x = bx * xz_multiplier_, limit_y = by * y_multiplier_, limit_z = bz * xz_multiplier_;
    double main_x = limit_x / xz_factor_, main_y = limit_y / y_factor_, main_z = limit_z / xz_factor_;
    double limit_smear = y_multiplier_ * smear_;
    double main_smear = limit_smear / y_factor_;
    double blend_min = 0.0, blend_max = 0.0, main_value = 0.0;
    double pow = 1.0;
    for (int i = 0; i < 8; i++) {
        const ImprovedNoise* n = main_->octave_noise(i);
        if (n) main_value += n->noise(PerlinNoise::wrap(main_x * pow), PerlinNoise::wrap(main_y * pow), PerlinNoise::wrap(main_z * pow), main_smear * pow, main_y * pow) / pow;
        pow /= 2.0;
    }
    double factor = (main_value / 10.0 + 1.0) / 2.0;
    bool is_max = factor >= 1.0, is_min = factor <= 0.0;
    pow = 1.0;
    for (int i = 0; i < 16; i++) {
        double wx = PerlinNoise::wrap(limit_x * pow), wy = PerlinNoise::wrap(limit_y * pow), wz = PerlinNoise::wrap(limit_z * pow);
        double ys = limit_smear * pow;
        if (!is_max) { const ImprovedNoise* n = min_limit_->octave_noise(i); if (n) blend_min += n->noise(wx, wy, wz, ys, limit_y * pow) / pow; }
        if (!is_min) { const ImprovedNoise* n = max_limit_->octave_noise(i); if (n) blend_max += n->noise(wx, wy, wz, ys, limit_y * pow) / pow; }
        pow /= 2.0;
    }
    return clamped_lerp(factor, blend_min / 512.0, blend_max / 512.0) / 128.0;
}

// ---- SimplexNoise --------------------------------------------------------------------------------------------

SimplexNoise::SimplexNoise(Rng& random) {
    xo = random.next_double() * 256.0;
    yo = random.next_double() * 256.0;
    zo = random.next_double() * 256.0;
    for (int i = 0; i < 256; i++) p_[i] = i;
    for (int i = 0; i < 256; i++) {
        int offset = random.next_int(256 - i);
        int tmp = p_[i];
        p_[i] = p_[offset + i];
        p_[offset + i] = tmp;
    }
    for (int i = 256; i < 512; i++) p_[i] = p_[i - 256];   // the Java array is 512 long; p(x) masks with 0xff so the tail is never read
}

double SimplexNoise::corner(int index, double x, double y, double z, double base) const {
    double t0 = base - x * x - y * y - z * z;
    if (t0 < 0.0) return 0.0;
    t0 *= t0;
    const int* g = kGradient[index];
    return t0 * t0 * (g[0] * x + g[1] * y + g[2] * z);
}

double SimplexNoise::get_value(double xin, double yin) const {
    const double SQRT_3 = std::sqrt(3.0), F2 = 0.5 * (SQRT_3 - 1.0), G2 = (3.0 - SQRT_3) / 6.0;
    double s = (xin + yin) * F2;
    int i = mth_floor(xin + s), j = mth_floor(yin + s);
    double t = (i + j) * G2;
    double X0 = i - t, Y0 = j - t;
    double x0 = xin - X0, y0 = yin - Y0;
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; } else { i1 = 0; j1 = 1; }
    double x1 = x0 - i1 + G2, y1 = y0 - j1 + G2, x2 = x0 - 1.0 + 2.0 * G2, y2 = y0 - 1.0 + 2.0 * G2;
    int ii = i & 0xff, jj = j & 0xff;
    int gi0 = p(ii + p(jj)) % 12, gi1 = p(ii + i1 + p(jj + j1)) % 12, gi2 = p(ii + 1 + p(jj + 1)) % 12;
    return 70.0 * (corner(gi0, x0, y0, 0.0, 0.5) + corner(gi1, x1, y1, 0.0, 0.5) + corner(gi2, x2, y2, 0.0, 0.5));
}

double SimplexNoise::get_value(double xin, double yin, double zin) const {
    const double F3 = 0.3333333333333333, G3 = 0.16666666666666666;
    double s = (xin + yin + zin) * F3;
    int i = mth_floor(xin + s), j = mth_floor(yin + s), k = mth_floor(zin + s);
    double t = (i + j + k) * G3;
    double x0 = xin - (i - t), y0 = yin - (j - t), z0 = zin - (k - t);
    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
    else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
    else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    double x1 = x0 - i1 + G3, y1 = y0 - j1 + G3, z1 = z0 - k1 + G3;
    double x2 = x0 - i2 + 0.3333333333333333, y2 = y0 - j2 + 0.3333333333333333, z2 = z0 - k2 + 0.3333333333333333;
    double x3 = x0 - 1.0 + 0.5, y3 = y0 - 1.0 + 0.5, z3 = z0 - 1.0 + 0.5;
    int ii = i & 0xff, jj = j & 0xff, kk = k & 0xff;
    int gi0 = p(ii + p(jj + p(kk))) % 12, gi1 = p(ii + i1 + p(jj + j1 + p(kk + k1))) % 12, gi2 = p(ii + i2 + p(jj + j2 + p(kk + k2))) % 12, gi3 = p(ii + 1 + p(jj + 1 + p(kk + 1))) % 12;
    return 32.0 * (corner(gi0, x0, y0, z0, 0.6) + corner(gi1, x1, y1, z1, 0.6) + corner(gi2, x2, y2, z2, 0.6) + corner(gi3, x3, y3, z3, 0.6));
}

PerlinSimplexNoise::PerlinSimplexNoise(Rng& random, const std::vector<int>& octave_set) {
    int lo = *std::min_element(octave_set.begin(), octave_set.end()), hi = *std::max_element(octave_set.begin(), octave_set.end());
    auto contains = [&](int v) { return std::find(octave_set.begin(), octave_set.end(), v) != octave_set.end(); };
    int low_freq = -lo, high_freq = hi;
    int octaves = low_freq + high_freq + 1;
    levels_.resize(size_t(octaves));
    auto zero_octave = std::make_unique<SimplexNoise>(random);
    int zero_index = high_freq;
    SimplexNoise* zero = zero_octave.get();
    if (high_freq >= 0 && high_freq < octaves && contains(0)) levels_[size_t(high_freq)] = std::move(zero_octave);
    for (int i = high_freq + 1; i < octaves; i++) {
        if (i >= 0 && contains(zero_index - i)) levels_[size_t(i)] = std::make_unique<SimplexNoise>(random);
        else random.consume_count(262);
    }
    if (high_freq > 0) {
        double v = zero->get_value(zero->xo, zero->yo, zero->zo) * double(9.223372E18f);
        int64_t seed = v >= 9.223372036854775807E18 ? INT64_MAX : v <= -9.223372036854775808E18 ? INT64_MIN : int64_t(v);
        LegacyRandom high(seed);
        for (int i = zero_index - 1; i >= 0; i--) {
            if (i < octaves && contains(zero_index - i)) levels_[size_t(i)] = std::make_unique<SimplexNoise>(high);
            else high.consume_count(262);
        }
    }
    highest_freq_input_factor_ = std::pow(2.0, high_freq);
    highest_freq_value_factor_ = 1.0 / (std::pow(2.0, octaves) - 1.0);
}

double PerlinSimplexNoise::get_value(double x, double y, bool use_noise_start) const {
    double value = 0.0, factor = highest_freq_input_factor_, value_factor = highest_freq_value_factor_;
    for (auto& n : levels_) {
        if (n) value += n->get_value(x * factor + (use_noise_start ? n->xo : 0.0), y * factor + (use_noise_start ? n->yo : 0.0)) * value_factor;
        factor /= 2.0;
        value_factor *= 2.0;
    }
    return value;
}

}  // namespace pn::vg
