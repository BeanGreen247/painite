#include "recipe.hpp"
#include "data.hpp"
#include <algorithm>
#include <dirent.h>
#include <functional>

namespace pn::sim {

namespace {

void add_spec(Ingredient& ing, const std::string& spec) {
    if (spec.empty()) return;
    if (spec[0] == '#') {
        for (const std::string& m : Tags::get().members("item", spec.substr(1)))
            if (const ItemType* t = Items::get().by_name(m)) ing.items.insert(t->index);
    } else if (const ItemType* t = Items::get().by_name(spec)) ing.items.insert(t->index);
    ing.empty = false;
}

Ingredient parse_ingredient(const Json& j) {
    Ingredient ing;
    if (j.is_str()) add_spec(ing, j.str);
    else if (j.is_arr()) for (const Json& e : j.arr) add_spec(ing, e.string());
    return ing;
}

ItemStack parse_result(const Json& j) {
    const ItemType* t = nullptr;
    int count = 1;
    if (j.is_str()) t = Items::get().by_name(j.str);
    else if (j.is_obj()) { t = Items::get().by_name(j.get_string("id")); count = int(j.get_int("count", 1)); }
    return t ? ItemStack(t, count) : ItemStack();
}

// Is the grid's cell (x, y) in the shape at the offset, and does it hold what the shape wants there
bool shaped_at(const CraftingRecipe& r, CraftingContainer& g, int ox, int oy, bool mirror) {
    for (int y = 0; y < g.height; y++)
        for (int x = 0; x < g.width; x++) {
            int px = x - ox, py = y - oy;
            const ItemStack& s = g.item(x + y * g.width);
            if (px >= 0 && py >= 0 && px < r.width && py < r.height) {
                const Ingredient& ing = r.ingredients[size_t((mirror ? r.width - 1 - px : px) + py * r.width)];
                if (ing.empty) { if (!s.is_empty()) return false; }
                else if (!ing.matches(s)) return false;
            } else if (!s.is_empty()) return false;
        }
    return true;
}

bool shapeless_match(const std::vector<Ingredient>& ings, const std::vector<const ItemStack*>& stacks, size_t i, std::vector<bool>& used) {
    if (i == ings.size()) return true;
    for (size_t k = 0; k < stacks.size(); k++) {
        if (used[k] || !ings[i].matches(*stacks[k])) continue;
        used[k] = true;
        if (shapeless_match(ings, stacks, i + 1, used)) return true;
        used[k] = false;
    }
    return false;
}

}  // namespace

bool CraftingRecipe::matches(CraftingContainer& g) const {
    if (shaped) {
        if (width > g.width || height > g.height) return false;
        for (int oy = 0; oy <= g.height - height; oy++)
            for (int ox = 0; ox <= g.width - width; ox++)
                if (shaped_at(*this, g, ox, oy, false) || shaped_at(*this, g, ox, oy, true)) return true;
        return false;
    }
    std::vector<const ItemStack*> stacks;
    for (int i = 0; i < g.size(); i++) if (!g.item(i).is_empty()) stacks.push_back(&g.item(i));
    if (stacks.size() != ingredients.size()) return false;
    std::vector<bool> used(stacks.size(), false);
    return shapeless_match(ingredients, stacks, 0, used);
}

Recipes& Recipes::get() { static Recipes r; return r; }

bool Recipes::load(const std::string& game_dir, std::string& err) {
    std::string dir = game_dir + "/data/minecraft/recipe";
    DIR* dp = opendir(dir.c_str());
    if (!dp) { err = "cannot open " + dir; return false; }
    std::vector<std::string> files;
    while (dirent* e = readdir(dp)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) files.push_back(n);
    }
    closedir(dp);
    std::sort(files.begin(), files.end());
    crafting_.clear();
    cooking_.clear();
    for (const std::string& f : files) {
        try {
            Json j = Json::parse_file(dir + "/" + f);
            std::string type = j.get_string("type");
            if (type.rfind("minecraft:", 0) == 0) type.erase(0, 10);
            std::string name = f.substr(0, f.size() - 5);
            if (type == "crafting_shaped") {
                CraftingRecipe r;
                r.shaped = true;
                r.name = name;
                const Json& pat = j.at("pattern");
                r.height = int(pat.arr.size());
                for (const Json& row : pat.arr) r.width = std::max<int>(r.width, int(row.str.size()));
                for (const Json& row : pat.arr)
                    for (int x = 0; x < r.width; x++) {
                        char c = size_t(x) < row.str.size() ? row.str[size_t(x)] : ' ';
                        Ingredient ing;
                        if (c != ' ') { const Json* k = j.at("key").find(std::string(1, c)); if (k) ing = parse_ingredient(*k); }
                        r.ingredients.push_back(std::move(ing));
                    }
                r.result = parse_result(j.at("result"));
                if (!r.result.is_empty()) crafting_.push_back(std::move(r));
            } else if (type == "crafting_shapeless") {
                CraftingRecipe r;
                r.name = name;
                for (const Json& e : j.at("ingredients").arr) r.ingredients.push_back(parse_ingredient(e));
                r.result = parse_result(j.at("result"));
                if (!r.result.is_empty()) crafting_.push_back(std::move(r));
            } else if (type == "smelting" || type == "blasting" || type == "smoking" || type == "campfire_cooking") {
                CookingRecipe r;
                r.kind = type;
                r.name = name;
                r.ingredient = parse_ingredient(j.at("ingredient"));
                r.result = parse_result(j.at("result"));
                r.experience = j.get_number("experience", 0) ;
                r.time = int(j.get_int("cookingtime", type == "smelting" ? 200 : type == "blasting" || type == "smoking" ? 100 : 600));
                if (!r.result.is_empty()) cooking_.push_back(std::move(r));
            }
        } catch (const std::exception& e) { err = f + ": " + e.what(); }
    }
    err.clear();
    return !crafting_.empty();
}

const CraftingRecipe* Recipes::find_crafting(CraftingContainer& grid) const {
    bool any = false;
    for (int i = 0; i < grid.size() && !any; i++) any = !grid.item(i).is_empty();
    if (!any) return nullptr;
    for (const CraftingRecipe& r : crafting_) if (r.matches(grid)) return &r;
    return nullptr;
}

const CookingRecipe* Recipes::find_cooking(const std::string& kind, const ItemStack& input) const {
    if (input.is_empty()) return nullptr;
    for (const CookingRecipe& r : cooking_) if (r.kind == kind && r.ingredient.matches(input)) return &r;
    return nullptr;
}

const CookingRecipe* Recipes::cooking_by_name(const std::string& name) const {
    for (const CookingRecipe& r : cooking_) if (r.name == name) return &r;
    return nullptr;
}

// FuelValues.vanillaBurnTimes (in ticks)
int Recipes::fuel_ticks(const ItemStack& s) const {
    if (s.is_empty()) return 0;
    const std::string& n = s.item()->name;
    if (n.rfind("minecraft:crimson_", 0) == 0 || n.rfind("minecraft:warped_", 0) == 0) return 0;   // nether wood does not burn
    static const std::pair<const char*, int> exact[] = {
        {"minecraft:lava_bucket", 20000}, {"minecraft:coal_block", 16000}, {"minecraft:dried_kelp_block", 4001}, {"minecraft:blaze_rod", 2400},
        {"minecraft:coal", 1600}, {"minecraft:charcoal", 1600}, {"minecraft:bamboo", 50}, {"minecraft:scaffolding", 50}, {"minecraft:stick", 100},
        {"minecraft:bowl", 100}, {"minecraft:ladder", 300}, {"minecraft:bookshelf", 300}, {"minecraft:chiseled_bookshelf", 300}, {"minecraft:barrel", 300},
        {"minecraft:crafting_table", 300}, {"minecraft:cartography_table", 300}, {"minecraft:fletching_table", 300}, {"minecraft:smithing_table", 300},
        {"minecraft:loom", 300}, {"minecraft:composter", 300}, {"minecraft:lectern", 300}, {"minecraft:note_block", 300}, {"minecraft:jukebox", 300},
        {"minecraft:chest", 300}, {"minecraft:trapped_chest", 300}, {"minecraft:daylight_detector", 300}, {"minecraft:bow", 300}, {"minecraft:fishing_rod", 300},
        {"minecraft:mangrove_roots", 300}, {"minecraft:wooden_sword", 200}, {"minecraft:wooden_pickaxe", 200}, {"minecraft:wooden_axe", 200},
        {"minecraft:wooden_shovel", 200}, {"minecraft:wooden_hoe", 200}, {"minecraft:crossbow", 300}, {"minecraft:white_banner", 300}};
    for (auto& e : exact) if (n == e.first) return e.second;
    if (n.find("_planks") != std::string::npos || n.find("_log") != std::string::npos || n.find("_wood") != std::string::npos || n.find("_stem") != std::string::npos ||
        n.find("_hyphae") != std::string::npos) {
        return 300;
    }
    if (n.find("_slab") != std::string::npos && n.find("wooden") != std::string::npos) return 150;
    for (const char* w : {"oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "cherry", "bamboo", "pale_oak"}) {
        std::string p = std::string("minecraft:") + w + "_";
        if (n.rfind(p, 0) != 0) continue;
        if (n.find("_slab") != std::string::npos) return 150;
        for (const char* x : {"_stairs", "_fence", "_fence_gate", "_door", "_trapdoor", "_pressure_plate", "_sign", "_boat", "_chest_boat", "_button", "_hanging_sign", "_shelf"})
            if (n.find(x) != std::string::npos) return n.find("_button") != std::string::npos ? 100 : n.find("_sign") != std::string::npos || n.find("_boat") != std::string::npos ? 200 : 300;
        if (n.find("_sapling") != std::string::npos) return 100;
    }
    if (n.find("_wool") != std::string::npos || n.find("_carpet") != std::string::npos) return 100;
    if (n.find("_sapling") != std::string::npos) return 100;
    return 0;
}

RecipeCrafting& RecipeCrafting::get() { static RecipeCrafting r; return r; }

ItemStack RecipeCrafting::result(CraftingContainer& grid) {
    const CraftingRecipe* r = Recipes::get().find_crafting(grid);
    return r ? r->result : ItemStack();
}

// the items that stay in the grid when one of each ingredient is used up (a bucket stays when its contents were crafted away)
std::vector<ItemStack> RecipeCrafting::remaining(CraftingContainer& grid) {
    std::vector<ItemStack> out(size_t(grid.size()));
    for (int i = 0; i < grid.size(); i++) {
        const ItemStack& s = grid.item(i);
        if (s.is_empty()) continue;
        const Json* rem = s.item()->component("minecraft:crafting_remainder");
        if (!rem) rem = s.item()->component("minecraft:use_remainder");
        if (!rem) continue;
        const Json* id = rem->find("id");
        if (const ItemType* t = Items::get().by_name(id ? id->string() : rem->string())) out[size_t(i)] = ItemStack(t, 1);
    }
    return out;
}

}  // namespace pn::sim
