#include "chunkparse.hpp"
#include <fstream>

namespace pt {

static std::vector<int> container(Rd& r, int n, bool blocks) {
    int bits = r.u8();
    std::vector<int> pal;
    if (bits == 0) { int v = int(r.varint()); return std::vector<int>(size_t(n), v); }
    bool use_pal = blocks ? bits <= 8 : bits <= 3;
    if (use_pal) {
        int cnt = int(r.varint());
        for (int i = 0; i < cnt && !r.bad; i++) pal.push_back(int(r.varint()));
    }
    int per = 64 / bits;
    size_t nl = size_t((n + per - 1) / per);
    std::vector<uint64_t> longs(nl);
    for (auto& l : longs) l = r.be64();
    std::vector<int> out(static_cast<size_t>(n));
    for (int k = 0; k < n; k++) {
        uint64_t v = (longs[size_t(k / per)] >> ((k % per) * bits)) & ((1ull << bits) - 1);
        out[size_t(k)] = use_pal ? (v < pal.size() ? pal[size_t(v)] : -1) : int(v);
    }
    return out;
}

ChunkData parse_chunk(const Bytes& d) {
    ChunkData c;
    Rd r(d);
    c.x = int(r.be32());
    c.z = int(r.be32());
    int cnt = int(r.varint());
    for (int i = 0; i < cnt; i++) { r.varint(); int ln = int(r.varint()); r.skip(size_t(ln) * 8); }
    int sz = int(r.varint());
    const uint8_t* end = r.p + sz;
    while (!r.bad && r.p < end) {
        SectionData s;
        s.nonair = int(int16_t(r.be16()));
        s.fluid = int(int16_t(r.be16()));
        s.blocks = container(r, 4096, true);
        s.biomes = container(r, 64, false);
        c.sections.push_back(std::move(s));
    }
    c.rest.assign(r.p, r.e);
    c.ok = !r.bad;
    return c;
}

static const uint8_t* skip_nbt(const uint8_t* p, const uint8_t* e, int t) {
    static const int sizes[7] = {0, 1, 2, 4, 8, 4, 8};
    if (p > e) return e + 1;
    auto be32 = [&](const uint8_t* q) { return int32_t(uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3]); };
    if (t == 0) return p;
    if (t >= 1 && t <= 6) return p + sizes[t];
    if (t == 7) return p + 4 + be32(p);
    if (t == 8) return p + 2 + (p[0] << 8 | p[1]);
    if (t == 9) {
        int et = p[0];
        int n = be32(p + 1);
        p += 5;
        for (int i = 0; i < n && p <= e; i++) p = skip_nbt(p, e, et);
        return p;
    }
    if (t == 10) {
        while (p < e && *p != 0) {
            int tt = *p;
            int ln = p[1] << 8 | p[2];
            p += 3 + ln;
            p = skip_nbt(p, e, tt);
        }
        return p + 1;
    }
    if (t == 11) return p + 4 + 4 * be32(p);
    if (t == 12) return p + 4 + 8 * be32(p);
    return e + 1;
}

TailInfo parse_tail(const Bytes& rest) {
    TailInfo t;
    Rd r(rest);
    int nbe = int(r.varint());
    t.block_entities = nbe;
    for (int i = 0; i < nbe && !r.bad; i++) {
        r.u8();
        int y = int(int16_t(r.be16()));
        int type = int(r.varint());
        int tag = r.u8();
        if (tag != 0) { const uint8_t* q = skip_nbt(r.p, r.e, tag); if (q > r.e) { r.bad = true; break; } r.p = q; }
        t.entities.emplace_back(type, y);
    }
    for (int m = 0; m < 4; m++) {
        int n = int(r.varint());
        uint64_t v = 0;
        for (int i = 0; i < n && !r.bad; i++) v |= uint64_t(r.u8()) << (8 * i);
        t.masks[m] = v;
    }
    for (int a = 0; a < 2; a++) {
        int n = int(r.varint());
        for (int k = 0; k < n && !r.bad; k++) { int ln = int(r.varint()); if (ln != 2048) r.bad = true; r.skip(size_t(ln)); }
        t.arrays[a] = n;
    }
    t.exact = !r.bad && r.left() == 0;
    return t;
}

std::map<int, std::string> load_block_names(const std::string& path, bool with_props) {
    std::map<int, std::string> m;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find('\t'), b = line.find('\t', a + 1), c = line.find('\t', b + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        std::string name = line.substr(a + 1, b - a - 1);
        std::string props = c == std::string::npos ? line.substr(b + 1) : line.substr(b + 1, c - b - 1);
        m[std::stoi(line.substr(0, a))] = with_props && !props.empty() ? name + "[" + props + "]" : name;
    }
    return m;
}

}  // namespace pt
