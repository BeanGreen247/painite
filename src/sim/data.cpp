#include "data.hpp"
#include "../cachestats.hpp"
#include <cstring>
#include <fstream>
#include <sstream>

namespace pn::sim {

static std::string strip_ns(const std::string& s) { return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s; }
static std::string with_ns(const std::string& s) { return s.find(':') == std::string::npos ? "minecraft:" + s : s; }

Tags& Tags::get() { static Tags t; return t; }
void Tags::set_dir(const std::string& d) { Tags& t = get(); std::lock_guard<std::mutex> l(t.mu_); t.dir_ = d; t.cache_.clear(); }

void Tags::load(const std::string& registry, const std::string& name, std::set<std::string>& out, int depth) {
    if (depth > 16) return;
    std::string path = dir_ + "/data/minecraft/tags/" + registry + "/" + strip_ns(name) + ".json";
    vg::Json j;
    try { j = vg::Json::parse_file(path); } catch (const std::exception&) { return; }
    const vg::Json* v = j.find("values");
    if (!v) return;
    for (auto& e : v->arr) {
        std::string id = e.is_str() ? e.str : e.get_string("id");
        if (id.empty()) continue;
        if (id[0] == '#') load(registry, id.substr(1), out, depth + 1);
        else out.insert(with_ns(id));
    }
}

const std::set<std::string>& Tags::members(const std::string& registry, const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    std::string n = name;
    if (!n.empty() && n[0] == '#') n = n.substr(1);
    std::string key = registry + "/" + with_ns(n);
    static CacheStat& st = cache_stat("tags");
    auto it = cache_.find(key);
    if (it != cache_.end()) { st.hit(); return it->second; }
    st.miss();
    std::set<std::string> out;
    load(registry, n, out, 0);
    int64_t members = int64_t(out.size());
    st.add(1, 64 + members * 64);   // a rough size: the set nodes and their strings
    return cache_.emplace(key, std::move(out)).first->second;
}

static void ordered_load(const std::string& dir, const std::string& registry, const std::string& name, std::vector<std::string>& out, int depth) {
    if (depth > 16) return;
    vg::Json j;
    try { j = vg::Json::parse_file(dir + "/data/minecraft/tags/" + registry + "/" + strip_ns(name) + ".json"); } catch (const std::exception&) { return; }
    const vg::Json* v = j.find("values");
    if (!v) return;
    for (auto& e : v->arr) {
        std::string id = e.is_str() ? e.str : e.get_string("id");
        if (id.empty()) continue;
        if (id[0] == '#') ordered_load(dir, registry, id.substr(1), out, depth + 1);
        else out.push_back(with_ns(id));
    }
}
std::vector<std::string> Tags::ordered(const std::string& registry, const std::string& name) {
    std::vector<std::string> out;
    std::string n = name;
    if (!n.empty() && n[0] == '#') n = n.substr(1);
    std::lock_guard<std::mutex> l(mu_);
    ordered_load(dir_, registry, n, out, 0);
    return out;
}

bool Tags::contains(const std::string& registry, const std::string& name, const std::string& entry) { return members(registry, name).count(with_ns(entry)) != 0; }

bool Tags::matches(const std::string& registry, const std::string& spec, const std::string& entry) {
    if (spec.empty()) return false;
    if (spec[0] == '#') return contains(registry, spec, entry);
    if (spec.find(',') == std::string::npos) return with_ns(spec) == with_ns(entry);
    std::stringstream ss(spec);
    std::string part;
    while (std::getline(ss, part, ',')) if (!part.empty() && matches(registry, part, entry)) return true;
    return false;
}

static std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (true) {
        size_t e = line.find('\t', i);
        out.push_back(line.substr(i, e == std::string::npos ? std::string::npos : e - i));
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return out;
}

BlockTable& BlockTable::get() { static BlockTable t; return t; }
bool BlockTable::load(const std::string& dir, std::string& err) {
    std::ifstream f(dir + "/blockprops.txt");
    if (!f) { err = "no blockprops.txt"; return false; }
    std::string line;
    while (std::getline(f, line)) {
        auto c = split_tabs(line);
        if (c.size() < 10) continue;
        BlockProps p;
        p.destroy_time = strtof(c[1].c_str(), nullptr);
        p.explosion_resistance = strtof(c[2].c_str(), nullptr);
        p.requires_tool = c[3] == "1";
        p.friction = strtof(c[4].c_str(), nullptr);
        p.speed_factor = strtof(c[5].c_str(), nullptr);
        p.jump_factor = strtof(c[6].c_str(), nullptr);
        p.random_ticks = c[7] == "1";
        p.push_reaction = c[8];
        p.has_block_entity = c[9] == "1";
        props_[c[0]] = p;
    }
    return !props_.empty();
}
const BlockProps* BlockTable::find(const std::string& name) const { auto it = props_.find(name); return it == props_.end() ? nullptr : &it->second; }

EntityTable& EntityTable::get() { static EntityTable t; return t; }
bool EntityTable::load(const std::string& dir, std::string& err) {
    std::ifstream f(dir + "/entityprops.txt");
    if (!f) { err = "no entityprops.txt"; return false; }
    std::string line;
    while (std::getline(f, line)) {
        auto c = split_tabs(line);
        if (c.size() < 8) continue;
        EntityProps p;
        p.category = c[1];
        p.width = strtof(c[2].c_str(), nullptr);
        p.height = strtof(c[3].c_str(), nullptr);
        p.eye_height = strtof(c[4].c_str(), nullptr);
        p.fire_immune = c[5] == "1";
        p.tracking_range = atoi(c[6].c_str());
        p.update_interval = atoi(c[7].c_str());
        if (c.size() > 8) {
            std::stringstream ss(c[8]);
            std::string kv;
            while (std::getline(ss, kv, ',')) {
                size_t eq = kv.find('=');
                if (eq != std::string::npos) p.attributes[kv.substr(0, eq)] = strtod(kv.c_str() + eq + 1, nullptr);
            }
        }
        props_[c[0]] = p;
    }
    return !props_.empty();
}
const EntityProps* EntityTable::find(const std::string& name) const { auto it = props_.find(name); return it == props_.end() ? nullptr : &it->second; }

}  // namespace pn::sim
