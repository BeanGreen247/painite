#include "nbt.hpp"
#include <cstring>

namespace pn {

namespace {
struct R {
    const uint8_t* p;
    const uint8_t* e;
    bool bad = false;
    int depth = 0;
    bool need(size_t n) { if (static_cast<size_t>(e - p) < n) { bad = true; return false; } return true; }
    uint8_t u8() { if (!need(1)) return 0; return *p++; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = static_cast<uint16_t>(p[0] << 8 | p[1]); p += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int k = 0; k < 4; k++) v = v << 8 | p[k]; p += 4; return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int k = 0; k < 8; k++) v = v << 8 | p[k]; p += 8; return v; }
    std::string str() { size_t n = u16(); if (!need(n)) return {}; std::string s(reinterpret_cast<const char*>(p), n); p += n; return s; }
};

void read_payload(R& r, uint8_t t, Nbt& o) {
    o.type = t;
    if (r.bad || ++r.depth > 512) { r.bad = true; return; }
    switch (t) {
        case Nbt::Byte: o.i = static_cast<int8_t>(r.u8()); break;
        case Nbt::Short: o.i = static_cast<int16_t>(r.u16()); break;
        case Nbt::Int: o.i = static_cast<int32_t>(r.u32()); break;
        case Nbt::Long: o.i = static_cast<int64_t>(r.u64()); break;
        case Nbt::Float: { uint32_t u = r.u32(); float f; memcpy(&f, &u, 4); o.f = f; break; }
        case Nbt::Double: { uint64_t u = r.u64(); double d; memcpy(&d, &u, 8); o.f = d; break; }
        case Nbt::ByteArray: {
            uint32_t n = r.u32();
            if (!r.need(n)) break;
            o.ba.assign(r.p, r.p + n);
            r.p += n;
            break;
        }
        case Nbt::String: o.s = r.str(); break;
        case Nbt::List: {
            o.etype = r.u8();
            uint32_t n = r.u32();
            if (n > 0 && o.etype == Nbt::End) { r.bad = true; break; }
            if (n > static_cast<size_t>(r.e - r.p) && o.etype != Nbt::End) { r.bad = true; break; }
            o.list.resize(n);
            for (uint32_t k = 0; k < n && !r.bad; k++) read_payload(r, o.etype, o.list[k]);
            break;
        }
        case Nbt::Compound:
            while (!r.bad) {
                uint8_t ct = r.u8();
                if (ct == Nbt::End) break;
                std::string name = r.str();
                Nbt child;
                read_payload(r, ct, child);
                o.map.emplace_back(std::move(name), std::move(child));
            }
            break;
        case Nbt::IntArray: {
            uint32_t n = r.u32();
            if (!r.need(static_cast<size_t>(n) * 4)) break;
            o.ia.resize(n);
            for (auto& v : o.ia) v = static_cast<int32_t>(r.u32());
            break;
        }
        case Nbt::LongArray: {
            uint32_t n = r.u32();
            if (!r.need(static_cast<size_t>(n) * 8)) break;
            o.la.resize(n);
            for (auto& v : o.la) v = static_cast<int64_t>(r.u64());
            break;
        }
        default: r.bad = true;
    }
    r.depth--;
}

void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(static_cast<uint8_t>(v >> 8)); o.push_back(static_cast<uint8_t>(v)); }
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<uint8_t>(v >> s)); }
void put64(std::vector<uint8_t>& o, uint64_t v) { for (int s = 56; s >= 0; s -= 8) o.push_back(static_cast<uint8_t>(v >> s)); }
void put_str(std::vector<uint8_t>& o, const std::string& s) { put16(o, static_cast<uint16_t>(s.size())); o.insert(o.end(), s.begin(), s.end()); }

void write_payload(std::vector<uint8_t>& o, const Nbt& t) {
    switch (t.type) {
        case Nbt::Byte: o.push_back(static_cast<uint8_t>(t.i)); break;
        case Nbt::Short: put16(o, static_cast<uint16_t>(t.i)); break;
        case Nbt::Int: put32(o, static_cast<uint32_t>(t.i)); break;
        case Nbt::Long: put64(o, static_cast<uint64_t>(t.i)); break;
        case Nbt::Float: { float f = static_cast<float>(t.f); uint32_t u; memcpy(&u, &f, 4); put32(o, u); break; }
        case Nbt::Double: { uint64_t u; memcpy(&u, &t.f, 8); put64(o, u); break; }
        case Nbt::ByteArray: put32(o, static_cast<uint32_t>(t.ba.size())); o.insert(o.end(), t.ba.begin(), t.ba.end()); break;
        case Nbt::String: put_str(o, t.s); break;
        case Nbt::List:
            o.push_back(t.list.empty() ? static_cast<uint8_t>(Nbt::End) : t.etype);
            put32(o, static_cast<uint32_t>(t.list.size()));
            for (auto& e : t.list) write_payload(o, e);
            break;
        case Nbt::Compound:
            for (auto& kv : t.map) {
                o.push_back(kv.second.type);
                put_str(o, kv.first);
                write_payload(o, kv.second);
            }
            o.push_back(0);
            break;
        case Nbt::IntArray: put32(o, static_cast<uint32_t>(t.ia.size())); for (auto v : t.ia) put32(o, static_cast<uint32_t>(v)); break;
        case Nbt::LongArray: put32(o, static_cast<uint32_t>(t.la.size())); for (auto v : t.la) put64(o, static_cast<uint64_t>(v)); break;
        default: break;
    }
}
}  // namespace

bool nbt_read(const uint8_t* p, size_t n, Nbt& out, bool named, size_t* used) {
    R r{p, p + n};
    uint8_t t = r.u8();
    if (r.bad || (t != Nbt::Compound && t != Nbt::List)) return false;
    if (named) r.str();
    read_payload(r, t, out);
    if (used) *used = static_cast<size_t>(r.p - p);
    return !r.bad;
}

void nbt_write(std::vector<uint8_t>& o, const Nbt& t, bool named, const std::string& name) {
    o.push_back(t.type);
    if (named) put_str(o, name);
    write_payload(o, t);
}

}  // namespace pn
