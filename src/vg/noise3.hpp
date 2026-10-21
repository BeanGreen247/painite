// The noise classes of 26.3 (net.minecraft.world.level.levelgen.synth): single precision gradient noise, noise stacks and the normal noise built from them.
// 26.3 computes all density in 32 bit floats; every operation here is written in the order Mojang's code does it (build with -ffp-contract=off).
#pragma once
#include "random.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pn::vg::v3 {

// DensityVolume: a grid of block positions, y varying fastest then x then z
struct Volume {
    int sx = 1, sy = 1, sz = 1;
    int minx = 0, miny = 0, minz = 0;
    int stepx = 1, stepy = 1, stepz = 1;
    int size() const { return sx * sy * sz; }
    int block_x(int x) const { return minx + x * stepx; }
    int block_y(int y) const { return miny + y * stepy; }
    int block_z(int z) const { return minz + z * stepz; }
    int max_block_x() const { return minx + sx * stepx - 1; }
    int max_block_y() const { return miny + sy * stepy - 1; }
    int max_block_z() const { return minz + sz * stepz - 1; }
    int index(int x, int y, int z) const { return y + (x + z * sx) * sy; }
    // indexOfBlock: -1 when the block is not on the grid
    int index_of_block(int bx, int by, int bz) const;
    bool operator==(const Volume& o) const {
        return sx == o.sx && sy == o.sy && sz == o.sz && minx == o.minx && miny == o.miny && minz == o.minz && stepx == o.stepx && stepy == o.stepy && stepz == o.stepz;
    }
};

// Mth's float helpers
float mth_lerp(float a, float p0, float p1);
float mth_lerp2(float a1, float a2, float x00, float x10, float x01, float x11);
float mth_lerp3(float a1, float a2, float a3, float x000, float x100, float x010, float x110, float x001, float x101, float x011, float x111);
float mth_smoothstep(float x);
float mth_clamp(float v, float lo, float hi);

class Noise {
public:
    virtual ~Noise() = default;
    virtual float get(double x, double y, double z) const = 0;
    virtual float get(double x, double y) const = 0;
    // buffer[i] += amplitude * noise at each block of the volume (xzScale and yScale multiply the block coordinates)
    virtual void add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const;
};

class GradientNoise : public Noise {
public:
    explicit GradientNoise(Rng& random, double offset_scale = 256.0);
    static double wrap(double x);
protected:
    struct Grad { int x, y, z; double dot(double a, double b, double c) const { return double(x) * a + double(y) * b + double(z) * c; } };
    static const Grad kGrad[16];
    int permute(int x) const { return perms_[x & 0xff]; }
    const Grad& perm_grad(int x) const { return kGrad[permute(x) & 0xf]; }
    static float grad_dot(int hash, float x, float y, float z) {
        const Grad& g = kGrad[hash & 0xf];
        return float(g.x) * x + float(g.y) * y + float(g.z) * z;
    }
    uint8_t perms_[256];
    double off_x_, off_y_, off_z_;
};

class PerlinNoise : public GradientNoise {
public:
    explicit PerlinNoise(Rng& random) : GradientNoise(random) {}
    float get(double x, double y, double z) const override;
    float get(double x, double y) const override { return get(wrap(x), 0.0, wrap(y)); }
    void add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const override;
    // PerlinNoise.noiseWithDerivative is not needed by terrain generation
protected:
    float sample_and_lerp(int x, int y, int z, float rx, float ry, float rz, float original_ry) const;
};

// the old terrain noise's octave: the y lookups are smeared into steps of fudge_y_scale
class SmearedPerlinNoise : public PerlinNoise {
public:
    SmearedPerlinNoise(Rng& random, double fudge_y_scale) : PerlinNoise(random), fudge_(fudge_y_scale) {}
    float get(double x, double y, double z) const override;
    void add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const override;
private:
    double compute_fudge_y(double original_y, double relative_y) const;
    double fudge_;
};

class NoiseStack : public Noise {
public:
    struct Layer { std::shared_ptr<Noise> noise; double frequency; float amplitude; };
    NoiseStack() = default;
    void add(std::shared_ptr<Noise> n, double frequency, float amplitude) { layers.push_back({std::move(n), frequency, amplitude}); }
    void add_stack(const NoiseStack& s, double frequency, float amplitude) {
        for (auto& l : s.layers) layers.push_back({l.noise, l.frequency * frequency, l.amplitude * amplitude});
    }
    float get(double x, double y, double z) const override;
    float get(double x, double y) const override;
    void add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const override;
    std::vector<Layer> layers;
};

// NormalNoise.Parameters (data/minecraft/worldgen/noise/*.json)
struct NoiseParams {
    double base_amplitude = 1.0;
    int base_octave = 0;
    int octave_count = 1;
    enum Normalization { DISABLED, ENABLED, LEGACY } normalize = ENABLED;
    std::vector<double> amplitude_modifiers;   // empty: all 1
};

// NormalNoise: two perlin stacks at slightly different frequencies; create() draws the octave noises from `random`
std::shared_ptr<NoiseStack> create_normal_noise(const NoiseParams& p, Rng& random);
// NormalNoise.createForLegacyNetherBiome (the nether's temperature and vegetation noises)
std::shared_ptr<NoiseStack> create_legacy_nether_noise(const NoiseParams& p, Rng& random);

// SimplexNoise (the end islands): double arithmetic, float result
class SimplexNoise : public GradientNoise {
public:
    SimplexNoise(Rng& random, bool discard_noise_offset) : GradientNoise(random, discard_noise_offset ? 0.0 : 256.0) {}
    float get(double x, double y) const override;
    float get(double x, double y, double z) const override;
private:
    double corner_noise_3d(int index, double x, double y, double z, double base) const;
};

}  // namespace pn::vg::v3
