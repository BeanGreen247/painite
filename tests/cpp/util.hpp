// Shared helpers for the C++ test suite: byte buffers, varints, string packing, timing.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace pt {

using Bytes = std::vector<uint8_t>;

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Wr {
    Bytes b;
    Wr& u8(uint8_t v) { b.push_back(v); return *this; }
    Wr& be16(uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xff); return *this; }
    Wr& be32(uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back((v >> s) & 0xff); return *this; }
    Wr& be64(uint64_t v) { for (int s = 56; s >= 0; s -= 8) b.push_back((v >> s) & 0xff); return *this; }
    Wr& le16(uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); return *this; }
    Wr& le24(uint32_t v) { for (int s = 0; s <= 16; s += 8) b.push_back((v >> s) & 0xff); return *this; }
    Wr& le32(uint32_t v) { for (int s = 0; s <= 24; s += 8) b.push_back((v >> s) & 0xff); return *this; }
    Wr& le64(uint64_t v) { for (int s = 0; s <= 56; s += 8) b.push_back((v >> s) & 0xff); return *this; }
    Wr& f32(float f) { uint32_t u; memcpy(&u, &f, 4); return be32(u); }
    Wr& f64(double f) { uint64_t u; memcpy(&u, &f, 8); return be64(u); }
    Wr& lef32(float f) { uint32_t u; memcpy(&u, &f, 4); return le32(u); }
    Wr& varint(uint32_t v) { while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; } b.push_back(uint8_t(v)); return *this; }
    Wr& varuint(uint64_t v) { while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; } b.push_back(uint8_t(v)); return *this; }
    Wr& zz32(int32_t v) { return varuint((uint32_t(v) << 1) ^ uint32_t(v >> 31)); }
    Wr& str(const std::string& s) { varint(uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); return *this; }
    Wr& bstr(const std::string& s) { varuint(s.size()); b.insert(b.end(), s.begin(), s.end()); return *this; }   // Bedrock: varuint length
    Wr& raw(const Bytes& d) { b.insert(b.end(), d.begin(), d.end()); return *this; }
    Wr& raw(const void* p, size_t n) { auto* c = static_cast<const uint8_t*>(p); b.insert(b.end(), c, c + n); return *this; }
    Wr& zeros(size_t n) { b.insert(b.end(), n, 0); return *this; }
};

struct Rd {
    const uint8_t* p;
    const uint8_t* e;
    bool bad = false;
    Rd(const Bytes& d, size_t at = 0) : p(d.data() + std::min(at, d.size())), e(d.data() + d.size()) {}
    Rd(const uint8_t* a, size_t n) : p(a), e(a + n) {}
    size_t left() const { return size_t(e - p); }
    uint8_t u8() { if (p >= e) { bad = true; return 0; } return *p++; }
    uint16_t be16() { uint16_t v = u8(); return uint16_t(v << 8 | u8()); }
    uint32_t be32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v = v << 8 | u8(); return v; }
    uint64_t be64() { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | u8(); return v; }
    uint16_t le16() { uint16_t v = u8(); return uint16_t(v | u8() << 8); }
    uint32_t le24() { uint32_t v = u8(); v |= uint32_t(u8()) << 8; v |= uint32_t(u8()) << 16; return v; }
    uint32_t le32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(u8()) << (8 * i); return v; }
    uint64_t le64() { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= uint64_t(u8()) << (8 * i); return v; }
    float f32() { uint32_t u = be32(); float f; memcpy(&f, &u, 4); return f; }
    double f64() { uint64_t u = be64(); double f; memcpy(&f, &u, 8); return f; }
    float lef32() { uint32_t u = le32(); float f; memcpy(&f, &u, 4); return f; }
    uint32_t varint() { uint32_t v = 0; for (int s = 0; s < 35; s += 7) { uint8_t c = u8(); v |= uint32_t(c & 0x7f) << s; if (!(c & 0x80)) return v; if (bad) return 0; } bad = true; return 0; }
    uint64_t varuint() { uint64_t v = 0; for (int s = 0; s < 64; s += 7) { uint8_t c = u8(); v |= uint64_t(c & 0x7f) << s; if (!(c & 0x80)) return v; if (bad) return 0; } bad = true; return 0; }
    int32_t zz32() { uint32_t v = uint32_t(varuint()); return int32_t(v >> 1) ^ -int32_t(v & 1); }
    std::string str() { uint32_t n = varint(); if (n > left()) { bad = true; return ""; } std::string s(reinterpret_cast<const char*>(p), n); p += n; return s; }
    Bytes take(size_t n) { if (n > left()) { bad = true; return {}; } Bytes r(p, p + n); p += n; return r; }
    void skip(size_t n) { if (n > left()) { bad = true; p = e; } else p += n; }
};

inline bool contains(const Bytes& d, const std::string& s) {
    if (s.empty() || d.size() < s.size()) return false;
    return std::search(d.begin(), d.end(), s.begin(), s.end()) != d.end();
}

// Java block position: x 26 bits, z 26 bits, y 12 bits
inline uint64_t block_pos(int x, int y, int z) { return ((uint64_t(x) & 0x3FFFFFF) << 38) | ((uint64_t(z) & 0x3FFFFFF) << 12) | (uint64_t(y) & 0xFFF); }
inline void unpack_pos(uint64_t p, int& x, int& y, int& z) {
    x = int(int64_t(p) >> 38);
    y = int(int64_t(p << 52) >> 52);
    z = int(int64_t(p << 26) >> 38);
}

}  // namespace pt
