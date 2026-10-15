// Loot tables (net.minecraft.world.level.storage.loot): the data driven pools, entries, conditions and functions, evaluated with the same random calls
// in the same order as Mojang's classes, so a table rolled with a seed gives the items vanilla gives.
#pragma once
#include "inventory.hpp"
#include "../vg/random.hpp"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pn::sim {

struct LootBlockState {
    std::string name;                                  // "minecraft:wheat"
    std::map<std::string, std::string> props;
};

struct LootEntityInfo {
    std::string type;                                  // "minecraft:zombie"
    bool on_fire = false, baby = false, in_water = false, sneaking = false;
    std::function<int(const std::string&)> enchantment_level;   // levels on its equipment (looting, ...)
    std::map<std::string, std::string> components;               // variant style components of animals ("minecraft:mooshroom/variant": "red")
};

class LevelQuery {
public:
    virtual ~LevelQuery() = default;
    virtual std::string biome_at(int x, int y, int z) { return ""; }
};

struct LootParams {
    const ItemStack* tool = nullptr;
    const LootBlockState* block_state = nullptr;
    bool has_origin = false;
    double ox = 0, oy = 0, oz = 0;
    float explosion_radius = -1;                       // < 0: not an explosion
    const LootEntityInfo* this_entity = nullptr;
    const LootEntityInfo* attacker = nullptr;
    bool killed_by_player = false;
    float luck = 0;
    std::vector<std::string> damage_source_tags;       // tags of the damage that killed it, e.g. "minecraft:is_fire"
    LevelQuery* level = nullptr;
    vg::Rng* level_random = nullptr;                   // used when no seed is given
};

class LootTable;

class LootContext {
public:
    LootContext(const LootParams& p, vg::Rng& r) : params(p), random(r) {}
    const LootParams& params;
    vg::Rng& random;
    std::set<const void*> visited;
};

// A data pack's loot tables, loaded on first use from data/game/<version>/data/minecraft/loot_table
class LootTables {
public:
    static void set_dir(const std::string& game_dir);
    static const LootTable* get(const std::string& id);   // "minecraft:chests/simple_dungeon"; null when unknown
};

class LootTable {
public:
    virtual ~LootTable() = default;
    // LootTable.getRandomItems(params, seed): items with stacks above their maximum split into several
    void get_random_items(const LootParams& params, int64_t seed, std::vector<ItemStack>& out) const;
    // LootTable.fill: a container gets the items in shuffled free slots
    void fill(Container& container, const LootParams& params, int64_t seed) const;
    void get_random_items_raw(LootContext& ctx, std::vector<ItemStack>& out) const;
    static std::unique_ptr<LootTable> parse(const vg::Json& j);
    struct Impl;
    std::unique_ptr<Impl> impl;
private:
    void random_items_split(LootContext& ctx, std::vector<ItemStack>& out) const;
    static void shuffle_and_split(std::vector<ItemStack>& result, int available, vg::Rng& random);
};

}  // namespace pn::sim
