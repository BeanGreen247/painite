// Mojang's world generation data (noise parameters, density functions, noise settings, biomes, features, ...) read from data/worldgen/<version>/.
#pragma once
#include "json.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace pn::vg {

class Registry {
public:
    // dir: data/worldgen/<version> (the folder that holds data/minecraft/...)
    explicit Registry(std::string dir) : dir_(std::move(dir)) {}
    // kind: "noise", "density_function", "noise_settings", "biome", "configured_carver", "placed_feature", ...; id: "minecraft:overworld/continents" or "overworld/continents"
    const Json& get(const std::string& kind, const std::string& id) const;
    bool exists(const std::string& kind, const std::string& id) const;
    const Json& tag(const std::string& id) const;   // tags/worldgen/<id>.json, e.g. "biome/has_structure/village_plains"
    // every block (or fluid) a tag holds, nested tags resolved: "minecraft:base_stone_overworld" or "#minecraft:base_stone_overworld"; kind is "block" or "fluid"
    // the same in the order Mojang's tag loader gives (file order, nested tags expanded in place), as HolderSet.getRandomElement indexes it
    std::vector<std::string> tag_ordered(const std::string& kind, const std::string& id) const;
    const std::set<std::string>& tag_members(const std::string& kind, const std::string& id) const;
    std::vector<std::string> list(const std::string& kind) const;
    const std::string& dir() const { return dir_; }
    // a datapack folder (holding data/minecraft/...) whose files shadow the extracted ones, like Mojang's datapack overrides
    void set_overlay(std::string dir) { overlay_ = std::move(dir); }
    static std::string strip(const std::string& id) { return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id; }
private:
    std::string path_of(const std::string& kind, const std::string& id) const;
    std::string dir_, overlay_;
    mutable std::mutex mu_;
    mutable std::map<std::string, std::unique_ptr<Json>> cache_;
    mutable std::map<std::string, std::unique_ptr<std::set<std::string>>> tag_cache_;
};

}  // namespace pn::vg
