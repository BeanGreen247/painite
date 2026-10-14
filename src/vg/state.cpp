#include "state.hpp"

namespace pn::vg {

WorldState::WorldState(const Registry& reg, const Json& settings, int64_t seed) : reg_(reg), settings_(settings), seed_(seed) {
    legacy_ = settings.get_bool("legacy_random_source", false);
    std::unique_ptr<Rng> r;
    if (legacy_) r = std::make_unique<LegacyRandom>(seed); else r = std::make_unique<Xoroshiro>(seed);
    random_ = r->fork_positional();
    aquifer_random_ = random_->from_hash_of("minecraft:aquifer")->fork_positional();
    ore_random_ = random_->from_hash_of("minecraft:ore")->fork_positional();
}

const NormalNoise* WorldState::noise(const std::string& id0) const {
    std::string id = id0.find(':') == std::string::npos ? "minecraft:" + id0 : id0;
    std::lock_guard<std::mutex> l(mu_);
    auto it = noises_.find(id);
    if (it != noises_.end()) return it->second.get();
    const Json& j = reg_.get("noise", id);
    NoiseParameters p;
    p.first_octave = int(j.at("firstOctave").int_value());
    for (auto& a : j.at("amplitudes").arr) p.amplitudes.push_back(a.number());
    std::unique_ptr<NormalNoise> n;
    if (id == "minecraft:nether/temperature" || id == "minecraft:nether/vegetation") {
        // the nether biome noises keep the pre-1.18 seeding: a legacy random seeded with seed (+1 for the vegetation) and sequential octaves
        LegacyRandom r(seed_ + (id == "minecraft:nether/vegetation" ? 1 : 0));
        n = std::make_unique<NormalNoise>(r, p, false);
    } else {
        auto r = random_->from_hash_of(id);
        n = std::make_unique<NormalNoise>(*r, p, true);
    }
    const NormalNoise* ptr = n.get();
    noises_[id] = std::move(n);
    return ptr;
}

const PosFactory& WorldState::random_factory(const std::string& name) const {
    std::lock_guard<std::mutex> l(mu_);
    auto it = factories_.find(name);
    if (it != factories_.end()) return *it->second;
    auto f = random_->from_hash_of(name)->fork_positional();
    const PosFactory& ref = *f;
    factories_[name] = std::move(f);
    return ref;
}

std::unique_ptr<Rng> WorldState::terrain_random() const {
    if (legacy_) return std::make_unique<LegacyRandom>(seed_);
    return random_->from_hash_of("minecraft:terrain");
}

}  // namespace pn::vg
