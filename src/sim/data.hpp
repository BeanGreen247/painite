// Gameplay data tables read from data/game/<version>: tags, block facts, entity facts.
#pragma once
#include "../vg/json.hpp"
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn::sim {

// Tags (data/minecraft/tags/<registry>/...): "block", "item", "entity_type", "fluid", "game_event", "damage_type", "enchantment", "point_of_interest_type"...
class Tags {
public:
    static Tags& get();
    static void set_dir(const std::string& game_dir);
    // is `entry` ("minecraft:stone") a member of tag `name` ("minecraft:mineable/pickaxe" or "#minecraft:mineable/pickaxe") of `registry`
    bool contains(const std::string& registry, const std::string& name, const std::string& entry);
    const std::set<std::string>& members(const std::string& registry, const std::string& name);
    // the same in the order the data pack lists them (HolderSet.getRandomElement picks by index)
    std::vector<std::string> ordered(const std::string& registry, const std::string& name);
    // a tag reference ("#minecraft:x") or a single id or a comma separated list of ids
    bool matches(const std::string& registry, const std::string& spec, const std::string& entry);
private:
    std::string dir_;
    std::mutex mu_;
    std::map<std::string, std::set<std::string>> cache_;
    void load(const std::string& registry, const std::string& name, std::set<std::string>& out, int depth);
};

struct BlockProps {
    float destroy_time = 0;             // hardness, -1 = unbreakable
    float explosion_resistance = 0;
    bool requires_tool = false;
    float friction = 0.6f, speed_factor = 1.0f, jump_factor = 1.0f;
    bool random_ticks = false;
    std::string push_reaction = "NORMAL";
    bool has_block_entity = false;
};

class BlockTable {
public:
    static BlockTable& get();
    bool load(const std::string& game_dir, std::string& err);
    const BlockProps* find(const std::string& name) const;
private:
    std::unordered_map<std::string, BlockProps> props_;
};

struct EntityProps {
    std::string category;
    float width = 0.6f, height = 1.8f, eye_height = 1.62f;
    bool fire_immune = false;
    int tracking_range = 5, update_interval = 3;
    std::map<std::string, double> attributes;   // default attribute base values of living entities
};

class EntityTable {
public:
    static EntityTable& get();
    bool load(const std::string& game_dir, std::string& err);
    const EntityProps* find(const std::string& name) const;
private:
    std::unordered_map<std::string, EntityProps> props_;
};

}  // namespace pn::sim
