// The 26.3 density function engine (net.minecraft.world.level.levelgen.densityfunction): functions are parsed from the data files, rewritten (references
// inlined, caches shared, uniform axes sliced) and compiled into samplers that answer for one block or fill a volume, all in 32 bit floats exactly like Mojang's.
#pragma once
#include "json.hpp"
#include "noise3.hpp"
#include "registry.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pn::vg::v3 {

// RandomState: the seed's positional random, the named noises and the named random factories
class RandomState3 {
public:
    RandomState3(const Registry& reg, int64_t seed, bool legacy_random);
    std::shared_ptr<Noise> noise(const std::string& name);                   // "minecraft:temperature"
    std::unique_ptr<Rng> random_from_hash(const std::string& id);             // random.fromHashOf(id)
    PosFactory& random_factory(const std::string& name);                      // getOrCreateRandomFactory
    NoiseParams noise_params(const std::string& name) const;
    const Registry& reg;
    int64_t seed;
    bool legacy;
private:
    std::unique_ptr<PosFactory> random_;
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Noise>> noises_;
    std::map<std::string, std::unique_ptr<PosFactory>> factories_;
};

struct SamplerCtx;

// DensitySampler
struct Sampler {
    virtual ~Sampler() = default;
    virtual float value(SamplerCtx& c, int x, int y, int z) const = 0;
    virtual void volume(SamplerCtx& c, float* out, const Volume& v) const = 0;   // out has v.size() entries
};

// SamplerContext: the user fields (the beardifier) and the cache cells; enable_caches() makes the cache functions remember
struct SamplerCtx {
    struct Cell {
        bool has_volume = false;
        Volume vol;
        std::vector<float> buf;
        bool has_key = false;
        int64_t key = 0;
        float value = 0;
    };
    std::vector<Cell> cells;
    bool caches = false;
    const Sampler* beardifier = nullptr;   // ContextKey beardifier; null: the zero fallback
    const Sampler* blend_alpha = nullptr;  // Blender.ALPHA_KEY and OFFSET_KEY (never set for new worlds)
    const Sampler* blend_offset = nullptr;
    bool blending = false;
    std::vector<std::vector<float>> free_bufs;
    Cell* cell(int id) {
        if (!caches) return nullptr;
        if (size_t(id) >= cells.size()) cells.resize((size_t(id) / 16 + 1) * 16);
        return &cells[size_t(id)];
    }
    std::vector<float> acquire(size_t n) {
        std::vector<float> b;
        if (!free_bufs.empty()) { b = std::move(free_bufs.back()); free_bufs.pop_back(); }
        b.resize(n);
        return b;
    }
    void release(std::vector<float>&& b) { if (free_bufs.size() < 64) free_bufs.push_back(std::move(b)); }
};

// a temporary buffer returned to the context when it goes out of scope
struct Buf {
    SamplerCtx& c;
    std::vector<float> v;
    Buf(SamplerCtx& ctx, size_t n) : c(ctx), v(ctx.acquire(n)) {}
    ~Buf() { c.release(std::move(v)); }
    float* data() { return v.data(); }
    float& operator[](size_t i) { return v[i]; }
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
};

struct Df;
using DfP = std::shared_ptr<const Df>;

// DensityFunctionCompiler: parses functions from the data files and hands out the optimised samplers
class DfCompiler {
public:
    explicit DfCompiler(RandomState3& rs);
    ~DfCompiler();
    // a router entry: a reference string, a number or an inline object
    const Sampler* sampler(const Json& fn);
    const Sampler* sampler_of_ref(const std::string& id);
    RandomState3& rs;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace pn::vg::v3
