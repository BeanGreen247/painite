// Bedrock side of the shared game: spawn positions, entities, movement, block edits and chunks from the world.
#include "bchunk.hpp"
#include "skin.hpp"
#include "bedrock.hpp"
#include "dimension.hpp"
#include "game.hpp"
#include "log.hpp"
#include "perf.hpp"
#include "server.hpp"
#include "util.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cmath>

namespace pn {

static void put_varuint(std::vector<uint8_t>& o, uint64_t v) {
    while (v >= 0x80) { o.push_back(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
    o.push_back(static_cast<uint8_t>(v));
}
static uint32_t zz32(int32_t v) { return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31); }
static uint64_t zz64(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63); }
static void put_zz(std::vector<uint8_t>& o, int32_t v) { put_varuint(o, zz32(v)); }
static void put_f32(std::vector<uint8_t>& o, float f) { uint8_t b[4]; memcpy(b, &f, 4); o.insert(o.end(), b, b + 4); }
static void put_i32(std::vector<uint8_t>& o, int32_t v) { for (int i = 0; i < 4; i++) o.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i))); }
static void put_i64(std::vector<uint8_t>& o, int64_t v) { for (int i = 0; i < 8; i++) o.push_back(static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i))); }
static void put_s(std::vector<uint8_t>& o, const std::string& s) { put_varuint(o, s.size()); o.insert(o.end(), s.begin(), s.end()); }
static bool rd_varuint(const uint8_t*& p, const uint8_t* e, uint64_t& v) {
    v = 0;
    for (int s = 0; s < 70; s += 7) {
        if (p >= e) return false;
        uint8_t c = *p++;
        v |= static_cast<uint64_t>(c & 0x7f) << s;
        if (!(c & 0x80)) return true;
    }
    return false;
}
static int64_t unzz64(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }
static float rd_f32(const uint8_t*& p) { float f; memcpy(&f, p, 4); p += 4; return f; }

static int bedrock_mode(int javamode) { return javamode == 3 ? 6 : javamode; }
static constexpr float kEye = 1.62f;

BSession* Bedrock::by_id(uint64_t id) {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : it->second;
}

// ---- recorded packets patched for this player ----------------------------------------------------------

std::vector<uint8_t> Bedrock::patched_start_game(const BSession& b, const std::vector<uint8_t>& raw) const {
    const uint8_t* p = raw.data();
    const uint8_t* e = p + raw.size();
    std::vector<uint8_t> o;
    uint64_t v;
    if (!rd_varuint(p, e, v)) return raw;
    put_varuint(o, zz64(b.eid));          // unique entity id
    if (!rd_varuint(p, e, v)) return raw;
    put_varuint(o, static_cast<uint64_t>(b.eid));   // runtime entity id
    if (!rd_varuint(p, e, v)) return raw;
    put_zz(o, bedrock_mode(b.gamemode));
    if (e - p < 20) return raw;
    float oy = 0;
    memcpy(&oy, p + 4, 4);
    p += 12;
    put_f32(o, static_cast<float>(b.x));
    put_f32(o, oy);                       // the recorded placeholder height; the final Respawn sets the real one
    put_f32(o, static_cast<float>(b.z));
    float rot[2];
    memcpy(rot, p, 8);
    p += 8;
    put_f32(o, b.pitch);
    put_f32(o, b.yaw);
    (void)rot;
    // seed (8), biome type (2), biome name (string)
    if (e - p < 11) return raw;
    o.insert(o.end(), p, p + 10);
    p += 10;
    if (!rd_varuint(p, e, v) || static_cast<uint64_t>(e - p) < v) return raw;
    put_varuint(o, v);
    o.insert(o.end(), p, p + v);
    p += v;
    for (int i = 0; i < 3; i++) {          // dimension, generator, world game mode
        const uint8_t* s = p;
        if (!rd_varuint(p, e, v)) return raw;
        if (i == 0) put_zz(o, b.dim);        // the recorded value is the overworld
        else o.insert(o.end(), s, p);
    }
    if (p >= e) return raw;
    o.push_back(*p++);                     // hardcore
    {
        const uint8_t* s = p;
        if (!rd_varuint(p, e, v)) return raw;   // difficulty
        o.insert(o.end(), s, p);
    }
    for (int i = 0; i < 3; i++) if (!rd_varuint(p, e, v)) return raw;   // recorded spawn position, replaced
    put_zz(o, s_.world.spawn_x);
    put_zz(o, s_.world.spawn_y);
    put_zz(o, s_.world.spawn_z);
    o.insert(o.end(), p, e);
    return o;
}

std::vector<uint8_t> Bedrock::patched_respawn(const BSession& b, const std::vector<uint8_t>& raw) const {
    if (raw.size() < 14) return raw;
    std::vector<uint8_t> o;
    float y;
    memcpy(&y, raw.data() + 4, 4);
    bool ready = raw[12] == 1;
    put_f32(o, static_cast<float>(b.x));
    put_f32(o, ready ? static_cast<float>(b.y) + kEye : y);
    put_f32(o, static_cast<float>(b.z));
    o.push_back(raw[12]);
    put_varuint(o, static_cast<uint64_t>(b.eid));
    return o;
}

// PlayerList "add" entry: header, name and ids for this player, the given skin (or the default classic one), and the recorded tail.
std::vector<uint8_t> Bedrock::player_list_for(const BedrockData& data, const uint8_t uuid[16], int64_t unique_id, const std::string& name, const std::string& xuid,
                                              const std::vector<uint8_t>* skin) const {
    const auto& in = data.player_list_raw;
    const uint8_t* p = in.data() + 1;
    const uint8_t* e = in.data() + in.size();
    uint64_t v;
    if (!rd_varuint(p, e, v) || e - p < 16) return in;       // count
    std::vector<uint8_t> out(in.data(), p);                  // action, count
    out.insert(out.end(), uuid, uuid + 16);
    p += 16;
    if (!rd_varuint(p, e, v)) return in;                      // recorded unique id
    put_varuint(out, zz64(unique_id));
    for (int i = 0; i < 3; i++) {                             // name, xuid, platform chat id
        if (!rd_varuint(p, e, v) || static_cast<uint64_t>(e - p) < v) return in;
        p += v;
        put_s(out, i == 0 ? name : i == 1 ? xuid : std::string());
    }
    if (e - p < 4) return in;
    out.insert(out.end(), p, p + 4);                          // build platform
    p += 4;
    const uint8_t* skin_end = nullptr;
    if (!bedrock_skin_extent(p, e, skin_end)) return in;
    if (skin && !skin->empty()) out.insert(out.end(), skin->begin(), skin->end());
    else out.insert(out.end(), default_skin_.begin(), default_skin_.end());
    out.insert(out.end(), skin_end, e);                       // teacher, host, sub client, colour
    return out;
}

std::vector<uint8_t> Bedrock::patched_player_list(const BSession& b) const { return player_list_for(dat(b), b.uuid, b.eid, b.name, b.xuid, &b.skin); }

std::vector<uint8_t> Bedrock::player_list_entry(const BedrockData& data, const GIdentity& id, bool add) const {
    if (add) return player_list_for(data, id.uuid, id.eid, id.name, id.xuid, &id.bedrock_skin);
    std::vector<uint8_t> o;
    o.push_back(1);
    put_varuint(o, 1);
    o.insert(o.end(), id.uuid, id.uuid + 16);
    return o;
}

// ---- spawning and registration --------------------------------------------------------------------------

void Bedrock::begin_play(BSession& b) {
    if (b.gid) return;
    auto id = std::make_shared<GIdentity>();
    id->bedrock = true;
    id->worker = 0;
    id->conn = b.id;
    id->eid = b.eid;
    id->name = b.name;
    id->xuid = b.xuid;
    memcpy(id->uuid, b.uuid, 16);
    {
        char ip[64] = "?";
        int port = 0;
        const sockaddr_storage& ss = b.rak->addr;
        if (ss.ss_family == AF_INET6) { auto* a = reinterpret_cast<const sockaddr_in6*>(&ss); inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip); port = ntohs(a->sin6_port); if (strncmp(ip, "::ffff:", 7) == 0) memmove(ip, ip + 7, strlen(ip) - 6); }
        else if (ss.ss_family == AF_INET) { auto* a = reinterpret_cast<const sockaddr_in*>(&ss); inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip); port = ntohs(a->sin_port); }
        id->addr = std::string(ip) + ":" + std::to_string(port);
    }
    GPos gp;
    gp.x = b.x; gp.y = b.y; gp.z = b.z; gp.yaw = b.yaw; gp.pitch = b.pitch; gp.head = b.yaw;
    id->joined_ms = now_ms();
    id->ping_ms = static_cast<int>(b.rak->srtt_ms);
    b.ident = id;
    b.gid = s_.game.add_player(id, gp, b.gamemode, std::max(2, b.radius));
}

void Bedrock::send_time_to(BSession& b, int64_t day_time) {
    std::vector<uint8_t> pl;
    put_zz(pl, static_cast<int32_t>(day_time % 24000));
    send_packets(b, {{10, pl}});
}

void Bedrock::send_game_mode(BSession& b, int mode) {
    std::vector<uint8_t> pl;
    put_zz(pl, bedrock_mode(mode));
    send_packets(b, {{62, pl}});
}

// ---- Game events ----------------------------------------------------------------------------------------

void Bedrock::block_event(uint64_t conn, int x, int y, int z, uint16_t block) {
    BSession* b = by_id(conn);
    if (!b || b->rak->closed) return;
    std::vector<uint8_t> pl;
    put_zz(pl, x);
    put_zz(pl, y);
    put_zz(pl, z);
    put_varuint(pl, static_cast<uint32_t>(Blocks::get().bedrock_id(b->proto, block)));
    put_varuint(pl, 3);   // neighbours + network
    put_varuint(pl, 0);   // layer
    send_packets(*b, {{21, pl}});
    rak_->flush(*b->rak, now_ms());
}

void Bedrock::chat_to(uint64_t conn, const std::string& text) {
    BSession* b = by_id(conn);
    if (!b || b->st != BSession::St::Playing) return;
    send_text(*b, text);
    rak_->flush(*b->rak, now_ms());
}

void Bedrock::kick(uint64_t conn, const std::string& reason) {
    BSession* b = by_id(conn);
    if (b) disconnect(*b, reason);
}

void Bedrock::on_game_event(uint64_t conn, const GEvent& ev) {
    PN_PERF("event.bedrock");
    BSession* b = by_id(conn);
    if (!b || b->rak->closed || b->st != BSession::St::Playing) return;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> out;
    switch (ev.kind) {
        case GEvent::AddTab:
            if (ev.id->gid == b->gid) break;
            out.emplace_back(63, player_list_entry(dat(*b), *ev.id, true));
            break;
        case GEvent::RemoveTab:
            out.emplace_back(63, player_list_entry(dat(*b), *ev.id, false));
            break;
        case GEvent::Spawn: {
            if (ev.id->gid == b->gid) break;
            if (dat(*b).add_player_tpl.empty()) { static bool warned = false; if (!warned) { warned = true; warn("Bedrock data has no AddPlayer template (regenerate with tools/fetch_bedrock_data.py): other players are invisible to Bedrock clients"); } break; }
            const auto& t = dat(*b).add_player_tpl;
            const uint8_t* p = t.data() + 16;
            const uint8_t* e = t.data() + t.size();
            uint64_t v;
            // name, runtime id, platform chat id
            if (!rd_varuint(p, e, v) || static_cast<uint64_t>(e - p) < v) break;
            std::string tname(reinterpret_cast<const char*>(p), v);
            p += v;
            if (!rd_varuint(p, e, v)) break;       // runtime id
            if (!rd_varuint(p, e, v) || static_cast<uint64_t>(e - p) < v) break;
            p += v;
            p += 36 + 8;                           // position, velocity, rotation (3 x 3 floats) and the empty held item
            if (e - p < 2) break;
            if (!rd_varuint(p, e, v)) break;       // game mode
            const uint8_t* meta = p;
            // the tail starts 8 bytes before "permission, command permission, 2 layers": find the layer header (the two permission bytes depend on the recorded player)
            static const uint8_t pat[] = {0x02, 0x05, 0x00, 0x3f, 0x01};
            const uint8_t* tail = nullptr;
            for (const uint8_t* q = e - sizeof pat; q > meta + 10; q--) if (memcmp(q, pat, sizeof pat) == 0) { tail = q - 10; break; }
            if (!tail) break;
            std::vector<uint8_t> pl;
            pl.insert(pl.end(), ev.id->uuid, ev.id->uuid + 16);
            put_s(pl, ev.id->name);
            put_varuint(pl, static_cast<uint64_t>(ev.id->eid));
            put_s(pl, "");
            put_f32(pl, static_cast<float>(ev.pos.x));
            put_f32(pl, static_cast<float>(ev.pos.y));
            put_f32(pl, static_cast<float>(ev.pos.z));
            for (int i = 0; i < 3; i++) put_f32(pl, 0);
            put_f32(pl, ev.pos.pitch);
            put_f32(pl, ev.pos.yaw);
            put_f32(pl, ev.pos.head);
            pl.insert(pl.end(), 8, 0);
            put_zz(pl, 0);   // game mode: survival (the client shows nothing special for it)
            std::vector<uint8_t> md(meta, tail);
            md = replace_string_bytes(md, tname, ev.id->name);
            pl.insert(pl.end(), md.begin(), md.end());
            put_i64(pl, ev.id->eid);
            pl.insert(pl.end(), tail + 8, e);
            out.emplace_back(12, std::move(pl));
            break;
        }
        case GEvent::Despawn: {
            std::vector<uint8_t> pl;
            put_varuint(pl, zz64(ev.id->eid));
            out.emplace_back(14, std::move(pl));
            break;
        }
        case GEvent::Move:
        case GEvent::Teleport: {
            if (ev.id->gid == b->gid) {
                if (ev.kind == GEvent::Teleport && ev.value == 1) {   // server moved this player
                    b->x = ev.pos.x; b->y = ev.pos.y; b->z = ev.pos.z; b->yaw = ev.pos.yaw; b->pitch = ev.pos.pitch;
                    std::vector<uint8_t> pl;
                    put_varuint(pl, static_cast<uint64_t>(b->eid));
                    put_f32(pl, static_cast<float>(ev.pos.x)); put_f32(pl, static_cast<float>(ev.pos.y) + kEye); put_f32(pl, static_cast<float>(ev.pos.z));
                    put_f32(pl, ev.pos.pitch); put_f32(pl, ev.pos.yaw); put_f32(pl, ev.pos.yaw);
                    pl.push_back(2);      // teleport
                    pl.push_back(1);
                    put_varuint(pl, 0);
                    pl.push_back(1);      // teleport data follows
                    put_i32(pl, 3);       // cause: command
                    put_i32(pl, 0);
                    put_varuint(pl, 0);   // tick
                    out.emplace_back(19, std::move(pl));
                    int ncx = static_cast<int>(std::floor(b->x / 16)), ncz = static_cast<int>(std::floor(b->z / 16));
                    if (ncx != b->cx || ncz != b->cz) { b->cx = ncx; b->cz = ncz; send_npu(*b); queue_chunks(*b); }
                }
                break;
            }
            std::vector<uint8_t> pl;
            put_varuint(pl, static_cast<uint64_t>(ev.id->eid));
            put_f32(pl, static_cast<float>(ev.pos.x)); put_f32(pl, static_cast<float>(ev.pos.y) + kEye); put_f32(pl, static_cast<float>(ev.pos.z));
            put_f32(pl, ev.pos.pitch); put_f32(pl, ev.pos.yaw); put_f32(pl, ev.pos.head);
            pl.push_back(ev.kind == GEvent::Teleport ? 2 : 0);
            pl.push_back(ev.pos.ground ? 1 : 0);
            put_varuint(pl, 0);
            if (ev.kind == GEvent::Teleport) { pl.push_back(1); put_i32(pl, 0); put_i32(pl, 0); } else pl.push_back(0);
            put_varuint(pl, 0);
            out.emplace_back(19, std::move(pl));
            break;
        }
        case GEvent::Swing: {
            std::vector<uint8_t> pl;
            pl.push_back(1);   // swing arm
            put_varuint(pl, static_cast<uint64_t>(ev.id->eid));
            put_f32(pl, 0);
            pl.push_back(0);
            out.emplace_back(44, std::move(pl));
            break;
        }
        case GEvent::Block:
            block_event(conn, ev.bx, ev.by, ev.bz, ev.block);
            return;
        case GEvent::Time: send_time_to(*b, ev.t1); return;
        case GEvent::Gamemode:
            if (ev.id->gid == b->gid) { b->gamemode = ev.value; send_game_mode(*b, ev.value); }
            return;
        case GEvent::Dimension:
            if (ev.id->gid == b->gid) change_dimension(*b, ev.pos);
            return;
        case GEvent::Message: send_text(*b, ev.text); break;
        case GEvent::Kick: disconnect(*b, ev.text); return;
        default: break;
    }
    if (!out.empty()) {
        send_packets(*b, out);
        rak_->flush(*b->rak, now_ms());
    }
}

// ---- input ----------------------------------------------------------------------------------------------

void Bedrock::on_auth_input(BSession& b, const uint8_t* p, size_t n) {
    const uint8_t* e = p + n;
    if (n < 40) return;
    float pitch = rd_f32(p), yaw = rd_f32(p);
    float x = rd_f32(p), y = rd_f32(p), z = rd_f32(p);
    p += 8;   // move vector
    float head = rd_f32(p);
    (void)head;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::fabs(x) > 3e7f || std::fabs(z) > 3e7f || std::fabs(y) > 3e7f) return;
    // input flags: list of ordinals
    uint64_t cnt;
    if (!rd_varuint(p, e, cnt) || cnt > 80) return;
    bool sneaking = b.sneaking, item_interact = false, block_action = false;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t v;
        if (!rd_varuint(p, e, v)) return;
        int ord = static_cast<int>(unzz64(v));
        if (ord == 27) sneaking = true;        // start sneaking
        else if (ord == 28) sneaking = false;  // stop sneaking
        else if (ord == 34) item_interact = true;
        else if (ord == 35) block_action = true;
    }
    b.x = x; b.y = y - kEye; b.z = z; b.yaw = yaw; b.pitch = pitch; b.sneaking = sneaking;
    if (b.st == BSession::St::Playing && b.gid) {
        GPos gp;
        gp.x = b.x; gp.y = b.y; gp.z = b.z; gp.yaw = yaw; gp.pitch = pitch; gp.head = yaw; gp.sneaking = sneaking; gp.dim = b.dim;
        s_.game.update_pos(b.gid, gp);
    }
    on_move(b, x, z);
    if (!(item_interact || block_action) || b.st != BSession::St::Playing) return;
    // input mode, play mode, interaction model, interact rotation, tick, delta
    uint64_t v;
    if (!rd_varuint(p, e, v) || !rd_varuint(p, e, v) || !rd_varuint(p, e, v)) return;
    if (e - p < 8) return;
    p += 8;
    if (!rd_varuint(p, e, v)) return;
    if (e - p < 12) return;
    p += 12;
    // optional parts, each behind a presence byte: transaction, item stack request, block actions
    if (p >= e) return;
    bool has_tx = *p++ != 0;
    if (has_tx) return;   // block placement through item transactions is handled by the inventory code once it exists
    if (p >= e) return;
    bool has_isr = *p++ != 0;
    if (has_isr) return;
    if (p >= e || !*p++) return;
    uint64_t acts;
    if (!rd_varuint(p, e, acts) || acts > 64) return;
    for (uint64_t i = 0; i < acts; i++) {
        uint64_t a;
        if (!rd_varuint(p, e, a)) return;
        int action = static_cast<int>(unzz64(a));
        int c[3];
        for (int k = 0; k < 3; k++) { uint64_t t; if (!rd_varuint(p, e, t)) return; c[k] = static_cast<int>(unzz64(t)); }
        uint64_t face;
        if (!rd_varuint(p, e, face)) return;
        // 13 creative destroy, 26 predicted break (client finished a break it computed itself)
        bool take = action == 13 || (action == 26 && b.gamemode != 1 && b.gamemode != 3 && b.gamemode != 2);
        if (!take) continue;
        int bx = c[0], by = c[1], bz = c[2];
        if (by < dim_info(b.dim).min_y || by >= dim_info(b.dim).min_y + dim_info(b.dim).height) continue;
        double dx = bx + 0.5 - b.x, dy = by + 0.5 - (b.y + 1.62), dz = bz + 0.5 - b.z;
        World& W = s_.dim(b.dim);
        uint16_t cur = W.get_block(bx, by, bz);
        const BlockInfo& bi = Blocks::get().info(cur);
        bool protect = b.dim == 0 && s_.cfg.spawn_protection > 0 && std::max(std::abs(bx - s_.world.spawn_x), std::abs(bz - s_.world.spawn_z)) <= s_.cfg.spawn_protection &&
                       s_.game.op_level(b.gid) == 0;
        if (dx * dx + dy * dy + dz * dz <= 49.0 && !bi.air && bi.name != "minecraft:bedrock" && !protect && W.set_block(bx, by, bz, 0)) s_.game.block_changed(b.dim, bx, by, bz, 0);
        else block_event(b.id, bx, by, bz, W.get_block(bx, by, bz));
    }
}

void Bedrock::on_command_request(BSession& b, const uint8_t* p, size_t n) {
    if (b.st != BSession::St::Playing || !b.gid) return;
    const uint8_t* e = p + n;
    uint64_t len;
    if (!rd_varuint(p, e, len) || len > 512 || static_cast<uint64_t>(e - p) < len) return;
    std::string cmd(reinterpret_cast<const char*>(p), len);
    info(b.name + " issued server command: " + cmd);
    s_.game.command(b.gid, cmd);
}

// ---- dimension change -------------------------------------------------------------------------------------

// ChangeDimension (61) moves the client to the new dimension; the chunks of that dimension follow like at login.
void Bedrock::change_dimension(BSession& b, const GPos& p) {
    b.dim = p.dim;
    b.x = p.x; b.y = p.y; b.z = p.z; b.yaw = p.yaw; b.pitch = p.pitch;
    b.sent.clear();
    b.pending.clear();
    b.cx = static_cast<int>(std::floor(b.x / 16));
    b.cz = static_cast<int>(std::floor(b.z / 16));
    std::vector<uint8_t> pl;
    put_zz(pl, p.dim);
    put_f32(pl, static_cast<float>(p.x)); put_f32(pl, static_cast<float>(p.y) + kEye); put_f32(pl, static_cast<float>(p.z));
    pl.push_back(0);   // not a respawn
    pl.push_back(0);   // no loading screen id
    send_packets(b, {{61, pl}});
    send_npu(b);
    queue_chunks(b);
    pump_chunks(b);
    rak_->flush(*b.rak, now_ms());
}

// ---- chunks from the world ------------------------------------------------------------------------------

const std::vector<uint8_t>& Bedrock::level_chunk_tail(int dim, int x, int z) {
    int64_t key = ((static_cast<int64_t>(x) << 32) | static_cast<uint32_t>(z)) ^ (static_cast<int64_t>(dim) << 58);
    auto ch = s_.dim(dim).chunk(x, z);
    uint32_t ver = ch->version.load();
    auto& slot = tail_cache_[key];
    if (slot.ver != ver || slot.tail.empty()) {
        tail_cache_bytes_ -= slot.tail.size();
        encode_bedrock_levelchunk(*ch, slot.tail, dim);
        slot.ver = ver;
        tail_cache_bytes_ += slot.tail.size();
        if (tail_cache_bytes_ > 16u * 1024 * 1024) {
            CachedTail keep = std::move(slot);
            tail_cache_.clear();
            auto& fresh = tail_cache_[key];
            fresh = std::move(keep);
            tail_cache_bytes_ = fresh.tail.size();
            return fresh.tail;
        }
    }
    return slot.tail;
}

}  // namespace pn
