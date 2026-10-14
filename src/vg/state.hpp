// The per-world random state: the positional random factory every noise is seeded from, the noise instances, and a few named random factories.
#pragma once
#include "noise.hpp"
#include "registry.hpp"
#include <map>
#include <memory>
#include <mutex>

namespace pn::vg {

class WorldState {
public:
    WorldState(const Registry& reg, const Json& settings, int64_t seed);
    const Registry& registry() const { return reg_; }
    const Json& settings() const { return settings_; }
    int64_t seed() const { return seed_; }
    bool legacy_random_source() const { return legacy_; }
    // NormalNoise by id ("minecraft:temperature"), created on first use
    const NormalNoise* noise(const std::string& id) const;
    const PosFactory& random() const { return *random_; }
    const PosFactory& aquifer_random() const { return *aquifer_random_; }
    const PosFactory& ore_random() const { return *ore_random_; }
    // RandomState.getOrCreateRandomFactory(name)
    const PosFactory& random_factory(const std::string& name) const;
    std::unique_ptr<Rng> terrain_random() const;   // seeds the old blended terrain noise
private:
    const Registry& reg_;
    const Json& settings_;
    int64_t seed_;
    bool legacy_;
    std::unique_ptr<PosFactory> random_, aquifer_random_, ore_random_;
    mutable std::mutex mu_;
    mutable std::map<std::string, std::unique_ptr<NormalNoise>> noises_;
    mutable std::map<std::string, std::unique_ptr<PosFactory>> factories_;
};

}  // namespace pn::vg
