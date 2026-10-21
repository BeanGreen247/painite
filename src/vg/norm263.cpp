// 26.3 changed the shape of the worldgen data (flat feature configs, block states as {id, properties}, block state providers as their own registry with
// shorter type names). The ported feature code reads the 26.2 shape, so the 26.3 files are rewritten into it when they are loaded.
#include "registry.hpp"
#include <set>
#include <sys/stat.h>

namespace pn::vg {

namespace {

bool file_exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

bool is_state(const Json& j) {
    if (!j.is_obj() || !j.has("id") || !j.at("id").is_str()) return false;
    if (j.at("id").str.find('/') != std::string::npos) return false;   // structure template ids are paths; block ids never are
    for (auto& kv : j.obj) if (kv.first != "id" && kv.first != "properties") return false;
    return true;
}

Json to_state(const Json& j) {
    Json out;
    out.type = Json::Object;
    Json name;
    name.type = Json::String;
    name.str = j.at("id").str;
    out.obj.emplace_back("Name", name);
    if (const Json* p = j.find("properties")) out.obj.emplace_back("Properties", *p);
    return out;
}

Json simple_provider(const Json& state) {
    Json out;
    out.type = Json::Object;
    Json t;
    t.type = Json::String;
    t.str = "minecraft:simple_state_provider";
    out.obj.emplace_back("type", t);
    out.obj.emplace_back("state", state);
    return out;
}

const std::set<std::string>& provider_keys() {
    static const std::set<std::string> k = {"trunk_provider", "foliage_provider", "below_trunk_provider", "state_provider", "provider", "to_place", "block_provider", "above_root_provider",
        "muddy_roots_provider", "root_provider", "cap_provider", "stem_provider", "hanging_root_state_provider", "root_state_provider", "alternate_inner_layer_provider", "filling_provider",
        "inner_layer_provider", "middle_layer_provider", "outer_layer_provider", "dirt_provider", "base_state_provider", "ground_provider", "block_state_provider", "barrier", "fluid", "ground_state", "then"};
    return k;
}

const std::set<std::string>& state_keys() {
    static const std::set<std::string> k = {"state", "data", "target", "decor_state", "hat_state", "stem_state", "valid_base_block", "base_block", "pointed_block", "contents", "rim",
        "default_state", "high_states", "low_states", "output_state", "block_state", "states", "inner_placements"};
    return k;
}

Json name_state(const std::string& id) {
    Json st;
    st.type = Json::Object;
    Json n;
    n.type = Json::String;
    n.str = id;
    st.obj.emplace_back("Name", n);
    return st;
}

std::string rename_provider(const std::string& t) {
    static const std::map<std::string, std::string> m = {{"minecraft:rule_based", "minecraft:rule_based_state_provider"}, {"minecraft:noise", "minecraft:noise_provider"},
        {"minecraft:dual_noise", "minecraft:dual_noise_provider"}, {"minecraft:noise_threshold", "minecraft:noise_threshold_provider"},
        {"minecraft:randomized_int", "minecraft:randomized_int_state_provider"}, {"minecraft:rotated", "minecraft:rotated_block_provider"}, {"minecraft:weighted", "minecraft:weighted_state_provider"}};
    auto it = m.find(t);
    return it == m.end() ? t : it->second;
}

struct Norm {
    const std::string& dir;
    void generic(Json& j);
    void provider(Json& j);
    void flatten_feature(Json& j);
};

void Norm::provider(Json& j) {
    if (j.is_str()) {
        std::string id = Registry::strip(j.str);
        std::string path = dir + "/data/minecraft/worldgen/block_state_provider/" + id + ".json";
        if (file_exists(path)) {
            Json p = Json::parse_file(path);
            provider(p);
            j = std::move(p);
        } else {
            Json st;
            st.type = Json::Object;
            Json n;
            n.type = Json::String;
            n.str = j.str;
            st.obj.emplace_back("Name", n);
            j = simple_provider(st);
        }
        return;
    }
    if (is_state(j)) { j = simple_provider(to_state(j)); return; }
    if (!j.is_obj()) { generic(j); return; }
    for (auto& kv : j.obj) {
        if (kv.first == "type" && kv.second.is_str()) kv.second.str = rename_provider(kv.second.str);
        else if (provider_keys().count(kv.first) || kv.first == "source" || kv.first == "then" || kv.first == "fallback") {
            // nested providers (rule_based "then", randomized_int "source", ...) are providers; other uses of these keys are states
            if (is_state(kv.second) || kv.second.is_str() || (kv.second.is_obj() && kv.second.has("type"))) provider(kv.second);
            else generic(kv.second);
        } else {
            if (state_keys().count(kv.first)) {
                if (kv.second.is_str() && kv.second.str.rfind("minecraft:", 0) == 0) { kv.second = name_state(kv.second.str); continue; }
                if (kv.second.is_arr()) for (auto& e : kv.second.arr) if (e.is_str() && e.str.rfind("minecraft:", 0) == 0) e = name_state(e.str);
            }
            generic(kv.second);
        }
    }
}

void Norm::flatten_feature(Json& j) {
    if (!j.is_obj() || !j.has("type") || j.has("config") || j.has("placement")) return;
    Json cfg;
    cfg.type = Json::Object;
    std::vector<std::pair<std::string, Json>> rest;
    Json type;
    for (auto& kv : j.obj) {
        if (kv.first == "type") type = kv.second; else cfg.obj.push_back(kv);
    }
    j.obj.clear();
    j.obj.emplace_back("type", type);
    j.obj.emplace_back("config", std::move(cfg));
}

void Norm::generic(Json& j) {
    if (j.is_arr()) { for (auto& e : j.arr) generic(e); return; }
    if (!j.is_obj()) return;
    if (is_state(j)) { j = to_state(j); return; }
    if (const Json* tj = j.find("type")) {   // a provider under a key that is not a known provider field (e.g. the "block" of a feature)
        if (tj->is_str() && rename_provider(tj->str) != tj->str) { provider(j); return; }
    }
    for (auto& kv : j.obj) {
        if (kv.first == "pool_aliases") continue;   // alias targets are pool ids, not block states
        if (state_keys().count(kv.first)) {
            if (kv.second.is_str() && kv.second.str.rfind("minecraft:", 0) == 0) { kv.second = name_state(kv.second.str); continue; }
            if (kv.second.is_arr()) for (auto& e : kv.second.arr) if (e.is_str() && e.str.rfind("minecraft:", 0) == 0) e = name_state(e.str);
        }
        if (provider_keys().count(kv.first) && (is_state(kv.second) || kv.second.is_str() || (kv.second.is_obj() && kv.second.has("type")))) provider(kv.second);
        else generic(kv.second);
        if ((kv.first == "feature" || kv.first == "default") && kv.second.is_obj()) flatten_feature(kv.second);
    }
}

}  // namespace

void normalize_263(const std::string& kind, Json& j, const std::string& dir) {
    if (kind != "configured_feature" && kind != "feature" && kind != "placed_feature" && kind != "structure" && kind != "template_pool" && kind != "processor_list" && kind != "structure_set" && kind != "block_state_provider") return;
    Norm n{dir};
    n.generic(j);
    if (kind == "configured_feature" || kind == "feature") n.flatten_feature(j);
}

}  // namespace pn::vg
