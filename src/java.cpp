// Java Edition protocol: framing, handshake, status, login, configuration replay and a flat-world play loop.
#include "server.hpp"
#include "log.hpp"
#include "util.hpp"
#include "hash.hpp"
#include "jchunk.hpp"
#include "perf.hpp"
#include "skin.hpp"
#include <algorithm>
#include <cmath>
#include <openssl/evp.h>
#include <random>
#include <zlib.h>

namespace pn {

static std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            default:
                if (ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
                else o += static_cast<char>(ch);
        }
    }
    return o;
}

std::string uuid_str(const uint8_t u[16]) {
    char b[40];
    snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return b;
}

// Builds [length][data length][zlib or raw] (or plain [length] when threshold < 0).
static void frame_into(std::vector<uint8_t>& out, const uint8_t* body, size_t n, int threshold, int level) {
    Wr hdr;
    if (threshold < 0) {
        hdr.varint(static_cast<int32_t>(n));
        out.insert(out.end(), hdr.b.begin(), hdr.b.end());
        out.insert(out.end(), body, body + n);
        return;
    }
    if (static_cast<int>(n) < threshold) {
        Wr d;
        d.varint(0);
        hdr.varint(static_cast<int32_t>(n + d.b.size()));
        out.insert(out.end(), hdr.b.begin(), hdr.b.end());
        out.insert(out.end(), d.b.begin(), d.b.end());
        out.insert(out.end(), body, body + n);
        return;
    }
    uLongf cap = compressBound(static_cast<uLong>(n));
    std::vector<uint8_t> z(cap);
    if (compress2(z.data(), &cap, body, static_cast<uLong>(n), level) != Z_OK) cap = 0;
    Wr d;
    d.varint(static_cast<int32_t>(n));
    hdr.varint(static_cast<int32_t>(d.b.size() + cap));
    out.insert(out.end(), hdr.b.begin(), hdr.b.end());
    out.insert(out.end(), d.b.begin(), d.b.end());
    out.insert(out.end(), z.begin(), z.begin() + static_cast<long>(cap));
}

void Worker::append_out(JConn& c, const uint8_t* p, size_t n) {
    size_t off = c.out.size();
    c.out.insert(c.out.end(), p, p + n);
    if (c.enc) {
        int outl = 0;
        EVP_EncryptUpdate(c.enc, c.out.data() + off, &outl, c.out.data() + off, static_cast<int>(n));
    }
}

void Worker::decrypt_new(JConn& c) {
    if (!c.dec || c.dec_done >= c.in.size()) return;
    int outl = 0;
    EVP_DecryptUpdate(c.dec, c.in.data() + c.dec_done, &outl, c.in.data() + c.dec_done, static_cast<int>(c.in.size() - c.dec_done));
    c.dec_done = c.in.size();
}

JConn::~JConn() {
    if (enc) EVP_CIPHER_CTX_free(enc);
    if (dec) EVP_CIPHER_CTX_free(dec);
}

void Worker::send_raw_frame(JConn& c, const uint8_t* body, size_t n) {
    if (c.dead) return;
    if (c.enc) {
        std::vector<uint8_t> tmp;
        frame_into(tmp, body, n, c.threshold, s_.cfg.compression_level);
        append_out(c, tmp.data(), tmp.size());
    } else {
        frame_into(c.out, body, n, c.threshold, s_.cfg.compression_level);
    }
    if (c.out.size() - c.outpos > 16u * 1024 * 1024) { c.dead = true; c.out.clear(); c.outpos = 0; }
}

void Worker::send(JConn& c, const std::vector<uint8_t>& body) { send_raw_frame(c, body.data(), body.size()); }

void Worker::kick(JConn& c, const std::string& reason) {
    if (c.dead) return;
    Wr w;
    if (c.st == St::Login) {
        int id = c.vd ? c.vd->id.lg_disconnect : 0;
        w.varint(id);
        w.str("{\"text\":\"" + json_escape(reason) + "\"}");
        send(c, w.b);
    } else if (c.st == St::Config && c.vd) {
        w.varint(c.vd->id.cf_disconnect);
        w.text_nbt(reason);
        send(c, w.b);
    } else if (c.st == St::Play && c.vd) {
        w.varint(c.vd->id.pl_disconnect);
        w.text_nbt(reason);
        send(c, w.b);
    }
    c.dead = true;
    if (c.counted) info(c.name + " lost connection: " + reason);
}

void Worker::parse_frames(JConn& c) {
    while (!c.dead) {
        if (c.dec && !c.dec_pending) decrypt_new(c);
        size_t avail = c.in.size() - c.inpos;
        int32_t len = 0;
        int k = read_varint(c.in.data() + c.inpos, avail, len, 3);
        if (k == 0) break;
        if (k < 0 || len <= 0 || len > 2097151) { c.dead = true; c.out.clear(); c.outpos = 0; return; }
        if (c.st != St::Play && len > 4096) { c.dead = true; c.out.clear(); c.outpos = 0; return; }  // pre-login packets are tiny
        if (avail < static_cast<size_t>(k) + static_cast<size_t>(len)) break;
        const uint8_t* body = c.in.data() + c.inpos + k;
        size_t blen = static_cast<size_t>(len);
        std::vector<uint8_t> inflated;
        if (c.threshold >= 0) {
            int32_t dl = 0;
            int kk = read_varint(body, blen, dl, 3);
            if (kk <= 0 || dl < 0 || dl > 8 * 1024 * 1024) { c.dead = true; return; }
            body += kk;
            blen -= static_cast<size_t>(kk);
            if (dl != 0) {
                inflated.resize(static_cast<size_t>(dl));
                uLongf outl = static_cast<uLongf>(dl);
                if (uncompress(inflated.data(), &outl, body, static_cast<uLong>(blen)) != Z_OK || outl != static_cast<uLongf>(dl)) { c.dead = true; return; }
                body = inflated.data();
                blen = inflated.size();
            }
        }
        handle_packet(c, body, blen);
        c.inpos += static_cast<size_t>(k) + static_cast<size_t>(len);
        if (c.dec_pending) { c.dec_pending = false; c.dec_done = c.inpos; }  // everything after the key packet is ciphertext
    }
    if (c.inpos == c.in.size()) { c.in.clear(); c.inpos = 0; c.dec_done = 0; if (c.in.capacity() > 64 * 1024) std::vector<uint8_t>().swap(c.in); }
    else if (c.inpos > 32 * 1024) {
        c.in.erase(c.in.begin(), c.in.begin() + static_cast<long>(c.inpos));
        c.dec_done = c.dec_done > c.inpos ? c.dec_done - c.inpos : 0;
        c.inpos = 0;
    }
}

void Worker::handle_packet(JConn& c, const uint8_t* p, size_t n) {
    Rd r(p, n);
    int pid = r.varint();
    if (r.bad) { c.dead = true; return; }
    switch (c.st) {
        case St::Handshake: on_handshake(c, r, pid); break;
        case St::Status: on_status(c, r, pid); break;
        case St::Login: on_login(c, r, pid); break;
        case St::Config: on_config(c, r, pid); break;
        case St::Play: on_play(c, r, pid); break;
    }
}

void Worker::on_handshake(JConn& c, Rd& r, int pid) {
    if (pid != 0) { c.dead = true; return; }
    c.proto = r.varint();
    r.str(255);
    r.be16();
    int intent = r.varint();
    if (r.bad) { c.dead = true; return; }
    c.vd = s_.find_data(c.proto);
    if (intent == 1) c.st = St::Status;
    else if (intent == 2 || intent == 3) c.st = St::Login;
    else c.dead = true;
}

void Worker::send_server_status(JConn& c) {
    const VanillaData* vd = c.vd ? c.vd : (s_.data.empty() ? nullptr : &s_.data.back());
    std::string j = "{\"version\":{\"name\":\"Painite " + std::string(vd ? vd->version : "?") + "\",\"protocol\":" +
                    std::to_string(c.vd ? c.proto : (vd ? vd->protocol : 0)) + "},\"players\":{\"max\":" + std::to_string(s_.cfg.max_players) +
                    ",\"online\":" + std::to_string(s_.online.load()) + "},\"description\":{\"text\":\"" + json_escape(s_.cfg.motd) +
                    "\"},\"enforcesSecureChat\":false";
    if (auto fav = s_.favicon_uri()) j += ",\"favicon\":\"" + *fav + "\"";
    j += "}";
    Wr w;
    w.varint(vd ? vd->id.st_status_response : 0);
    w.str(j);
    send(c, w.b);
}

void Worker::on_status(JConn& c, Rd& r, int pid) {
    if (pid == 0) {
        send_server_status(c);
    } else if (pid == 1) {
        uint64_t v = r.be64();
        Wr w;
        w.varint(1);
        w.be64(v);
        send(c, w.b);
        c.dead = true;  // closed once flushed
    } else {
        c.dead = true;
    }
}

void Worker::on_login(JConn& c, Rd& r, int pid) {
    const auto& id = c.vd->id;
    if (c.auth_pending) return;  // nothing is accepted while the session server is being asked
    if (pid == id.lg_encrypt_resp) {
        if (c.awaiting_key) on_encryption_response(c, r);
        else c.dead = true;
        return;
    }
    if (pid == id.lg_acknowledged) {
        if (c.vd && c.reserved && !c.awaiting_key) start_config(c);
        return;
    }
    if (pid != id.lg_hello || c.reserved || c.awaiting_key || !c.name.empty()) return;
    std::string name = r.str(16);
    uint8_t uuid_in[16];
    for (auto& b : uuid_in) b = r.u8();
    if (r.bad || name.empty() || name.size() > 16) { c.dead = true; return; }
    for (unsigned char ch : name)
        if (!(isalnum(ch) || ch == '_')) { c.dead = true; return; }
    c.name = name;
    if (s_.cfg.online_mode) { begin_online_login(c); return; }
    // offline ("cracked") mode: same UUID scheme as Spigot/Paper, so player data stays compatible
    offline_uuid(name, c.uuid);
    finish_login(c);
}

void Worker::begin_online_login(JConn& c) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    uint64_t t = rng();
    memcpy(c.token, &t, 4);
    Wr w;
    w.varint(c.vd->id.lg_encrypt_req);
    w.str("");
    w.varint(static_cast<int32_t>(s_.auth.public_key_der().size()));
    w.raw(s_.auth.public_key_der().data(), s_.auth.public_key_der().size());
    w.varint(4);
    w.raw(c.token, 4);
    w.boolean(true);
    send(c, w.b);
    c.awaiting_key = true;
}

void Worker::on_encryption_response(JConn& c, Rd& r) {
    int ls = r.varint();
    if (r.bad || ls <= 0 || ls > 256 || static_cast<size_t>(ls) > r.left()) { c.dead = true; return; }
    std::vector<uint8_t> es(r.p, r.p + ls);
    r.skip(static_cast<size_t>(ls));
    int lt = r.varint();
    if (r.bad || lt <= 0 || lt > 256 || static_cast<size_t>(lt) > r.left()) { c.dead = true; return; }
    std::vector<uint8_t> et(r.p, r.p + lt);
    std::vector<uint8_t> secret, token;
    if (!s_.auth.rsa_decrypt(es.data(), es.size(), secret) || !s_.auth.rsa_decrypt(et.data(), et.size(), token) || secret.size() != 16 ||
        token.size() != 4 || memcmp(token.data(), c.token, 4) != 0) {
        kick(c, "Protocol error");
        return;
    }
    c.awaiting_key = false;
    c.enc = EVP_CIPHER_CTX_new();
    c.dec = EVP_CIPHER_CTX_new();
    if (EVP_EncryptInit_ex(c.enc, EVP_aes_128_cfb8(), nullptr, secret.data(), secret.data()) != 1 ||
        EVP_DecryptInit_ex(c.dec, EVP_aes_128_cfb8(), nullptr, secret.data(), secret.data()) != 1) {
        c.dead = true;
        return;
    }
    c.dec_pending = true;  // bytes after this packet are encrypted
    c.auth_pending = true;
    AuthRequest rq;
    rq.conn_id = c.id;
    rq.worker_id = id_;
    rq.username = c.name;
    rq.server_hash = s_.auth.server_hash(secret.data(), secret.size());
    if (s_.auth.prevent_proxy()) rq.ip = c.addr.substr(0, c.addr.rfind(':'));
    s_.auth.submit(std::move(rq));
}

void Worker::on_auth_result(uint64_t conn_id, AuthResult& res) {
    JConn* c = nullptr;
    for (auto& kv : conns_) if (kv.second->id == conn_id) { c = kv.second.get(); break; }
    if (!c || c->dead) return;
    c->auth_pending = false;
    if (!res.ok) {
        warn("Failed to verify username for " + c->name + ": " + res.error);
        kick(*c, "Failed to verify username!");
        return;
    }
    c->name = res.name;
    for (int i = 0; i < 16; i++) c->uuid[i] = static_cast<uint8_t>(strtoul(res.uuid_hex.substr(static_cast<size_t>(i) * 2, 2).c_str(), nullptr, 16));
    c->props = std::move(res.props);
    c->skin_rgba = std::move(res.skin_rgba);
    c->skin_slim = res.skin_slim;
    finish_login(*c);
    if (!c->out.empty()) flush(*c);
}

void Worker::finish_login(JConn& c) {
    if (!c.vd) {
        std::string sup;
        for (auto& d : s_.data) sup += (sup.empty() ? "" : ", ") + d.version;
        kick(c, "Unsupported Minecraft version. Painite supports: " + sup);
        return;
    }
    std::string ustr = uuid_string(c.uuid), why;
    if (s_.lists.banned(c.name, ustr, &why)) { kick(c, "You are banned from this server.\nReason: " + (why.empty() ? std::string("Banned by an operator.") : why)); return; }
    if (s_.lists.ip_banned(c.addr.substr(0, c.addr.rfind(':')), &why)) { kick(c, "Your IP address is banned from this server.\nReason: " + (why.empty() ? std::string("Banned by an operator.") : why)); return; }
    if (s_.cfg.white_list && !s_.whitelisted(c.name, ustr)) { kick(c, "You are not white-listed on this server!"); return; }
    if (s_.online.load() >= s_.cfg.max_players && s_.lists.op_level(c.name, ustr) == 0) { kick(c, "The server is full!"); return; }
    if (!s_.reserve_name(c.name)) { kick(c, "A player with that name is already online."); return; }
    c.reserved = true;
    info("UUID of player " + c.name + " is " + uuid_str(c.uuid));

    if (s_.cfg.profile != "lan" && s_.cfg.compression_threshold >= 0 && !(s_.cfg.profile == "auto" && s_.local_peer(c.addr))) {
        Wr w;
        w.varint(c.vd->id.lg_compression);
        w.varint(s_.cfg.compression_threshold);
        send(c, w.b);
        c.threshold = s_.cfg.compression_threshold;
    }
    Wr w;
    w.varint(c.vd->id.lg_finished);
    w.raw(c.uuid, 16);
    w.str(c.name);
    w.varint(static_cast<int32_t>(c.props.size()));
    for (auto& p : c.props) {
        w.str(p.name);
        w.str(p.value);
        w.boolean(!p.signature.empty());
        if (!p.signature.empty()) w.str(p.signature);
    }
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    uint64_t sid[2] = {rng(), rng()};
    w.raw(sid, 16);  // session id
    send(c, w.b);
}

void Worker::start_config(JConn& c) {
    c.st = St::Config;
    const VanillaData& vd = *c.vd;
    Wr w;
    w.varint(vd.id.cf_custom_payload);
    w.str("minecraft:brand");
    w.str("Painite");
    send(c, w.b);
    for (size_t i = 0; i <= vd.index_known_packs && i < vd.config_packets.size(); i++) send(c, vd.config_packets[i]);
    c.known_packs_sent = true;
}

void Worker::on_config(JConn& c, Rd& r, int pid) {
    const auto& id = c.vd->id;
    if (pid == id.cfs_client_information) {
        r.str(16);
        int vdist = r.u8();
        r.varint();
        r.boolean();
        uint8_t parts = r.u8();
        if (!r.bad) c.skin_parts = parts;
        if (!r.bad) c.radius = std::max(2, std::min(vdist, s_.cfg.view_distance));
    } else if (pid == id.cfs_known_packs) {
        for (size_t i = c.vd->index_known_packs + 1; i < c.vd->config_packets.size(); i++) send(c, c.vd->config_packets[i]);
    } else if (pid == id.cfs_finish) {
        start_play(c);
    }
}

void Worker::send_chat(JConn& c, const std::string& text) {
    Wr w;
    w.varint(c.vd->id.pl_system_chat);
    std::string t = text.substr(0, 256);
    for (auto& ch : t) if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    w.text_nbt(t);
    w.boolean(false);
    send(c, w.b);
}

uint64_t pack_block_pos(int x, int y, int z) {
    return ((static_cast<uint64_t>(x) & 0x3FFFFFF) << 38) | ((static_cast<uint64_t>(z) & 0x3FFFFFF) << 12) | (static_cast<uint64_t>(y) & 0xFFF);
}

void Worker::choose_spawn(JConn& c) {
    auto sp = s_.world.pick_spawn(uuid_str(c.uuid), s_.cfg.gamemode);
    c.x = sp.x; c.y = sp.y; c.z = sp.z; c.yaw = sp.yaw; c.pitch = sp.pitch; c.gamemode = sp.gamemode;
}

static int64_t pack_chunk(int x, int z) { return (static_cast<int64_t>(x) << 32) | static_cast<uint32_t>(z); }

void Worker::start_play(JConn& c) {
    c.st = St::Play;
    const VanillaData& vd = *c.vd;
    const auto& id = vd.id;
    c.radius = std::min(c.radius, s_.cfg.view_distance);
    c.entity_id = s_.next_entity++;
    c.counted = true;
    s_.online++;
    s_.register_player(c.name);
    c.last_ka_sent = now_ms();
    c.last_ka_ack = c.last_ka_sent;
    choose_spawn(c);

    Wr w;  // Login (play)
    w.varint(id.pl_login);
    w.be32(static_cast<uint32_t>(c.entity_id));
    w.boolean(s_.cfg.hardcore);
    w.varint(3);
    w.str("minecraft:overworld");
    w.str("minecraft:the_nether");
    w.str("minecraft:the_end");
    w.varint(s_.cfg.max_players);
    w.varint(c.radius);
    w.varint(std::max(2, std::min(s_.cfg.simulation_distance, c.radius)));
    w.boolean(false);
    w.boolean(true);
    w.boolean(false);
    w.varint(vd.dim_type_overworld);
    w.str("minecraft:overworld");
    w.be64(0x5a17c0de5a17c0deull);
    if (vd.protocol >= 777) {
        w.varint(c.gamemode);
        w.varint(0);  // previous game mode: 0 = undefined
    } else {
        w.u8(static_cast<uint8_t>(c.gamemode));
        w.u8(0xff);   // previous game mode: -1 = undefined (checked against recorded samples by tests/check_layouts.py)
    }
    w.boolean(false);
    w.boolean(true);   // flat
    w.boolean(false);
    w.varint(0);
    w.varint(-63);     // sea level of the flat world
    w.boolean(false);
    w.boolean(false);
    send(c, w.b);

    Wr ab;
    ab.varint(id.pl_abilities);
    int gm = c.gamemode;
    ab.u8(gm == 1 ? 0x0d : gm == 3 ? 0x0f : 0x00);
    ab.f32(0.05f);
    ab.f32(0.1f);
    send(c, ab.b);

    Wr cm;
    cm.varint(id.pl_commands);
    cm.varint(1);
    cm.u8(0);
    cm.varint(0);
    cm.varint(0);
    send(c, cm.b);

    Wr pos;
    pos.varint(id.pl_position);
    pos.varint(1);
    pos.f64(c.x); pos.f64(c.y); pos.f64(c.z);
    pos.f64(0); pos.f64(0); pos.f64(0);
    pos.f32(c.yaw); pos.f32(c.pitch);
    pos.be32(0);
    send(c, pos.b);

    Wr sp;
    sp.varint(id.pl_spawn);
    sp.str("minecraft:overworld");
    sp.be64(pack_block_pos(s_.world.spawn_x, s_.world.spawn_y, s_.world.spawn_z));
    sp.f32(0); sp.f32(0);
    send(c, sp.b);

    Wr ge;
    ge.varint(id.pl_game_event);
    ge.u8(13);
    ge.f32(0);
    send(c, ge.b);

    send_time(c, s_.game.game_time(), s_.game.day_time());
    if (int dp = vd.packet("play/c/change_difficulty"); dp >= 0) {
        Wr df;
        df.varint(dp);
        df.u8(static_cast<uint8_t>(s_.cfg.difficulty >= 0 ? s_.cfg.difficulty : s_.world.difficulty));
        df.boolean(false);
        send(c, df.b);
    }

    Wr pi;  // own tab-list entry
    pi.varint(id.pl_player_info);
    pi.u8(0xff);
    pi.varint(1);
    pi.raw(c.uuid, 16);
    pi.str(c.name);
    pi.varint(0);
    pi.boolean(false);
    pi.varint(gm);
    pi.boolean(true);
    pi.varint(0);
    pi.boolean(false);
    pi.varint(0);
    pi.boolean(false);
    send(c, pi.b);

    char loc[96];
    snprintf(loc, sizeof loc, "([minecraft:overworld]%.1f, %.1f, %.1f)", c.x, c.y, c.z);
    info(c.name + " joined the game");
    info(c.name + "[/" + c.addr + "] logged in with entity id " + std::to_string(c.entity_id) + " at " + loc);
    s_.broadcast(c.name + " joined the game");

    c.cx = static_cast<int>(std::floor(c.x / 16));
    c.cz = static_cast<int>(std::floor(c.z / 16));
    Wr ce;
    ce.varint(id.pl_center);
    ce.varint(c.cx);
    ce.varint(c.cz);
    send(c, ce.b);
    rebuild_pending(c);
    pump_chunks(c);

    auto ident = std::make_shared<GIdentity>();
    ident->worker = id_;
    ident->conn = c.id;
    ident->eid = c.entity_id;
    ident->name = c.name;
    memcpy(ident->uuid, c.uuid, 16);
    ident->props = c.props;
    {
        bool alex = (c.uuid[15] ^ c.uuid[7]) & 1;
        if (c.skin_rgba.size() == 64 * 64 * 4) ident->bedrock_skin = bedrock_classic_skin(c.skin_rgba, c.skin_slim, "painite-java-" + uuid_str(c.uuid));
        else ident->bedrock_skin = bedrock_classic_skin(default_skin_rgba(alex), alex, alex ? "painite-alex" : "painite-steve");
    }
    ident->addr = c.addr;
    GPos gp;
    gp.x = c.x; gp.y = c.y; gp.z = c.z; gp.yaw = c.yaw; gp.pitch = c.pitch; gp.head = c.yaw; gp.skin_parts = c.skin_parts;
    c.gid = s_.game.add_player(ident, gp, c.gamemode, c.radius);
    send_command_tree(c);
}

void Worker::rebuild_pending(JConn& c) {
    c.pending.clear();
    for (int dx = -c.radius; dx <= c.radius; dx++)
        for (int dz = -c.radius; dz <= c.radius; dz++) {
            int x = c.cx + dx, z = c.cz + dz;
            if (!c.sent.count(pack_chunk(x, z))) c.pending.emplace_back(x, z);
        }
    int cx = c.cx, cz = c.cz;
    std::sort(c.pending.begin(), c.pending.end(), [&](const auto& a, const auto& b) {
        long da = static_cast<long>(a.first - cx) * (a.first - cx) + static_cast<long>(a.second - cz) * (a.second - cz);
        long db = static_cast<long>(b.first - cx) * (b.first - cx) + static_cast<long>(b.second - cz) * (b.second - cz);
        return da > db;  // nearest at the back
    });
}

void Worker::send_chunk(JConn& c, int x, int z) {
    const auto& id = c.vd->id;
    auto ch = s_.world.chunk(x, z);
    uint32_t ver = ch->version.load();
    int64_t key = pack_chunk(x, z) ^ (static_cast<int64_t>(c.vd->protocol) << 52) ^ (c.threshold < 0 ? (1ll << 51) : 0);
    auto it = chunk_cache_.find(key);
    if (it != chunk_cache_.end() && it->second.ver != ver) {
        chunk_cache_bytes_ -= it->second.f.size();
        s_.mem_sub(static_cast<int64_t>(it->second.f.size()));
        chunk_cache_.erase(it);
        it = chunk_cache_.end();
    }
    if (it == chunk_cache_.end()) {
        Wr w;
        w.varint(id.pl_chunk);
        { PN_PERF("chunk.encode_java"); encode_java_chunk(*ch, *c.vd, s_.world.biome_names, w); }
        ver = ch->version.load();
        std::vector<uint8_t> f;
        frame_into(f, w.b.data(), w.b.size(), c.threshold, s_.cfg.compression_level);
        size_t cap = static_cast<size_t>(s_.cfg.chunk_cache_mb) * 1024 * 1024;
        if (chunk_cache_bytes_ + f.size() > cap) {
            for (auto& kv : chunk_cache_) s_.mem_sub(static_cast<int64_t>(kv.second.f.size()));
            chunk_cache_.clear();
            chunk_cache_bytes_ = 0;
        }
        if (s_.mem_add(static_cast<int64_t>(f.size()))) {
            chunk_cache_bytes_ += f.size();
            it = chunk_cache_.emplace(key, CachedChunk{ver, std::move(f)}).first;
        } else {
            append_out(c, f.data(), f.size());
            return;
        }
    }
    append_out(c, it->second.f.data(), it->second.f.size());
}

void Worker::pump_chunks(JConn& c) {
    if (c.pending.empty() || c.dead) return;
    size_t cap = static_cast<size_t>(s_.cfg.backlog_cap_kb) * 1024;
    if (c.batches_out >= 3 || c.out.size() - c.outpos > cap) return;
    const auto& id = c.vd->id;
    int n = std::max(1, std::min(32, static_cast<int>(std::ceil(c.chunk_rate))));
    n = std::min<int>(n, static_cast<int>(c.pending.size()));
    Wr st;
    st.varint(id.pl_batch_start);
    send(c, st.b);
    for (int i = 0; i < n; i++) {
        auto [x, z] = c.pending.back();
        c.pending.pop_back();
        send_chunk(c, x, z);
        c.sent.insert(pack_chunk(x, z));
    }
    Wr fin;
    fin.varint(id.pl_batch_finish);
    fin.varint(n);
    send(c, fin.b);
    c.batches_out++;
}

void Worker::on_move(JConn& c, double nx, double ny, double nz, float yaw, float pitch, bool has_pos, bool has_rot, bool ground) {
    if (has_pos) {
        if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz) || std::fabs(nx) > 3e7 || std::fabs(nz) > 3e7 || std::fabs(ny) > 3e7) { kick(c, "Invalid move player packet received"); return; }
    } else { nx = c.x; ny = c.y; nz = c.z; }
    if (has_rot) {
        if (!std::isfinite(yaw) || !std::isfinite(pitch)) { kick(c, "Invalid move player packet received"); return; }
        c.yaw = yaw;
        c.pitch = std::max(-90.0f, std::min(90.0f, pitch));
    }
    c.x = nx;
    c.y = ny;
    c.z = nz;
    c.ground = ground;
    if (c.gid) {
        GPos gp;
        gp.x = c.x; gp.y = c.y; gp.z = c.z; gp.yaw = c.yaw; gp.pitch = c.pitch; gp.head = c.yaw; gp.ground = ground; gp.sneaking = c.sneaking; gp.skin_parts = c.skin_parts;
        s_.game.update_pos(c.gid, gp);
    }
    int ncx = static_cast<int>(std::floor(nx / 16)), ncz = static_cast<int>(std::floor(nz / 16));
    if (ncx == c.cx && ncz == c.cz) return;
    c.cx = ncx;
    c.cz = ncz;
    const auto& id = c.vd->id;
    Wr ce;
    ce.varint(id.pl_center);
    ce.varint(c.cx);
    ce.varint(c.cz);
    send(c, ce.b);
    for (auto it = c.sent.begin(); it != c.sent.end();) {
        int x = static_cast<int>(*it >> 32), z = static_cast<int>(static_cast<int32_t>(*it & 0xffffffff));
        if (std::abs(x - c.cx) > c.radius || std::abs(z - c.cz) > c.radius) {
            Wr f;
            f.varint(id.pl_forget);
            f.be32(static_cast<uint32_t>(z));
            f.be32(static_cast<uint32_t>(x));
            send(c, f.b);
            it = c.sent.erase(it);
        } else ++it;
    }
    rebuild_pending(c);
    pump_chunks(c);
}

void Worker::on_play(JConn& c, Rd& r, int pid) {
    const auto& id = c.vd->id;
    if (pid == id.pls_keep_alive) {
        c.last_ka_ack = now_ms();
    } else if (pid == id.pls_move_pos) {
        double x = r.f64(), y = r.f64(), z = r.f64();
        uint8_t fl = r.u8();
        if (!r.bad) on_move(c, x, y, z, 0, 0, true, false, fl & 1);
    } else if (pid == id.pls_move_pos_rot) {
        double x = r.f64(), y = r.f64(), z = r.f64();
        float yaw = r.f32(), pitch = r.f32();
        uint8_t fl = r.u8();
        if (!r.bad) on_move(c, x, y, z, yaw, pitch, true, true, fl & 1);
    } else if (pid == id.pls_move_rot) {
        float yaw = r.f32(), pitch = r.f32();
        uint8_t fl = r.u8();
        if (!r.bad) on_move(c, 0, 0, 0, yaw, pitch, false, true, fl & 1);
    } else if (pid == id.pls_move_status) {
        uint8_t fl = r.u8();
        if (!r.bad) on_move(c, 0, 0, 0, 0, 0, false, false, fl & 1);
    } else if (pid == c.vd->packet("play/s/player_input")) {
        uint8_t fl = r.u8();
        if (!r.bad && ((fl & 0x20) != 0) != c.sneaking) {
            c.sneaking = fl & 0x20;
            on_move(c, 0, 0, 0, 0, 0, false, false, c.ground);
        }
    } else if (pid == c.vd->packet("play/s/player_action")) {
        on_dig(c, r);
    } else if (pid == c.vd->packet("play/s/use_item_on")) {
        on_place(c, r);
    } else if (pid == c.vd->packet("play/s/set_creative_mode_slot")) {
        if (c.gamemode == 1) on_creative_slot(c, r);
    } else if (pid == c.vd->packet("play/s/set_carried_item")) {
        int slot = static_cast<int16_t>(r.be16());
        if (!r.bad && slot >= 0 && slot < 9) c.held = slot;
    } else if (pid == c.vd->packet("play/s/punch") || pid == c.vd->packet("play/s/swing")) {
        if (c.gid) s_.game.swing(c.gid);
    } else if (pid == id.pls_batch_received) {
        float rate = r.f32();
        if (!r.bad && std::isfinite(rate)) c.chunk_rate = std::max(0.01f, std::min(rate, 64.0f));
        if (c.batches_out > 0) c.batches_out--;
        pump_chunks(c);
    } else if (pid == id.pls_chat) {
        std::string m = r.str(256);
        if (r.bad || m.empty()) return;
        if (c.gid) s_.game.chat(c.gid, m);
    } else if (pid == id.pls_chat_command) {
        std::string m = r.str(256);
        if (r.bad) return;
        info(c.name + " issued server command: /" + m);
        if (c.gid) s_.game.command(c.gid, m);
    } else if (pid == id.pls_client_information) {
        r.str(16);
        int vdist = r.u8();
        r.varint();
        r.boolean();
        uint8_t parts = r.u8();
        if (!r.bad) c.skin_parts = parts;
        if (!r.bad) {
            int nr = std::max(2, std::min(vdist, s_.cfg.view_distance));
            if (nr != c.radius) { c.radius = nr; rebuild_pending(c); }
        }
    }
}

}  // namespace pn
