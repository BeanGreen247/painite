#include "registry.hpp"
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace pn::vg {

void normalize_263(const std::string& kind, Json& j, const std::string& dir);

std::string Registry::path_of(const std::string& kind_in, const std::string& id) const {
    // 26.3 renamed two registries: configured_feature -> feature, configured_carver -> carver
    std::string kind = kind_in;
    if (kind == "configured_feature" || kind == "configured_carver") {
        std::string alt = kind == "configured_feature" ? "feature" : "carver";
        struct stat st0;
        if (stat((dir_ + "/data/minecraft/worldgen/" + alt).c_str(), &st0) == 0) kind = alt;
    }
    std::string rel = "/data/minecraft/worldgen/" + kind + "/" + strip(id) + ".json";
    if (!overlay_.empty()) {
        struct stat st;
        if (stat((overlay_ + rel).c_str(), &st) == 0) return overlay_ + rel;
    }
    return dir_ + rel;
}

bool Registry::exists(const std::string& kind, const std::string& id) const {
    struct stat st;
    return stat(path_of(kind, id).c_str(), &st) == 0;
}

const Json& Registry::get(const std::string& kind, const std::string& id) const {
    std::string path = path_of(kind, id);
    std::lock_guard<std::mutex> l(mu_);
    auto it = cache_.find(path);
    if (it != cache_.end()) return *it->second;
    auto j = std::make_unique<Json>(Json::parse_file(path));
    if (dir_.find("26.3") != std::string::npos) normalize_263(kind, *j, dir_);
    const Json& ref = *j;
    cache_[path] = std::move(j);
    return ref;
}

const Json& Registry::tag(const std::string& id) const {
    std::string path = dir_ + "/data/minecraft/tags/worldgen/" + strip(id) + ".json";
    std::lock_guard<std::mutex> l(mu_);
    auto it = cache_.find(path);
    if (it != cache_.end()) return *it->second;
    auto j = std::make_unique<Json>(Json::parse_file(path));
    const Json& ref = *j;
    cache_[path] = std::move(j);
    return ref;
}

const std::set<std::string>& Registry::tag_members(const std::string& kind, const std::string& id) const {
    std::string name = id;
    if (!name.empty() && name[0] == '#') name = name.substr(1);
    name = strip(name);
    std::string key = kind + "/" + name;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = tag_cache_.find(key);
        if (it != tag_cache_.end()) return *it->second;
    }
    auto out = std::make_unique<std::set<std::string>>();
    Json j = Json::parse_file(dir_ + "/data/minecraft/tags/" + key + ".json");
    for (auto& v : j.at("values").arr) {
        std::string e = v.is_str() ? v.str : v.at("id").str;
        if (!e.empty() && e[0] == '#') { auto& sub = tag_members(kind, e); out->insert(sub.begin(), sub.end()); }
        else out->insert(e.rfind("minecraft:", 0) == 0 ? e : "minecraft:" + e);
    }
    std::lock_guard<std::mutex> l(mu_);
    auto& slot = tag_cache_[key];
    if (!slot) slot = std::move(out);
    return *slot;
}

std::vector<std::string> Registry::tag_ordered(const std::string& kind, const std::string& id) const {
    std::string name = id;
    if (!name.empty() && name[0] == '#') name = name.substr(1);
    Json j = Json::parse_file(dir_ + "/data/minecraft/tags/" + kind + "/" + strip(name) + ".json");
    std::vector<std::string> out;
    for (auto& v : j.at("values").arr) {
        std::string e = v.is_str() ? v.str : v.at("id").str;
        if (!e.empty() && e[0] == '#') { for (auto& m : tag_ordered(kind, e)) if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m); }
        else {
            if (e.rfind("minecraft:", 0) != 0) e = "minecraft:" + e;
            if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
        }
    }
    return out;
}

static void walk(const std::string& base, const std::string& rel, std::vector<std::string>& out) {
    std::string dir = rel.empty() ? base : base + "/" + rel;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string r = rel.empty() ? n : rel + "/" + n;
        struct stat st;
        if (stat((base + "/" + r).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) walk(base, r, out);
        else if (n.size() > 5 && n.substr(n.size() - 5) == ".json") out.push_back("minecraft:" + r.substr(0, r.size() - 5));
    }
    closedir(d);
}

std::vector<std::string> Registry::list(const std::string& kind) const {
    std::vector<std::string> out;
    walk(dir_ + "/data/minecraft/worldgen/" + kind, "", out);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace pn::vg
