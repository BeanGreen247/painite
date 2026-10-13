// Java level_chunk_with_light body from a world chunk.
#include "jchunk.hpp"
#include <algorithm>
#include <cstring>

namespace pn {

static int ceil_log2(int v) { int b = 0; while ((1 << b) < v) b++; return b; }

static void put_longs(Wr& w, const std::vector<uint64_t>& v) { for (uint64_t x : v) w.be64(x); }

// Block state container: single value, linear/hash palette (4..8 bits) or global ids.
static void put_block_container(Wr& w, const std::vector<int>& vals, int global_bits) {
    std::vector<int> pal;
    std::vector<uint16_t> idx(4096);
    for (size_t i = 0; i < 4096; i++) {
        int v = vals[i];
        size_t k = 0;
        for (; k < pal.size(); k++) if (pal[k] == v) break;
        if (k == pal.size()) pal.push_back(v);
        idx[i] = static_cast<uint16_t>(k);
        if (pal.size() > 256) break;
    }
    if (pal.size() == 1) { w.u8(0); w.varint(pal[0]); return; }
    if (pal.size() <= 256) {
        int bits = std::max(4, ceil_log2(static_cast<int>(pal.size())));
        w.u8(static_cast<uint8_t>(bits));
        w.varint(static_cast<int32_t>(pal.size()));
        for (int p : pal) w.varint(p);
        int per = 64 / bits;
        std::vector<uint64_t> longs((4096 + static_cast<size_t>(per) - 1) / static_cast<size_t>(per), 0);
        for (size_t i = 0; i < 4096; i++) longs[i / static_cast<size_t>(per)] |= static_cast<uint64_t>(idx[i]) << ((i % static_cast<size_t>(per)) * static_cast<size_t>(bits));
        put_longs(w, longs);
        return;
    }
    int bits = global_bits;
    w.u8(static_cast<uint8_t>(bits));
    int per = 64 / bits;
    std::vector<uint64_t> longs((4096 + static_cast<size_t>(per) - 1) / static_cast<size_t>(per), 0);
    for (size_t i = 0; i < 4096; i++) longs[i / static_cast<size_t>(per)] |= static_cast<uint64_t>(vals[i]) << ((i % static_cast<size_t>(per)) * static_cast<size_t>(bits));
    put_longs(w, longs);
}

static void put_biome_container(Wr& w, const std::vector<int>& vals, int global_bits) {
    std::vector<int> pal;
    std::vector<uint8_t> idx(64);
    for (size_t i = 0; i < 64; i++) {
        size_t k = 0;
        for (; k < pal.size(); k++) if (pal[k] == vals[i]) break;
        if (k == pal.size()) pal.push_back(vals[i]);
        idx[i] = static_cast<uint8_t>(k);
    }
    if (pal.size() == 1) { w.u8(0); w.varint(pal[0]); return; }
    if (pal.size() <= 8) {
        int bits = std::max(1, ceil_log2(static_cast<int>(pal.size())));
        w.u8(static_cast<uint8_t>(bits));
        w.varint(static_cast<int32_t>(pal.size()));
        for (int p : pal) w.varint(p);
        int per = 64 / bits;
        std::vector<uint64_t> longs((64 + static_cast<size_t>(per) - 1) / static_cast<size_t>(per), 0);
        for (size_t i = 0; i < 64; i++) longs[i / static_cast<size_t>(per)] |= static_cast<uint64_t>(idx[i]) << ((i % static_cast<size_t>(per)) * static_cast<size_t>(bits));
        put_longs(w, longs);
        return;
    }
    w.u8(static_cast<uint8_t>(global_bits));
    int per = 64 / global_bits;
    std::vector<uint64_t> longs((64 + static_cast<size_t>(per) - 1) / static_cast<size_t>(per), 0);
    for (size_t i = 0; i < 64; i++) longs[i / static_cast<size_t>(per)] |= static_cast<uint64_t>(vals[i]) << ((i % static_cast<size_t>(per)) * static_cast<size_t>(global_bits));
    put_longs(w, longs);
}

static std::vector<uint64_t> heightmap_longs(const std::vector<int16_t>& h) {
    std::vector<uint64_t> l(37, 0);
    for (size_t i = 0; i < 256; i++) l[i / 7] |= static_cast<uint64_t>(h[i] - kMinY) << ((i % 7) * 9);
    return l;
}

static void put_bitset(Wr& w, const std::vector<bool>& bits) {
    size_t nbytes = 0;
    for (size_t i = 0; i < bits.size(); i++) if (bits[i]) nbytes = i / 8 + 1;
    w.varint(static_cast<int32_t>(nbytes));
    for (size_t b = 0; b < nbytes; b++) {
        uint8_t v = 0;
        for (size_t k = 0; k < 8 && b * 8 + k < bits.size(); k++) if (bits[b * 8 + k]) v |= static_cast<uint8_t>(1u << k);
        w.u8(v);
    }
}

void encode_java_chunk(Chunk& c, const VanillaData& vd, const std::vector<std::string>& biome_names, Wr& out) {
    const Blocks& B = Blocks::get();
    std::lock_guard<std::mutex> l(c.mu);
    Light L;
    compute_light(c, L);
    int global_bits = std::max(4, ceil_log2(B.proto_total(vd.protocol)));
    int biome_bits = std::max(1, ceil_log2(static_cast<int>(vd.biomes.size())));
    std::vector<int> bmap(biome_names.size());
    for (size_t i = 0; i < biome_names.size(); i++) {
        std::string n = biome_names[i];
        int id = vd.reg_id("biome/" + (n.rfind("minecraft:", 0) == 0 ? n.substr(10) : n));
        bmap[i] = id < 0 ? vd.biome_plains : id;
    }
    out.be32(static_cast<uint32_t>(c.x));
    out.be32(static_cast<uint32_t>(c.z));
    out.varint(3);
    const auto hm = heightmap_longs(L.height_mb), ws = heightmap_longs(L.height_ws);
    out.varint(5); out.varint(37); put_longs(out, hm);
    out.varint(1); out.varint(37); put_longs(out, ws);
    out.varint(4); out.varint(37); put_longs(out, hm);
    Wr data;
    std::vector<int> vals(4096), bv(64);
    for (int s = 0; s < kSections; s++) {
        const Section* sec = c.sec[s].get();
        int nonair = 0, fluid = 0;
        if (sec) {
            for (int i = 0; i < 4096; i++) {
                uint16_t b = sec->b[i];
                if (b) {
                    const BlockInfo& bi = B.info(b);
                    if (!bi.air) nonair++;
                    if (bi.fluid) fluid++;
                }
                vals[static_cast<size_t>(i)] = B.proto_state(vd.protocol, b);
            }
            for (int i = 0; i < 64; i++) bv[static_cast<size_t>(i)] = bmap[sec->biome[i] < bmap.size() ? sec->biome[i] : 0];
        } else {
            std::fill(vals.begin(), vals.end(), 0);
            for (int i = 0; i < 64; i++) bv[static_cast<size_t>(i)] = bmap[c.air_biomes[s][i] < bmap.size() ? c.air_biomes[s][i] : 0];
        }
        data.be16(static_cast<uint16_t>(nonair));
        data.be16(static_cast<uint16_t>(fluid));
        put_block_container(data, vals, global_bits);
        put_biome_container(data, bv, biome_bits);
    }
    out.varint(static_cast<int32_t>(data.b.size()));
    out.raw(data.b.data(), data.b.size());
    // block entities: those whose data the client shows get their NBT, the rest an empty compound (the client still needs the entry to draw chests, beds, ...)
    {
        static const char* shown[] = {"sign", "hanging_sign", "banner", "skull", "beacon", "mob_spawner", "command_block", "lectern", "campfire", "soul_campfire",
                                      "decorated_pot", "jukebox", "chiseled_bookshelf", "brushable_block", "end_gateway", "conduit", "enchanting_table", "bed", "shelf", "copper_golem_statue"};
        const Nbt* list = c.extra.get("block_entities");
        std::vector<std::vector<uint8_t>> ents;
        if (list && list->type == Nbt::List) {
            for (auto& be : list->list) {
                std::string id = be.gets("id");
                std::string shortid = id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
                int type = vd.reg_id("block_entity_type/" + shortid);
                if (type < 0) continue;
                int bx = static_cast<int>(be.geti("x")), by = static_cast<int>(be.geti("y")), bz = static_cast<int>(be.geti("z"));
                if ((bx >> 4) != c.x || (bz >> 4) != c.z || by < kMinY || by >= kMinY + kHeight) continue;
                Wr e;
                e.u8(static_cast<uint8_t>(((bx & 15) << 4) | (bz & 15)));
                e.be16(static_cast<uint16_t>(static_cast<int16_t>(by)));
                e.varint(type);
                bool visible = false;
                for (const char* s : shown) if (shortid == s || (strlen(s) > 4 && shortid.size() >= strlen(s) && shortid.compare(shortid.size() - strlen(s), strlen(s), s) == 0)) visible = true;
                Nbt tag = Nbt::compound();
                if (visible) for (auto& kv : be.map) if (kv.first != "id" && kv.first != "x" && kv.first != "y" && kv.first != "z") tag.map.push_back(kv);
                std::vector<uint8_t> nb;
                nbt_write(nb, tag, false);
                e.raw(nb.data(), nb.size());
                ents.push_back(std::move(e.b));
                if (ents.size() >= 4096) break;
            }
        }
        out.varint(static_cast<int32_t>(ents.size()));
        for (auto& e : ents) out.raw(e.data(), e.size());
    }
    // light
    const size_t S = kSections + 2;
    std::vector<bool> sky_mask(S), blk_mask(S), esky(S), eblk(S);
    std::vector<size_t> sky_idx, blk_idx;
    auto classify = [&](const std::vector<uint8_t>& arr, size_t i, std::vector<bool>& mask, std::vector<bool>& empty, std::vector<size_t>& idx) {
        const uint8_t* p = arr.data() + i * 2048;
        bool zero = true;
        for (size_t k = 0; k < 2048; k++) if (p[k]) { zero = false; break; }
        if (zero) empty[i] = true; else { mask[i] = true; idx.push_back(i); }
    };
    for (size_t i = 0; i < S; i++) { classify(L.sky, i, sky_mask, esky, sky_idx); classify(L.blk, i, blk_mask, eblk, blk_idx); }
    put_bitset(out, sky_mask);
    put_bitset(out, blk_mask);
    put_bitset(out, esky);
    put_bitset(out, eblk);
    out.varint(static_cast<int32_t>(sky_idx.size()));
    for (size_t i : sky_idx) { out.varint(2048); out.raw(L.sky.data() + i * 2048, 2048); }
    out.varint(static_cast<int32_t>(blk_idx.size()));
    for (size_t i : blk_idx) { out.varint(2048); out.raw(L.blk.data() + i * 2048, 2048); }
}

}  // namespace pn
