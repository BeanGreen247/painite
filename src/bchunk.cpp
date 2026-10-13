#include "bchunk.hpp"
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace pn {

static std::vector<int> g_biome_ids;

void bedrock_set_biomes(const std::vector<std::string>& java_names, const std::string& file) {
    std::map<std::string, int> m;
    std::ifstream f(file);
    std::string n;
    int id;
    while (f >> n >> id) m[n] = id;
    g_biome_ids.assign(java_names.size(), 1);
    for (size_t i = 0; i < java_names.size(); i++) {
        auto it = m.find(java_names[i]);
        if (it != m.end()) g_biome_ids[i] = it->second;
    }
}

static void put_varuint(std::vector<uint8_t>& o, uint32_t v) {
    while (v >= 0x80) { o.push_back(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
    o.push_back(static_cast<uint8_t>(v));
}
static void put_zz(std::vector<uint8_t>& o, int32_t v) { put_varuint(o, (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31)); }

// Paletted storage as Bedrock writes it: header (bits << 1 | 1), 32 bit words, palette. Index = x * 256 + z * 16 + y.
static void put_storage(std::vector<uint8_t>& o, const std::vector<int32_t>& vals, bool zigzag_palette_entries) {
    std::vector<int32_t> pal;
    std::vector<uint16_t> idx(4096);
    for (size_t i = 0; i < 4096; i++) {
        size_t k = 0;
        for (; k < pal.size(); k++) if (pal[k] == vals[i]) break;
        if (k == pal.size()) pal.push_back(vals[i]);
        idx[i] = static_cast<uint16_t>(k);
    }
    int bits = 1;
    static const int allowed[] = {1, 2, 3, 4, 5, 6, 8, 16};
    for (int b : allowed) { bits = b; if ((1u << b) >= pal.size()) break; }
    if (pal.size() == 1) {
        o.push_back(1);   // 0 bits per block, runtime palette
        put_zz(o, pal[0]);
        return;
    }
    o.push_back(static_cast<uint8_t>((bits << 1) | 1));
    int per = 32 / bits;
    size_t words = (4096 + static_cast<size_t>(per) - 1) / static_cast<size_t>(per);
    for (size_t w = 0; w < words; w++) {
        uint32_t v = 0;
        for (int k = 0; k < per; k++) {
            size_t i = w * static_cast<size_t>(per) + static_cast<size_t>(k);
            if (i < 4096) v |= static_cast<uint32_t>(idx[i]) << (k * bits);
        }
        for (int s = 0; s < 4; s++) o.push_back(static_cast<uint8_t>(v >> (8 * s)));
    }
    put_zz(o, static_cast<int32_t>(pal.size()));
    for (int32_t p : pal) put_zz(o, p);
    (void)zigzag_palette_entries;
}

void encode_bedrock_levelchunk(Chunk& c, std::vector<uint8_t>& tail) {
    std::lock_guard<std::mutex> l(c.mu);
    int highest = 0;
    for (int s = 0; s < kSections; s++) {
        if (!c.sec[s]) continue;
        for (int i = 0; i < 4096; i++) if (c.sec[s]->b[i]) { highest = s + 1; break; }
    }
    std::vector<uint8_t> biomes;
    std::vector<int32_t> prev;
    for (int s = 0; s < kSections; s++) {
        std::vector<int32_t> vals(4096, 1);
        {
            const uint8_t* cells = c.sec[s] ? c.sec[s]->biome : c.air_biomes[s];
            for (int x = 0; x < 16; x++) for (int z = 0; z < 16; z++) for (int y = 0; y < 16; y++) {
                uint8_t b = cells[(y >> 2) * 16 + (z >> 2) * 4 + (x >> 2)];
                vals[static_cast<size_t>(x * 256 + z * 16 + y)] = b < g_biome_ids.size() ? g_biome_ids[b] : 1;
            }
        }
        if (s > 0 && vals == prev) biomes.push_back(0xff);
        else put_storage(biomes, vals, true);
        prev = std::move(vals);
    }
    biomes.push_back(0);   // border blocks
    tail.clear();
    tail.push_back(0);                 // dimension
    tail.push_back(0);                 // sub chunk count (limited request mode)
    tail.push_back(1);                 // highest sub chunk present
    put_zz(tail, highest);
    tail.push_back(0);                 // cache disabled
    tail.push_back(0);                 // no blob hashes
    put_varuint(tail, static_cast<uint32_t>(biomes.size()));
    tail.insert(tail.end(), biomes.begin(), biomes.end());
}

void encode_bedrock_subchunk(Chunk& c, int section, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> l(c.mu);
    const Blocks& B = Blocks::get();
    out.clear();
    const Section* sec = (section >= 0 && section < kSections) ? c.sec[section].get() : nullptr;
    bool any = false;
    if (sec) for (int i = 0; i < 4096; i++) if (sec->b[i]) { any = true; break; }
    if (!any) { out = {6, 0, 3, 0, 3, 0, 0}; return; }   // success, all air
    std::vector<int32_t> l0(4096), l1(4096);
    int32_t air = B.bedrock_id(0);
    bool water_layer = false;
    int32_t water = B.bedrock_id(B.find("minecraft:water", "level=0"));
    for (int x = 0; x < 16; x++) for (int z = 0; z < 16; z++) for (int y = 0; y < 16; y++) {
        uint16_t b = sec->b[(y << 8) | (z << 4) | x];
        size_t o = static_cast<size_t>(x * 256 + z * 16 + y);
        l0[o] = B.bedrock_id(b);
        l1[o] = air;
        if (b && B.info(b).props.find("waterlogged=true") != std::string::npos) { l1[o] = water; water_layer = true; }
        // blocks that are fluids in Java hold the fluid in the first layer already
    }
    std::vector<uint8_t> payload;
    payload.push_back(9);
    payload.push_back(water_layer ? 2 : 1);
    payload.push_back(static_cast<uint8_t>(static_cast<int8_t>(section - 4)));
    put_storage(payload, l0, true);
    if (water_layer) put_storage(payload, l1, true);
    out.push_back(1);
    out.push_back(1);
    put_varuint(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    // heightmap: per column the number of blocks up to the highest non-air one inside this section
    int vals[256];
    bool all_hi = true, all_lo = true;
    for (int x = 0; x < 16; x++) for (int z = 0; z < 16; z++) {
        int h = 0;
        for (int y = 15; y >= 0; y--) if (sec->b[(y << 8) | (z << 4) | x]) { h = y + 1; break; }
        bool above = false;
        for (int s = section + 1; s < kSections && !above; s++) {
            if (!c.sec[s]) continue;
            for (int y = 0; y < 16; y++) if (c.sec[s]->b[(y << 8) | (z << 4) | x]) { above = true; break; }
        }
        vals[z * 16 + x] = above ? 16 : h;
        if (!above) all_hi = false;
        if (vals[z * 16 + x] != 0) all_lo = false;
    }
    if (all_hi) { out.insert(out.end(), {2, 0, 2, 0, 0}); return; }
    if (all_lo) { out.insert(out.end(), {3, 0, 3, 0, 0}); return; }
    out.push_back(1);
    out.push_back(1);
    for (int z = 0; z < 16; z++) {
        out.push_back(16);
        for (int x = 0; x < 16; x++) out.push_back(static_cast<uint8_t>(vals[z * 16 + x]));
    }
    out.push_back(4);   // render heightmap: copied
    out.push_back(0);
    out.push_back(0);
}

}  // namespace pn
