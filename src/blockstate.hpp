// Reading and changing the properties of a block state ("powered=true,facing=north"), cached because redstone asks for the same few changes all the time.
#pragma once
#include "blocks.hpp"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pn {

// the value of a property, "" when the state has none
inline std::string block_prop(uint16_t id, const char* key) {
    const std::string& props = Blocks::get().info(id).props;
    size_t klen = strlen(key);
    for (size_t i = 0; i < props.size();) {
        size_t e = props.find(',', i);
        if (e == std::string::npos) e = props.size();
        if (e - i > klen && props.compare(i, klen, key) == 0 && props[i + klen] == '=') return props.substr(i + klen + 1, e - i - klen - 1);
        i = e + 1;
    }
    return "";
}

inline bool block_flag(uint16_t id, const char* key) { return block_prop(id, key) == "true"; }
inline int block_int(uint16_t id, const char* key, int def = 0) { std::string v = block_prop(id, key); return v.empty() ? def : atoi(v.c_str()); }

// the same block with one property changed; the state itself when it has no such property or the value is already set
inline uint16_t block_with(uint16_t id, const char* key, const std::string& value) {
    thread_local std::unordered_map<std::string, uint16_t> cache;
    std::string ck = std::to_string(id) + "/" + key + "/" + value;
    auto it = cache.find(ck);
    if (it != cache.end()) return it->second;
    const BlockInfo& bi = Blocks::get().info(id);
    std::string out;
    bool changed = false;
    for (size_t i = 0; i < bi.props.size();) {
        size_t e = bi.props.find(',', i);
        if (e == std::string::npos) e = bi.props.size();
        std::string part = bi.props.substr(i, e - i);
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.compare(0, eq, key) == 0) { part = std::string(key) + "=" + value; changed = true; }
        if (!out.empty()) out += ',';
        out += part;
        i = e + 1;
    }
    uint16_t r = changed ? Blocks::get().find(bi.name, out) : id;
    cache[ck] = r;
    return r;
}
inline uint16_t block_with(uint16_t id, const char* key, bool value) { return block_with(id, key, std::string(value ? "true" : "false")); }
inline uint16_t block_with(uint16_t id, const char* key, int value) { return block_with(id, key, std::to_string(value)); }

inline bool block_named(uint16_t id, const char* name) { return Blocks::get().info(id).name == name; }
inline bool block_name_has(uint16_t id, const char* part) { return Blocks::get().info(id).name.find(part) != std::string::npos; }

}  // namespace pn
