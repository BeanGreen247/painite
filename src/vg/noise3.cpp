#include "noise3.hpp"
#include <algorithm>
#include <cmath>

namespace pn::vg::v3 {

int Volume::index_of_block(int bx, int by, int bz) const {
    int rx = bx - minx, ry = by - miny, rz = bz - minz;
    auto floor_div = [](int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; };
    auto floor_mod = [](int a, int b) { int m = a % b; return (m != 0 && ((m < 0) != (b < 0))) ? m + b : m; };
    if (stepx == 1 && stepy == 1 && stepz == 1) {
        if (rx >= 0 && ry >= 0 && rz >= 0 && rx < sx && ry < sy && rz < sz) return index(rx, ry, rz);
    } else if (rx >= 0 && ry >= 0 && rz >= 0 && rx < sx * stepx && ry < sy * stepy && rz < sz * stepz &&
               floor_mod(rx, stepx) == 0 && floor_mod(ry, stepy) == 0 && floor_mod(rz, stepz) == 0) {
        return index(floor_div(rx, stepx), floor_div(ry, stepy), floor_div(rz, stepz));
    }
    return -1;
}

float mth_lerp(float a, float p0, float p1) { return p0 + a * (p1 - p0); }
float mth_lerp2(float a1, float a2, float x00, float x10, float x01, float x11) { return mth_lerp(a2, mth_lerp(a1, x00, x10), mth_lerp(a1, x01, x11)); }
float mth_lerp3(float a1, float a2, float a3, float x000, float x100, float x010, float x110, float x001, float x101, float x011, float x111) {
    return mth_lerp(a3, mth_lerp2(a1, a2, x000, x100, x010, x110), mth_lerp2(a1, a2, x001, x101, x011, x111));
}
float mth_smoothstep(float x) { return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); }
float mth_clamp(float v, float lo, float hi) { if (v < lo) return lo; return std::min(v, hi); }

static int mth_floor(double v) { return int(std::floor(v)); }

void Noise::add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const {
    int index = 0;
    for (int iz = 0; iz < v.sz; iz++) {
        double z = double(v.block_z(iz)) * xz_scale;
        for (int ix = 0; ix < v.sx; ix++) {
            double x = double(v.block_x(ix)) * xz_scale;
            for (int iy = 0; iy < v.sy; iy++) {
                double y = double(v.block_y(iy)) * y_scale;
                buffer[index] += amplitude * get(x, y, z);
                index++;
            }
        }
    }
}

const GradientNoise::Grad GradientNoise::kGrad[16] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
                                                      {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}, {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1}};

GradientNoise::GradientNoise(Rng& random, double offset_scale) {
    off_x_ = random.next_double() * offset_scale;
    off_y_ = random.next_double() * offset_scale;
    off_z_ = random.next_double() * offset_scale;
    for (int i = 0; i < 256; i++) perms_[i] = uint8_t(i);
    for (int i = 0; i < 256; i++) {
        int offset = random.next_int(256 - i);
        uint8_t tmp = perms_[i];
        perms_[i] = perms_[offset + i];
        perms_[offset + i] = tmp;
    }
}

double GradientNoise::wrap(double x) {
    static const double half_round_off = std::nextafter(16777216.0, 0.0);
    if (x >= -half_round_off && x < half_round_off) return x;
    return x - std::floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}

// ---- PerlinNoise -------------------------------------------------------------------------------------------------

float PerlinNoise::sample_and_lerp(int x, int y, int z, float rx, float ry, float rz, float original_ry) const {
    int x0 = permute(x), x1 = permute(x + 1);
    int xy00 = permute(x0 + y), xy01 = permute(x0 + y + 1), xy10 = permute(x1 + y), xy11 = permute(x1 + y + 1);
    float d000 = grad_dot(permute(xy00 + z), rx, ry, rz);
    float d100 = grad_dot(permute(xy10 + z), rx - 1.0f, ry, rz);
    float d010 = grad_dot(permute(xy01 + z), rx, ry - 1.0f, rz);
    float d110 = grad_dot(permute(xy11 + z), rx - 1.0f, ry - 1.0f, rz);
    float d001 = grad_dot(permute(xy00 + z + 1), rx, ry, rz - 1.0f);
    float d101 = grad_dot(permute(xy10 + z + 1), rx - 1.0f, ry, rz - 1.0f);
    float d011 = grad_dot(permute(xy01 + z + 1), rx, ry - 1.0f, rz - 1.0f);
    float d111 = grad_dot(permute(xy11 + z + 1), rx - 1.0f, ry - 1.0f, rz - 1.0f);
    float xa = mth_smoothstep(rx), ya = mth_smoothstep(original_ry), za = mth_smoothstep(rz);
    return mth_lerp3(xa, ya, za, d000, d100, d010, d110, d001, d101, d011, d111);
}

float PerlinNoise::get(double ax, double ay, double az) const {
    double x = wrap(ax) + off_x_, y = wrap(ay) + off_y_, z = wrap(az) + off_z_;
    int fx = mth_floor(x), fy = mth_floor(y), fz = mth_floor(z);
    float rx = float(x - double(fx)), ry = float(y - double(fy)), rz = float(z - double(fz));
    return sample_and_lerp(fx, fy, fz, rx, ry, rz, ry);
}

void PerlinNoise::add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const {
    float d000xz = 0, d100xz = 0, d010xz = 0, d110xz = 0, d001xz = 0, d101xz = 0, d011xz = 0, d111xz = 0;
    float g000y = 0, g100y = 0, g010y = 0, g110y = 0, g001y = 0, g101y = 0, g011y = 0, g111y = 0;
    int index = 0;
    for (int iz = 0; iz < v.sz; iz++) {
        double z = wrap(double(v.block_z(iz)) * xz_scale) + off_z_;
        int fz = mth_floor(z);
        float rz = float(z - double(fz));
        float az = mth_smoothstep(rz);
        for (int ix = 0; ix < v.sx; ix++) {
            double x = wrap(double(v.block_x(ix)) * xz_scale) + off_x_;
            int fx = mth_floor(x);
            float rx = float(x - double(fx));
            int x0 = permute(fx), x1 = permute(fx + 1);
            float ax = mth_smoothstep(rx);
            int last_floor_y = INT32_MIN;
            for (int iy = 0; iy < v.sy; iy++) {
                double y = wrap(double(v.block_y(iy)) * y_scale) + off_y_;
                int fy = mth_floor(y);
                float ry = float(y - double(fy));
                float ay = mth_smoothstep(ry);
                if (last_floor_y != fy) {
                    int xy00 = permute(x0 + fy), xy01 = permute(x0 + fy + 1), xy10 = permute(x1 + fy), xy11 = permute(x1 + fy + 1);
                    const Grad& g000 = perm_grad(xy00 + fz);
                    d000xz = float(g000.x) * rx + float(g000.z) * rz; g000y = float(g000.y);
                    const Grad& g100 = perm_grad(xy10 + fz);
                    d100xz = float(g100.x) * (rx - 1.0f) + float(g100.z) * rz; g100y = float(g100.y);
                    const Grad& g010 = perm_grad(xy01 + fz);
                    d010xz = float(g010.x) * rx + float(g010.z) * rz; g010y = float(g010.y);
                    const Grad& g110 = perm_grad(xy11 + fz);
                    d110xz = float(g110.x) * (rx - 1.0f) + float(g110.z) * rz; g110y = float(g110.y);
                    const Grad& g001 = perm_grad(xy00 + fz + 1);
                    d001xz = float(g001.x) * rx + float(g001.z) * (rz - 1.0f); g001y = float(g001.y);
                    const Grad& g101 = perm_grad(xy10 + fz + 1);
                    d101xz = float(g101.x) * (rx - 1.0f) + float(g101.z) * (rz - 1.0f); g101y = float(g101.y);
                    const Grad& g011 = perm_grad(xy01 + fz + 1);
                    d011xz = float(g011.x) * rx + float(g011.z) * (rz - 1.0f); g011y = float(g011.y);
                    const Grad& g111 = perm_grad(xy11 + fz + 1);
                    d111xz = float(g111.x) * (rx - 1.0f) + float(g111.z) * (rz - 1.0f); g111y = float(g111.y);
                    last_floor_y = fy;
                }
                buffer[index] += amplitude * mth_lerp3(ax, ay, az, d000xz + g000y * ry, d100xz + g100y * ry, d010xz + g010y * (ry - 1.0f), d110xz + g110y * (ry - 1.0f),
                                                       d001xz + g001y * ry, d101xz + g101y * ry, d011xz + g011y * (ry - 1.0f), d111xz + g111y * (ry - 1.0f));
                index++;
            }
        }
    }
}

// ---- SmearedPerlinNoise ------------------------------------------------------------------------------------------

double SmearedPerlinNoise::compute_fudge_y(double original_y, double relative_y) const {
    double fudge_limit = original_y >= 0.0 && original_y < relative_y ? original_y : relative_y;
    return double(mth_floor(fudge_limit / fudge_ + double(1.0E-7f))) * fudge_;
}

float SmearedPerlinNoise::get(double ax, double ay, double az) const {
    double x = wrap(ax) + off_x_, y = wrap(ay) + off_y_, z = wrap(az) + off_z_;
    int fx = mth_floor(x), fy = mth_floor(y), fz = mth_floor(z);
    float rx = float(x - double(fx));
    double ry = y - double(fy);
    float rz = float(z - double(fz));
    float fudged = float(ry - compute_fudge_y(ay, ry));
    return sample_and_lerp(fx, fy, fz, rx, fudged, rz, float(ry));
}

void SmearedPerlinNoise::add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const {
    float d000xz = 0, d100xz = 0, d010xz = 0, d110xz = 0, d001xz = 0, d101xz = 0, d011xz = 0, d111xz = 0;
    float g000y = 0, g100y = 0, g010y = 0, g110y = 0, g001y = 0, g101y = 0, g011y = 0, g111y = 0;
    int index = 0;
    for (int iz = 0; iz < v.sz; iz++) {
        double z = wrap(double(v.block_z(iz)) * xz_scale) + off_z_;
        int fz = mth_floor(z);
        float rz = float(z - double(fz));
        float az = mth_smoothstep(rz);
        for (int ix = 0; ix < v.sx; ix++) {
            double x = wrap(double(v.block_x(ix)) * xz_scale) + off_x_;
            int fx = mth_floor(x);
            float rx = float(x - double(fx));
            int x0 = permute(fx), x1 = permute(fx + 1);
            float ax = mth_smoothstep(rx);
            int last_floor_y = INT32_MIN;
            for (int iy = 0; iy < v.sy; iy++) {
                double original_y = double(v.block_y(iy)) * y_scale;
                double y = wrap(original_y) + off_y_;
                int fy = mth_floor(y);
                double ry = y - double(fy);
                float ay = mth_smoothstep(float(ry));
                if (last_floor_y != fy) {
                    int xy00 = permute(x0 + fy), xy01 = permute(x0 + fy + 1), xy10 = permute(x1 + fy), xy11 = permute(x1 + fy + 1);
                    const Grad& g000 = perm_grad(xy00 + fz);
                    d000xz = float(g000.x) * rx + float(g000.z) * rz; g000y = float(g000.y);
                    const Grad& g100 = perm_grad(xy10 + fz);
                    d100xz = float(g100.x) * (rx - 1.0f) + float(g100.z) * rz; g100y = float(g100.y);
                    const Grad& g010 = perm_grad(xy01 + fz);
                    d010xz = float(g010.x) * rx + float(g010.z) * rz; g010y = float(g010.y);
                    const Grad& g110 = perm_grad(xy11 + fz);
                    d110xz = float(g110.x) * (rx - 1.0f) + float(g110.z) * rz; g110y = float(g110.y);
                    const Grad& g001 = perm_grad(xy00 + fz + 1);
                    d001xz = float(g001.x) * rx + float(g001.z) * (rz - 1.0f); g001y = float(g001.y);
                    const Grad& g101 = perm_grad(xy10 + fz + 1);
                    d101xz = float(g101.x) * (rx - 1.0f) + float(g101.z) * (rz - 1.0f); g101y = float(g101.y);
                    const Grad& g011 = perm_grad(xy01 + fz + 1);
                    d011xz = float(g011.x) * rx + float(g011.z) * (rz - 1.0f); g011y = float(g011.y);
                    const Grad& g111 = perm_grad(xy11 + fz + 1);
                    d111xz = float(g111.x) * (rx - 1.0f) + float(g111.z) * (rz - 1.0f); g111y = float(g111.y);
                    last_floor_y = fy;
                }
                float fy_rel = float(ry - compute_fudge_y(original_y, ry));
                buffer[index] += amplitude * mth_lerp3(ax, ay, az, d000xz + g000y * fy_rel, d100xz + g100y * fy_rel, d010xz + g010y * (fy_rel - 1.0f), d110xz + g110y * (fy_rel - 1.0f),
                                                       d001xz + g001y * fy_rel, d101xz + g101y * fy_rel, d011xz + g011y * (fy_rel - 1.0f), d111xz + g111y * (fy_rel - 1.0f));
                index++;
            }
        }
    }
}

// ---- NoiseStack --------------------------------------------------------------------------------------------------

float NoiseStack::get(double x, double y, double z) const {
    float value = 0.0f;
    for (const Layer& l : layers) {
        double f = l.frequency;
        value += l.amplitude * l.noise->get(x * f, y * f, z * f);
    }
    return value;
}

float NoiseStack::get(double x, double y) const {
    float value = 0.0f;
    for (const Layer& l : layers) {
        double f = l.frequency;
        value += l.amplitude * l.noise->get(x * f, y * f);
    }
    return value;
}

void NoiseStack::add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const {
    for (const Layer& l : layers) {
        double f = l.frequency;
        l.noise->add_to_volume(buffer, v, xz_scale * f, y_scale * f, amplitude * l.amplitude);
    }
}

// ---- NormalNoise -------------------------------------------------------------------------------------------------

namespace {

struct Octave { int index; double frequency; double amplitude; };

double amplitude_modifier(const std::vector<double>& mods, int i) { return mods.empty() ? 1.0 : mods[size_t(i)]; }

std::vector<Octave> build_octaves(int base_octave, double base_amplitude, int count, bool normalize, const std::vector<double>& mods) {
    double frequency = std::pow(2.0, double(base_octave));
    double amplitude = base_amplitude;
    if (normalize) amplitude *= std::pow(0.5, -double(count - 1)) / (std::pow(0.5, -double(count)) - 1.0);
    std::vector<Octave> out;
    for (int i = 0; i < count; i++) {
        double mod = amplitude_modifier(mods, i);
        if (mod != 0.0) out.push_back({base_octave + i, frequency, amplitude * mod});
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return out;
}

double estimate_deviation(const std::vector<Octave>& octaves) {
    double variance = 0.0;
    for (auto& o : octaves) {
        double d = 0.2702247831245211 * std::fabs(o.amplitude);
        variance += d * d;
    }
    return std::sqrt(variance);
}

double normalization_factor(double target_amplitude, const std::vector<Octave>& octaves) {
    double input_dev = estimate_deviation(octaves);
    if (input_dev == 0.0) return 0.0;
    double input_sum_dev = input_dev * std::sqrt(2.0);
    double target_dev = target_amplitude * 0.3333333333333333;
    return target_dev / input_sum_dev;
}

double parity_normalization_factor(double base_amplitude, int count, const std::vector<double>& mods) {
    int min_octave = INT32_MAX, max_octave = INT32_MIN;
    for (int i = 0; i < count; i++) {
        double m = amplitude_modifier(mods, i);
        if (m == 0.0) continue;
        min_octave = std::min(min_octave, i);
        max_octave = std::max(max_octave, i);
    }
    int span = max_octave - min_octave;
    double expected = 0.1 * (1.0 + 1.0 / double(span + 1));
    return base_amplitude * 0.5 * 0.3333333333333333 / expected;
}

double compute_normalization(const NoiseParams& p, const std::vector<Octave>& octaves) {
    double target = 0.0;
    for (auto& o : octaves) target += std::fabs(o.amplitude);
    double nf = normalization_factor(target, octaves);
    if (p.normalize == NoiseParams::LEGACY && nf != 0.0) nf = parity_normalization_factor(p.base_amplitude, p.octave_count, p.amplitude_modifiers);
    return nf;
}

}  // namespace

std::shared_ptr<NoiseStack> create_normal_noise(const NoiseParams& p, Rng& random) {
    std::vector<Octave> octaves = build_octaves(p.base_octave, p.base_amplitude, p.octave_count, p.normalize != NoiseParams::DISABLED, p.amplitude_modifiers);
    double nf = compute_normalization(p, octaves);
    std::unique_ptr<PosFactory> first_random = random.fork_positional();
    std::unique_ptr<PosFactory> second_random = random.fork_positional();
    auto stack = std::make_shared<NoiseStack>();
    for (auto& o : octaves) {
        std::string seed = "octave_" + std::to_string(o.index);
        auto r1 = first_random->from_hash_of(seed);
        auto first = std::make_shared<PerlinNoise>(*r1);
        auto r2 = second_random->from_hash_of(seed);
        auto second = std::make_shared<PerlinNoise>(*r2);
        float value_factor = float(nf * o.amplitude);
        stack->add(first, o.frequency, value_factor);
        stack->add(second, o.frequency * 1.0181268882175227, value_factor);
    }
    return stack;
}

// LegacyFbmInitializer.createForLegacyNetherBiome
static NoiseStack legacy_fbm(Rng& random, int first_octave, const std::vector<double>& amplitudes) {
    int octaves = int(amplitudes.size());
    int zero_octave_index = -first_octave;
    std::vector<std::shared_ptr<PerlinNoise>> levels(static_cast<size_t>(octaves));
    auto zero = std::make_shared<PerlinNoise>(random);
    if (zero_octave_index >= 0 && zero_octave_index < octaves && amplitudes[size_t(zero_octave_index)] != 0.0) levels[size_t(zero_octave_index)] = zero;
    for (int i = zero_octave_index - 1; i >= 0; i--) {
        if (i < octaves && amplitudes[size_t(i)] != 0.0) levels[size_t(i)] = std::make_shared<PerlinNoise>(random);
        else random.consume_count(262);
    }
    double factor = std::pow(2.0, double(-zero_octave_index));
    double value_factor = std::pow(2.0, double(octaves - 1)) / (std::pow(2.0, double(octaves)) - 1.0);
    NoiseStack stack;
    for (size_t i = 0; i < levels.size(); i++) {
        if (levels[i]) stack.add(levels[i], factor, float(value_factor * amplitudes[i]));
        factor *= 2.0;
        value_factor /= 2.0;
    }
    return stack;
}

std::shared_ptr<NoiseStack> create_legacy_nether_noise(const NoiseParams& p, Rng& random) {
    std::vector<Octave> octaves = build_octaves(p.base_octave, p.base_amplitude, p.octave_count, p.normalize != NoiseParams::DISABLED, p.amplitude_modifiers);
    double nf = compute_normalization(p, octaves);
    std::vector<double> amps = p.amplitude_modifiers;
    if (amps.empty()) amps.assign(size_t(p.octave_count), 1.0);
    NoiseStack first = legacy_fbm(random, p.base_octave, amps);
    NoiseStack second = legacy_fbm(random, p.base_octave, amps);
    float value_factor = float(nf * p.base_amplitude);
    auto out = std::make_shared<NoiseStack>();
    out->add_stack(first, 1.0, value_factor);
    out->add_stack(second, 1.0181268882175227, value_factor);
    return out;
}

// ---- SimplexNoise ------------------------------------------------------------------------------------------------

static const double kSqrt3 = std::sqrt(3.0);
static const double kF2 = 0.5 * (kSqrt3 - 1.0);
static const double kG2 = (3.0 - kSqrt3) / 6.0;

double SimplexNoise::corner_noise_3d(int index, double x, double y, double z, double base) const {
    double t0 = base - x * x - y * y - z * z;
    if (t0 < 0.0) return 0.0;
    t0 *= t0;
    return t0 * t0 * kGrad[index].dot(x, y, z);
}

float SimplexNoise::get(double xin_, double yin_) const {
    double xin = xin_ + off_x_, yin = yin_ + off_y_;
    double s = (xin + yin) * kF2;
    int i = mth_floor(xin + s);
    int j = mth_floor(yin + s);
    double t = double(i + j) * kG2;
    double X0 = double(i) - t, Y0 = double(j) - t;
    double x0 = xin - X0, y0 = yin - Y0;
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; } else { i1 = 0; j1 = 1; }
    double x1 = x0 - double(i1) + kG2, y1 = y0 - double(j1) + kG2;
    double x2 = x0 - 1.0 + 2.0 * kG2, y2 = y0 - 1.0 + 2.0 * kG2;
    int ii = i & 0xff, jj = j & 0xff;
    int gi0 = permute(ii + permute(jj)) % 12;
    int gi1 = permute(ii + i1 + permute(jj + j1)) % 12;
    int gi2 = permute(ii + 1 + permute(jj + 1)) % 12;
    double n0 = corner_noise_3d(gi0, x0, y0, 0.0, 0.5);
    double n1 = corner_noise_3d(gi1, x1, y1, 0.0, 0.5);
    double n2 = corner_noise_3d(gi2, x2, y2, 0.0, 0.5);
    return float(70.0 * (n0 + n1 + n2));
}

float SimplexNoise::get(double xin_, double yin_, double zin_) const {
    double xin = xin_ + off_x_, yin = yin_ + off_y_, zin = zin_ + off_z_;
    double s = (xin + yin + zin) * 0.3333333333333333;
    int i = mth_floor(xin + s), j = mth_floor(yin + s), k = mth_floor(zin + s);
    double t = double(i + j + k) * 0.16666666666666666;
    double x0 = xin - (double(i) - t), y0 = yin - (double(j) - t), z0 = zin - (double(k) - t);
    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
    else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
    else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    double x1 = x0 - double(i1) + 0.16666666666666666, y1 = y0 - double(j1) + 0.16666666666666666, z1 = z0 - double(k1) + 0.16666666666666666;
    double x2 = x0 - double(i2) + 0.3333333333333333, y2 = y0 - double(j2) + 0.3333333333333333, z2 = z0 - double(k2) + 0.3333333333333333;
    double x3 = x0 - 1.0 + 0.5, y3 = y0 - 1.0 + 0.5, z3 = z0 - 1.0 + 0.5;
    int ii = i & 0xff, jj = j & 0xff, kk = k & 0xff;
    int gi0 = permute(ii + permute(jj + permute(kk))) % 12;
    int gi1 = permute(ii + i1 + permute(jj + j1 + permute(kk + k1))) % 12;
    int gi2 = permute(ii + i2 + permute(jj + j2 + permute(kk + k2))) % 12;
    int gi3 = permute(ii + 1 + permute(jj + 1 + permute(kk + 1))) % 12;
    double n0 = corner_noise_3d(gi0, x0, y0, z0, 0.6);
    double n1 = corner_noise_3d(gi1, x1, y1, z1, 0.6);
    double n2 = corner_noise_3d(gi2, x2, y2, z2, 0.6);
    double n3 = corner_noise_3d(gi3, x3, y3, z3, 0.6);
    return float(32.0 * (n0 + n1 + n2 + n3));
}

}  // namespace pn::vg::v3
