// Byte buffers, VarInt helpers, small string/time utilities shared by all modules.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace pn {

struct Wr {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void boolean(bool v) { b.push_back(v ? 1 : 0); }
    void raw(const void* p, size_t n) {
        auto* c = static_cast<const uint8_t*>(p);
        b.insert(b.end(), c, c + n);
    }
    void be16(uint16_t v) { u8(v >> 8); u8(v); }
    void be32(uint32_t v) { for (int s = 24; s >= 0; s -= 8) u8(v >> s); }
    void be64(uint64_t v) { for (int s = 56; s >= 0; s -= 8) u8(v >> s); }
    void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); be32(u); }
    void f64(double v) { uint64_t u; std::memcpy(&u, &v, 8); be64(u); }
    void varint(int32_t v) {
        uint32_t u = static_cast<uint32_t>(v);
        while (u >= 0x80) { u8(static_cast<uint8_t>(u | 0x80)); u >>= 7; }
        u8(static_cast<uint8_t>(u));
    }
    void str(std::string_view s) { varint(static_cast<int32_t>(s.size())); raw(s.data(), s.size()); }
    // Network NBT root for a plain text component: TAG_String without a name.
    void text_nbt(std::string_view s) { u8(8); be16(static_cast<uint16_t>(s.size())); raw(s.data(), s.size()); }
    void le16(uint16_t v) { u8(v); u8(v >> 8); }
};

struct Rd {
    const uint8_t* p;
    const uint8_t* e;
    bool bad = false;
    Rd(const uint8_t* b, size_t n) : p(b), e(b + n) {}
    size_t left() const { return static_cast<size_t>(e - p); }
    uint8_t u8() { if (p >= e) { bad = true; return 0; } return *p++; }
    bool boolean() { return u8() != 0; }
    uint16_t be16() { uint16_t v = u8() << 8; return v | u8(); }
    uint32_t be32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | u8(); return v; }
    uint64_t be64() { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | u8(); return v; }
    float f32() { uint32_t u = be32(); float f; std::memcpy(&f, &u, 4); return f; }
    double f64() { uint64_t u = be64(); double d; std::memcpy(&d, &u, 8); return d; }
    int32_t varint() {
        uint32_t v = 0;
        for (int s = 0; s < 35; s += 7) {
            uint8_t c = u8();
            v |= static_cast<uint32_t>(c & 0x7f) << s;
            if (!(c & 0x80)) return static_cast<int32_t>(v);
            if (bad) return 0;
        }
        bad = true;
        return 0;
    }
    std::string str(size_t max = 32767) {
        int32_t n = varint();
        if (bad || n < 0 || static_cast<size_t>(n) > max * 4 || static_cast<size_t>(n) > left()) { bad = true; return {}; }
        std::string s(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
        p += n;
        return s;
    }
    void skip(size_t n) { if (n > left()) { bad = true; p = e; } else p += n; }
};

// Returns bytes consumed, 0 if incomplete, -1 if malformed (longer than 3 bytes for a frame length).
inline int read_varint(const uint8_t* p, size_t n, int32_t& out, int maxbytes = 5) {
    uint32_t v = 0;
    for (int i = 0; i < maxbytes; i++) {
        if (static_cast<size_t>(i) >= n) return 0;
        v |= static_cast<uint32_t>(p[i] & 0x7f) << (7 * i);
        if (!(p[i] & 0x80)) { out = static_cast<int32_t>(v); return i + 1; }
    }
    return -1;
}

std::string now_hms();
std::string trim(std::string_view s);
std::string lower(std::string_view s);

}  // namespace pn
