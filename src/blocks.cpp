#include "blocks.hpp"
#include <algorithm>
#include <fstream>
#include <tuple>
#include <sstream>

namespace pn {

Blocks& Blocks::get() {
    static Blocks b;
    return b;
}

static bool has(const std::string& n, const char* s) { return n.find(s) != std::string::npos; }

static bool any(const std::string& n, std::initializer_list<const char*> l) {
    for (auto* s : l) if (n.find(s) != std::string::npos) return true;
    return false;
}

static void classify(BlockInfo& b) {
    const std::string& n = b.name;
    b.air = n == "minecraft:air" || n == "minecraft:cave_air" || n == "minecraft:void_air";
    b.fluid = n == "minecraft:water" || n == "minecraft:lava" || n == "minecraft:bubble_column" || n == "minecraft:kelp" || n == "minecraft:kelp_plant" ||
              n == "minecraft:seagrass" || n == "minecraft:tall_seagrass" || has(b.props, "waterlogged=true");
    b.replaceable = b.air || any(n, {":water", ":lava", "short_grass", "tall_grass", ":fern", "large_fern", "dead_bush", "seagrass", ":vine", ":snow", ":fire", ":light"}) && !has(n, "block");
    // light behaviour and collision, by name (the block report has neither)
    static const std::initializer_list<const char*> plants = {"torch", "flower", "grass", "fern", "vine", "sapling", "rail", "button", "lever", "dead_bush",
        "seagrass", "kelp", "fire", "pressure_plate", "tripwire", "redstone_wire", "sign", "banner", "portal", "wheat", "carrots", "potatoes", "beetroots",
        "petals", "leaf_litter", "wildflowers", "poppy", "dandelion", "tulip", "orchid", "allium", "bluet", "daisy", "cornflower", "rose", "peony", "lilac",
        "sunflower", "nether_wart", "roots", "sprouts", "bush", "dry_grass", "sweet_berry", "structure_void", "bubble_column"};
    static const std::initializer_list<const char*> clear = {"glass", "pane", "carpet", "slab", "stairs", "fence", "wall", "door", "chain", "lantern",
        "cobweb", "dripleaf", "candle", "flower_pot", "head", "skull", "_bed", "chest", "scaffolding", "cake", "coral", "comparator",
        "repeater", "lectern", "anvil", "hopper", "cauldron", "end_rod", "lightning_rod", "pointed_dripstone", "amethyst_cluster", "sea_pickle",
        "turtle_egg", "frogspawn", "glow_lichen", "sculk_vein", "hanging_roots", "spore_blossom", "azalea", "pitcher", "melon_stem", "pumpkin_stem", "cocoa", "fan", "barrier", "_bud"};
    bool block_word = has(n, "_block") || has(n, "grass_block");
    bool plant = (any(n, plants) && !block_word && n != "minecraft:light_gray_carpet" && !has(n, "light_blue") && !has(n, "light_gray")) || n == "minecraft:light";
    bool exact_clear = n == "minecraft:bamboo" || n == "minecraft:bamboo_sapling" || n == "minecraft:brown_mushroom" || n == "minecraft:red_mushroom" ||
                       n == "minecraft:tinted_glass_never";
    uint8_t op = 15;
    if (b.air || plant) op = 0;
    else if (n == "minecraft:water" || has(n, "leaves") || n == "minecraft:ice" || n == "minecraft:frosted_ice") op = 1;
    else if (n == "minecraft:lava") op = 0;
    else if ((any(n, clear) && !block_word) || exact_clear) op = 0;
    static const std::pair<const char*, int> lights[] = {{"minecraft:torch", 14}, {"minecraft:wall_torch", 14}, {"minecraft:soul_torch", 10},
        {"minecraft:soul_wall_torch", 10}, {"minecraft:lantern", 15}, {"minecraft:soul_lantern", 10}, {"minecraft:glowstone", 15}, {"minecraft:sea_lantern", 15},
        {"minecraft:shroomlight", 15}, {"minecraft:lava", 15}, {"minecraft:fire", 15}, {"minecraft:soul_fire", 10}, {"minecraft:jack_o_lantern", 15},
        {"minecraft:beacon", 15}, {"minecraft:end_rod", 14}, {"minecraft:magma_block", 3}, {"minecraft:ochre_froglight", 15}, {"minecraft:verdant_froglight", 15},
        {"minecraft:pearlescent_froglight", 15}, {"minecraft:glow_lichen", 7}, {"minecraft:enchanting_table", 7}, {"minecraft:ender_chest", 7},
        {"minecraft:crying_obsidian", 10}, {"minecraft:conduit", 15}, {"minecraft:end_portal", 15}, {"minecraft:nether_portal", 11}, {"minecraft:sculk_catalyst", 6},
        {"minecraft:copper_torch", 14}, {"minecraft:copper_lantern", 15}};
    for (auto& l : lights) if (n == l.first) b.emit = static_cast<uint8_t>(l.second);
    bool lit = has(b.props, "lit=true");
    if (lit && any(n, {"campfire"})) b.emit = 15;
    else if (lit && n == "minecraft:redstone_lamp") b.emit = 15;
    else if (lit && any(n, {"furnace", "smoker"})) b.emit = 13;
    else if (lit && n == "minecraft:redstone_torch") b.emit = 7;
    else if (lit && n == "minecraft:redstone_wall_torch") b.emit = 7;
    else if (lit && has(n, "candle")) b.emit = 3;
    else if (lit && has(n, "ore")) b.emit = 9;
    if (n == "minecraft:light") b.emit = 15;
    b.opacity = op;
    b.solid = !(b.air || plant || n == "minecraft:water" || n == "minecraft:lava" || b.replaceable);
}

bool Blocks::load(const std::vector<std::pair<int, std::string>>& files, std::string& err) {
    bool first = true;
    for (auto& [protocol, path] : files) {
        std::ifstream f(path);
        if (!f) { err = "cannot open " + path; return false; }
        std::string line;
        std::vector<int>& pm = proto_[protocol];
        std::unordered_map<std::string, int> keys;
        std::unordered_map<std::string, int> defs;
        std::vector<std::tuple<int, std::string, std::string, bool>> rows;
        while (std::getline(f, line)) {
            std::vector<std::string> p;
            std::stringstream ss(line);
            std::string cell;
            while (std::getline(ss, cell, '\t')) p.push_back(cell);
            if (p.size() < 3) continue;
            bool def = p.back() == "1";
            std::string props = p.size() >= 4 ? p[2] : "";
            rows.emplace_back(std::stoi(p[0]), p[1], props, def);
        }
        if (first) {
            int maxid = 0;
            for (auto& r : rows) maxid = std::max(maxid, std::get<0>(r));
            total_[protocol] = maxid + 1;
            if (maxid >= 65535) { err = "too many block states for 16 bit ids"; return false; }
            info_.resize(static_cast<size_t>(maxid) + 1);
            for (auto& [id, name, props, def] : rows) {
                BlockInfo& b = info_[static_cast<size_t>(id)];
                b.name = name;
                b.props = props;
                classify(b);
                std::string k = props.empty() ? name : name + "[" + props + "]";
                by_key_[k] = static_cast<uint16_t>(id);
                if (def) defaults_[name] = static_cast<uint16_t>(id);
            }
            auto st = defaults_.find("minecraft:stone");
            if (st != defaults_.end()) stone_ = st->second;
            pm.resize(info_.size());
            for (size_t i = 0; i < pm.size(); i++) pm[i] = static_cast<int>(i);
            first = false;
        } else {
            std::unordered_map<std::string, int> dstate;
            std::unordered_map<std::string, int> kmap;
            { int mx = 0; for (auto& r : rows) mx = std::max(mx, std::get<0>(r)); total_[protocol] = mx + 1; }
            for (auto& [id, name, props, def] : rows) {
                kmap[props.empty() ? name : name + "[" + props + "]"] = id;
                if (def) dstate[name] = id;
            }
            int stone_id = dstate.count("minecraft:stone") ? dstate["minecraft:stone"] : 1;
            pm.assign(info_.size(), 0);
            for (size_t i = 0; i < info_.size(); i++) {
                auto k = kmap.find(key(static_cast<uint16_t>(i)));
                if (k != kmap.end()) { pm[i] = k->second; continue; }
                auto d = dstate.find(info_[i].name);
                pm[i] = d != dstate.end() ? d->second : (info_[i].air ? 0 : stone_id);
            }
        }
    }
    return !info_.empty();
}

uint16_t Blocks::find(const std::string& name, const std::string& props) const {
    auto it = by_key_.find(props.empty() ? name : name + "[" + props + "]");
    if (it != by_key_.end()) return it->second;
    auto d = defaults_.find(name);
    return d != defaults_.end() ? d->second : 0;
}

uint16_t Blocks::default_state(const std::string& name) const {
    auto d = defaults_.find(name);
    return d != defaults_.end() ? d->second : 0;
}

int Blocks::proto_state(int protocol, uint16_t id) const {
    auto it = proto_.find(protocol);
    if (it == proto_.end() || id >= it->second.size()) return 0;
    return it->second[id];
}

}  // namespace pn
