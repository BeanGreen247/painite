#include "registry.hpp"
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace pn::vg {

std::string Registry::path_of(const std::string& kind, const std::string& id) const {
    return dir_ + "/data/minecraft/worldgen/" + kind + "/" + strip(id) + ".json";
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
