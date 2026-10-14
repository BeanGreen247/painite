#include "blockdata.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pn::vg {

BlockData::BlockData(const std::string& flags_path, const std::string& classes_path, const Registry& reg) : B_(Blocks::get()), reg_(reg) {
    flags_.assign(B_.count(), 0);
    std::ifstream f(flags_path);
    if (!f) throw std::runtime_error("missing " + flags_path + " (run tools/fetch_worldgen.sh)");
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        uint16_t id = B_.find(line.substr(0, a), line.substr(a + 1, b - a - 1));
        // find() falls back to the default state when the properties are unknown: only take exact matches
        const BlockInfo& bi = B_.info(id);
        if (bi.name != line.substr(0, a) || bi.props != line.substr(a + 1, b - a - 1)) continue;
        flags_[id] = uint32_t(std::stoull(line.substr(b + 1), nullptr, 16));
    }
    {
        std::ifstream cf(classes_path);
        if (!cf) throw std::runtime_error("missing " + classes_path + " (run tools/fetch_worldgen.sh)");
        while (std::getline(cf, line)) {
            size_t a = line.find('\t');
            if (a == std::string::npos) continue;
            std::vector<std::string> chain;
            std::stringstream ss(line.substr(a + 1));
            std::string k;
            while (std::getline(ss, k, '>')) chain.push_back(k);
            classes_[line.substr(0, a)] = std::move(chain);
        }
        class_of_.assign(B_.count(), nullptr);
        static const std::vector<std::string> none;
        for (size_t i = 0; i < B_.count(); i++) {
            auto it = classes_.find(B_.info(uint16_t(i)).name);
            class_of_[i] = it == classes_.end() ? &none : &it->second;
        }
    }
    chunk_flags_.assign(65536, 0);
    const auto& leaves = tag_table("minecraft:leaves");
    for (size_t i = 0; i < flags_.size() && i < 65536; i++) {
        uint32_t fl = flags_[i];
        uint8_t c = 0;
        if (fl & BF_AIR) c |= 1;
        if (fl & (BF_WATER | BF_LAVA)) c |= 2;
        if (fl & BF_BLOCKS_MOTION) c |= 4;
        if (i < leaves.size() && leaves[i]) c |= 8;
        chunk_flags_[i] = c;
    }
}

const std::vector<std::string>& BlockData::classes(uint16_t id) const {
    static const std::vector<std::string> none;
    return id < class_of_.size() ? *class_of_[id] : none;
}

const std::vector<uint8_t>& BlockData::tag_table(const std::string& tag) const {
    std::string key = Registry::strip(tag[0] == '#' ? tag.substr(1) : tag);
    std::lock_guard<std::mutex> l(mu_);
    auto it = tags_.find(key);
    if (it != tags_.end()) return *it->second;
    auto table = std::make_unique<std::vector<uint8_t>>(B_.count(), 0);
    const auto& members = reg_.tag_members("block", key);
    for (size_t i = 0; i < B_.count(); i++) (*table)[i] = members.count(B_.info(uint16_t(i)).name) ? 1 : 0;
    return *(tags_[key] = std::move(table));
}

bool BlockData::in_tag(uint16_t id, const std::string& tag) const {
    const auto& t = tag_table(tag);
    return id < t.size() && t[id];
}

static bool find_prop(const std::string& props, const std::string& key, size_t& vstart, size_t& vend) {
    size_t pos = 0;
    while (pos < props.size()) {
        size_t comma = props.find(',', pos);
        if (comma == std::string::npos) comma = props.size();
        size_t eq = props.find('=', pos);
        if (eq != std::string::npos && eq < comma && props.compare(pos, eq - pos, key) == 0 && eq - pos == key.size()) { vstart = eq + 1; vend = comma; return true; }
        pos = comma + 1;
    }
    return false;
}

std::string BlockData::prop(uint16_t id, const std::string& key) const {
    const std::string& p = B_.info(id).props;
    size_t a, b;
    return find_prop(p, key, a, b) ? p.substr(a, b - a) : std::string();
}

bool BlockData::has_prop(uint16_t id, const std::string& key) const {
    size_t a, b;
    return find_prop(B_.info(id).props, key, a, b);
}

uint16_t BlockData::with(uint16_t id, const std::string& key, const std::string& value) const {
    const BlockInfo& bi = B_.info(id);
    size_t a, b;
    if (!find_prop(bi.props, key, a, b)) return id;
    std::string np = bi.props.substr(0, a) + value + bi.props.substr(b);
    return B_.find(bi.name, np);
}

uint16_t BlockData::state(const Json& j) const {
    std::string name = j.at("Name").str;
    uint16_t id = B_.default_state(name);
    if (const Json* pr = j.find("Properties"))
        for (auto& kv : pr->obj) id = with(id, kv.first, kv.second.str);
    return id;
}

}  // namespace pn::vg
