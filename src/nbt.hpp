// Minimal NBT tree: parse and write (big-endian, named root as in region files; or nameless as on the Java network).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pn {

struct Nbt {
    enum : uint8_t { End, Byte, Short, Int, Long, Float, Double, ByteArray, String, List, Compound, IntArray, LongArray };
    uint8_t type = End;
    int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<uint8_t> ba;
    std::vector<int32_t> ia;
    std::vector<int64_t> la;
    uint8_t etype = End;                           // list element type
    std::vector<Nbt> list;
    std::vector<std::pair<std::string, Nbt>> map;  // compound, insertion order

    const Nbt* get(const std::string& k) const {
        for (auto& kv : map) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    Nbt* get(const std::string& k) {
        for (auto& kv : map) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    void erase(const std::string& k) {
        for (size_t n = 0; n < map.size(); n++) if (map[n].first == k) { map.erase(map.begin() + static_cast<long>(n)); return; }
    }
    Nbt& put(const std::string& k, Nbt v) {
        if (auto* e = get(k)) { *e = std::move(v); return *e; }
        map.emplace_back(k, std::move(v));
        return map.back().second;
    }
    static Nbt make(uint8_t t, int64_t v = 0) { Nbt n; n.type = t; n.i = v; return n; }
    static Nbt str(std::string v) { Nbt n; n.type = String; n.s = std::move(v); return n; }
    static Nbt compound() { Nbt n; n.type = Compound; return n; }
    static Nbt list_of(uint8_t et) { Nbt n; n.type = List; n.etype = et; return n; }
    static Nbt longs(std::vector<int64_t> v) { Nbt n; n.type = LongArray; n.la = std::move(v); return n; }
    int64_t geti(const std::string& k, int64_t def = 0) const {
        auto* e = get(k);
        return e && e->type >= Byte && e->type <= Long ? e->i : def;
    }
    std::string gets(const std::string& k) const {
        auto* e = get(k);
        return e && e->type == String ? e->s : std::string();
    }
};

// Reads one root tag. named = region/file style (type, name, payload); !named = network style (type, payload).
bool nbt_read(const uint8_t* p, size_t n, Nbt& out, bool named = true, size_t* used = nullptr);
void nbt_write(std::vector<uint8_t>& o, const Nbt& t, bool named = true, const std::string& name = "");

}  // namespace pn
