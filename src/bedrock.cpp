// Bedrock server logic. The join sequence (resource packs, StartGame, registries, chunks) is replayed from a
// session recorded off a real Bedrock Dedicated Server by tools/bedrock_capture.py; only per-player parts are patched.
#include "bchunk.hpp"
#include "bedrock.hpp"
#include "dimension.hpp"
#include "game.hpp"
#include "xbox.hpp"
#include "skin.hpp"
#include "hash.hpp"
#include "json.hpp"
#include "log.hpp"
#include "server.hpp"
#include "util.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <netinet/in.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <random>
#include <zlib.h>

namespace pn {

// ---------------------------------------------------------------- helpers

static std::string b64enc(const uint8_t* p, size_t n, bool url) {
    static const char* std_t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char* url_t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* t = url ? url_t : std_t;
    std::string o;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == n) { uint32_t v = p[i] << 16; o += t[v >> 18]; o += t[(v >> 12) & 63]; if (!url) o += "=="; }
    else if (i + 2 == n) { uint32_t v = (p[i] << 16) | (p[i + 1] << 8); o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; if (!url) o += '='; }
    return o;
}

static bool b64dec(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>(acc >> bits)); }
    }
    return true;
}

static std::vector<std::string> split_jwt(const std::string& j) {
    std::vector<std::string> parts;
    size_t a = 0;
    for (size_t i = 0; i <= j.size(); i++)
        if (i == j.size() || j[i] == '.') { parts.push_back(j.substr(a, i - a)); a = i + 1; }
    return parts;
}

static bool jwt_json(const std::string& part, JV& out) {
    std::vector<uint8_t> raw;
    if (!b64dec(part, raw)) return false;
    JP jp{reinterpret_cast<const char*>(raw.data()), reinterpret_cast<const char*>(raw.data()) + raw.size()};
    out = jp.val();
    return !jp.bad && out.t == JV::Obj;
}

static std::vector<uint8_t> deflate_raw(const uint8_t* p, size_t n, int level) {
    z_stream z{};
    deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    std::vector<uint8_t> out(deflateBound(&z, static_cast<uLong>(n)));
    z.next_in = const_cast<Bytef*>(p);
    z.avail_in = static_cast<uInt>(n);
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

static bool inflate_raw(const uint8_t* p, size_t n, std::vector<uint8_t>& out, size_t cap) {
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) return false;
    out.clear();
    z.next_in = const_cast<Bytef*>(p);
    z.avail_in = static_cast<uInt>(n);
    uint8_t buf[16384];
    int rc;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&z); return false; }
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
        if (out.size() > cap) { inflateEnd(&z); return false; }
    } while (rc != Z_STREAM_END && (z.avail_in > 0 || z.avail_out == 0));
    inflateEnd(&z);
    return true;
}

static void put_varuint(std::vector<uint8_t>& o, uint32_t v) {
    while (v >= 0x80) { o.push_back(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
    o.push_back(static_cast<uint8_t>(v));
}

static uint32_t zz_enc(int32_t v) { return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31); }
static int32_t zz_dec(uint32_t v) { return static_cast<int32_t>((v >> 1) ^ (~(v & 1) + 1)); }

static bool read_varuint(const uint8_t*& p, const uint8_t* e, uint32_t& v) {
    v = 0;
    for (int s = 0; s < 35; s += 7) {
        if (p >= e) return false;
        uint8_t c = *p++;
        v |= static_cast<uint32_t>(c & 0x7f) << s;
        if (!(c & 0x80)) return true;
    }
    return false;
}

static void put_str(std::vector<uint8_t>& o, const std::string& s) { put_varuint(o, static_cast<uint32_t>(s.size())); o.insert(o.end(), s.begin(), s.end()); }

std::vector<uint8_t> replace_string_bytes(const std::vector<uint8_t>& in, const std::string& from, const std::string& to) {
    std::vector<uint8_t> pat;
    put_str(pat, from);
    std::vector<uint8_t> rep;
    put_str(rep, to);
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < in.size()) {
        if (i + pat.size() <= in.size() && memcmp(&in[i], pat.data(), pat.size()) == 0) { out.insert(out.end(), rep.begin(), rep.end()); i += pat.size(); }
        else out.push_back(in[i++]);
    }
    return out;
}

static std::string uuid_from_bytes(const uint8_t* u) {
    char b[40];
    snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return b;
}

static bool parse_uuid(const std::string& s, uint8_t out[16]) {
    int n = 0;
    uint8_t tmp[16];
    for (size_t i = 0; i < s.size() && n < 16; i++) {
        if (s[i] == '-') continue;
        if (i + 1 >= s.size()) return false;
        char h[3] = {s[i], s[i + 1], 0};
        char* e;
        long v = strtol(h, &e, 16);
        if (*e) return false;
        tmp[n++] = static_cast<uint8_t>(v);
        i++;
    }
    if (n != 16) return false;
    memcpy(out, tmp, 16);
    return true;
}

// ---------------------------------------------------------------- data file

static bool load_bedrock_file(const std::string& path, int level, BedrockData& d, std::string& err);

bool load_bedrock_data(const std::string& cfg_dir, const std::string& exe_dir, int level, std::map<int, BedrockData>& out, std::string& err) {
    for (auto& dir : {cfg_dir, std::string("data"), exe_dir + "/data", exe_dir + "/../data"}) {
        if (dir.empty()) continue;
        DIR* dp = opendir(dir.c_str());
        if (!dp) continue;
        std::vector<std::string> names;
        while (auto* e = readdir(dp)) {
            std::string n = e->d_name;
            if (n.rfind("bedrock-", 0) == 0 && n.size() > 12 && n.substr(n.size() - 4) == ".bin" && n.rfind("bedrock-blocks", 0) != 0) names.push_back(n);
        }
        closedir(dp);
        std::sort(names.begin(), names.end());   // a later file for the same protocol wins
        for (auto& n : names) {
            BedrockData d;
            std::string ferr;
            if (!load_bedrock_file(dir + "/" + n, level, d, ferr)) { warn("Bedrock data " + n + " skipped: " + ferr); continue; }
            out[d.protocol] = std::move(d);
        }
        if (!out.empty()) return true;
    }
    err = "no Bedrock data found (data/bedrock-*.bin). Run: python3 tools/fetch_bedrock_data.py";
    return false;
}

static bool load_bedrock_file(const std::string& path, int level, BedrockData& d, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), {});
    if (raw.size() < 16 || memcmp(raw.data(), "PNBE", 4) != 0) { err = "bad Bedrock data file " + path; return false; }
    uint32_t ver, proto, count;
    memcpy(&ver, &raw[4], 4); memcpy(&proto, &raw[8], 4); memcpy(&count, &raw[12], 4);
    d.protocol = static_cast<int>(proto);
    size_t pos = 16;
    std::vector<uint8_t> body;
    auto flush_body = [&](bool patch) {
        if (!body.empty()) {
            auto z = deflate_raw(body.data(), body.size(), level);
            BedrockData::Segment seg;
            seg.batch.push_back(0);
            seg.batch.insert(seg.batch.end(), z.begin(), z.end());
            d.start.push_back(std::move(seg));
            body.clear();
        }
        if (patch) { BedrockData::Segment seg; seg.patch_player_list = true; d.start.push_back(std::move(seg)); }
    };
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 7 > raw.size()) { err = "truncated Bedrock data file"; return false; }
        uint8_t kind = raw[pos];
        uint16_t id;
        uint32_t len;
        memcpy(&id, &raw[pos + 1], 2); memcpy(&len, &raw[pos + 3], 4);
        pos += 7;
        if (pos + len > raw.size()) { err = "truncated Bedrock data file"; return false; }
        std::vector<uint8_t> pl(raw.begin() + pos, raw.begin() + pos + len);
        pos += len;
        switch (kind) {
            case 1: d.info = std::move(pl); break;
            case 2: d.stack = std::move(pl); break;
            case 3:
                if (id == 11 || id == 45) {
                    flush_body(false);
                    BedrockData::Segment seg;
                    seg.patch = id == 11 ? 2 : 3;
                    seg.raw = std::move(pl);
                    d.start.push_back(std::move(seg));
                } else if (id == 63) {
                    if (d.player_list_raw.empty()) d.player_list_raw = pl;
                    flush_body(true);
                } else {
                    std::vector<uint8_t> pk;
                    put_varuint(pk, id);
                    pk.insert(pk.end(), pl.begin(), pl.end());
                    if (!body.empty() && body.size() + pk.size() > (512u << 10)) flush_body(false);
                    put_varuint(body, static_cast<uint32_t>(pk.size()));
                    body.insert(body.end(), pk.begin(), pk.end());
                }
                break;
            case 4: d.extras.emplace_back(id, std::move(pl)); break;
            case 5: d.chunk_tail = std::move(pl); break;
            case 6: d.subchunks[id - 64] = std::move(pl); break;
            case 7: d.capture_name.assign(pl.begin(), pl.end()); break;
            case 8: if (id == 12) d.add_player_tpl = std::move(pl); break;
        }
    }
    flush_body(false);
    if (d.info.empty() || d.stack.empty() || d.chunk_tail.empty() || d.player_list_raw.empty() || d.capture_name.empty()) { err = "incomplete Bedrock data file " + path; return false; }
    return true;
}

// ---------------------------------------------------------------- session

BSession::~BSession() {
    if (enc) EVP_CIPHER_CTX_free(enc);
    if (dec) EVP_CIPHER_CTX_free(dec);
}

Bedrock::Bedrock(Server& s, int fd, std::map<int, BedrockData>&& data, bool has_data) : s_(s), data_(std::move(data)), has_data_(has_data) {
    default_skin_ = bedrock_classic_skin(default_skin_rgba(false), false, "painite-steve");
    server_key_ = EVP_EC_gen("P-384");
    if (server_key_) {
        int n = i2d_PUBKEY(server_key_, nullptr);
        std::vector<uint8_t> der(static_cast<size_t>(n));
        uint8_t* p = der.data();
        i2d_PUBKEY(server_key_, &p);
        server_pub_b64_ = b64enc(der.data(), der.size(), false);
    }
    rak_ = std::make_unique<Rak>(fd, s.guid, "", [this](RakSession& r, const uint8_t* p, size_t n) { on_payload(r, p, n); },
                                 [this](RakSession& r) { on_close(r); });
}

Bedrock::~Bedrock() {
    if (server_key_) EVP_PKEY_free(server_key_);
}

void Bedrock::shutdown() {
    rak_->for_each([&](RakSession& r) { rak_->close(r, true); });
    rak_->tick(now_ms());
}

void Bedrock::on_datagram(const sockaddr_storage& from, socklen_t fl, const uint8_t* p, size_t n, int64_t now) { rak_->on_datagram(from, fl, p, n, now); }

void Bedrock::tick(int64_t now) {
    if (now - last_pong_ms_ > 1000) {
        last_pong_ms_ = now;
        const Config& c = s_.cfg;
        std::string motd = c.motd, level = c.level_name;
        for (auto& ch : motd) if (ch == ';') ch = ',';
        for (auto& ch : level) if (ch == ';') ch = ',';
        static const char* gm[] = {"Survival", "Creative", "Adventure", "Spectator"};
        rak_->set_pong("MCPE;" + motd + ";" + std::to_string(c.bedrock_protocol) + ";" + c.bedrock_version + ";" + std::to_string(s_.online.load()) + ";" +
                       std::to_string(c.max_players) + ";" + std::to_string(s_.guid) + ";" + level + ";" + gm[c.gamemode & 3] + ";1;" +
                       std::to_string(c.bedrock_port_effective()) + ";" + std::to_string(c.bedrock_port_effective()) + ";");
    }
    rak_->for_each([&](RakSession& r) {
        auto* b = static_cast<BSession*>(r.user);
        if (b && b->ident) b->ident->ping_ms = static_cast<int>(r.srtt_ms);
        if (b && !r.closed && (b->st == BSession::St::Spawning || (b->st == BSession::St::Playing && !b->pending.empty()))) pump_chunks(*b);
    });
    rak_->tick(now);
}

void Bedrock::on_close(RakSession& r) {
    auto* b = static_cast<BSession*>(r.user);
    if (!b) return;
    r.user = nullptr;
    by_id_.erase(b->id);
    if (b->gid) { s_.game.remove_player(b->gid); b->gid = 0; }
    if (b->reserved) s_.release_name(b->name);
    if (b->counted) {
        s_.unregister_player(b->name);
        s_.online--;
        playing_--;
        info(b->name + " lost connection: Disconnected");
        info(b->name + " left the game");
        if (!s_.stopping) s_.broadcast(b->name + " left the game");
    }
    delete b;
}

void Bedrock::on_payload(RakSession& r, const uint8_t* p, size_t n) {
    if (!has_data_) { rak_->close(r, true); return; }
    auto* b = static_cast<BSession*>(r.user);
    if (!b) { b = new BSession(); b->rak = &r; b->threshold = std::max(1, std::min(s_.cfg.compression_threshold < 0 ? 1 : s_.cfg.compression_threshold, 65535)); r.user = b; b->id = s_.next_conn_id++; by_id_[b->id] = b; }
    std::vector<uint8_t> buf(p, p + n);
    if (b->dec) {
        if (buf.size() < 9) { rak_->close(r, false); return; }
        int outl = 0;
        EVP_DecryptUpdate(b->dec, buf.data(), &outl, buf.data(), static_cast<int>(buf.size()));
        uint8_t trailer[8];
        memcpy(trailer, buf.data() + buf.size() - 8, 8);
        buf.resize(buf.size() - 8);
        uint8_t ctr[8];
        for (int i = 0; i < 8; i++) ctr[i] = static_cast<uint8_t>(b->recv_ctr >> (8 * i));
        b->recv_ctr++;
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        uint8_t h[32];
        unsigned hl = 0;
        EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
        EVP_DigestUpdate(md, ctr, 8);
        EVP_DigestUpdate(md, buf.data(), buf.size());
        EVP_DigestUpdate(md, b->key, 32);
        EVP_DigestFinal_ex(md, h, &hl);
        EVP_MD_CTX_free(md);
        if (memcmp(h, trailer, 8) != 0) { rak_->close(r, false); return; }
    }
    std::vector<uint8_t> body;
    const uint8_t* bp = buf.data();
    size_t bn = buf.size();
    if (b->compress) {
        if (bn < 1) return;
        uint8_t algo = bp[0];
        if (algo == 0) { if (!inflate_raw(bp + 1, bn - 1, body, 32u << 20)) { rak_->close(r, false); return; } bp = body.data(); bn = body.size(); }
        else if (algo == 0xff) { bp++; bn--; }
        else { rak_->close(r, false); return; }
    }
    const uint8_t* e = bp + bn;
    while (bp < e && !r.closed) {
        uint32_t len;
        if (!read_varuint(bp, e, len) || len > static_cast<uint32_t>(e - bp)) { rak_->close(r, false); return; }
        const uint8_t* pe = bp + len;
        uint32_t hdr;
        const uint8_t* q = bp;
        if (!read_varuint(q, pe, hdr)) { rak_->close(r, false); return; }
        on_packet(*b, static_cast<uint16_t>(hdr & 0x3ff), q, static_cast<size_t>(pe - q));
        bp = pe;
        if (r.closed) return;
    }
    rak_->flush(r, now_ms());
}

void Bedrock::send_raw_body(BSession& b, std::vector<uint8_t> data) {
    if (b.rak->closed) return;
    if (b.enc) {
        uint8_t ctr[8];
        for (int i = 0; i < 8; i++) ctr[i] = static_cast<uint8_t>(b.send_ctr >> (8 * i));
        b.send_ctr++;
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        uint8_t h[32];
        unsigned hl = 0;
        EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
        EVP_DigestUpdate(md, ctr, 8);
        EVP_DigestUpdate(md, data.data(), data.size());
        EVP_DigestUpdate(md, b.key, 32);
        EVP_DigestFinal_ex(md, h, &hl);
        EVP_MD_CTX_free(md);
        data.insert(data.end(), h, h + 8);
        int outl = 0;
        EVP_EncryptUpdate(b.enc, data.data(), &outl, data.data(), static_cast<int>(data.size()));
    }
    data.insert(data.begin(), 0xFE);
    rak_->send(*b.rak, data.data(), data.size(), true);
}

void Bedrock::send_packets(BSession& b, const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& pk) {
    std::vector<uint8_t> body;
    for (auto& p : pk) {
        std::vector<uint8_t> one;
        put_varuint(one, p.first);
        one.insert(one.end(), p.second.begin(), p.second.end());
        put_varuint(body, static_cast<uint32_t>(one.size()));
        body.insert(body.end(), one.begin(), one.end());
    }
    if (!b.compress) { send_raw_body(b, std::move(body)); return; }
    std::vector<uint8_t> out;
    if (static_cast<int>(body.size()) >= b.threshold) {
        auto z = deflate_raw(body.data(), body.size(), s_.cfg.compression_level);
        out.push_back(0);
        out.insert(out.end(), z.begin(), z.end());
    } else {
        out.push_back(0xff);
        out.insert(out.end(), body.begin(), body.end());
    }
    send_raw_body(b, std::move(out));
}

void Bedrock::send_batch(BSession& b, const std::vector<uint8_t>& batch) { send_raw_body(b, batch); }

void Bedrock::disconnect(BSession& b, const std::string& msg) {
    std::vector<uint8_t> pl;
    put_varuint(pl, 0);
    pl.push_back(0);
    put_str(pl, msg);
    put_str(pl, "");
    send_packets(b, {{5, pl}});
    rak_->flush(*b.rak, now_ms());
    rak_->close(*b.rak, true);
}

// ---------------------------------------------------------------- login and encryption

bool Bedrock::start_encryption(BSession& b, const std::string& client_pub_b64, std::string& jwt) {
    std::vector<uint8_t> der;
    if (!server_key_ || !b64dec(client_pub_b64, der)) return false;
    const uint8_t* dp = der.data();
    EVP_PKEY* peer = d2i_PUBKEY(nullptr, &dp, static_cast<long>(der.size()));
    if (!peer) return false;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(server_key_, nullptr);
    std::vector<uint8_t> secret;
    size_t sl = 0;
    bool ok = ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, peer) > 0 && EVP_PKEY_derive(ctx, nullptr, &sl) > 0;
    if (ok) { secret.resize(sl); ok = EVP_PKEY_derive(ctx, secret.data(), &sl) > 0; secret.resize(sl); }
    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    if (!ok) return false;
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    uint8_t salt[16];
    for (auto& x : salt) x = static_cast<uint8_t>(rng());
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    unsigned hl = 0;
    EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
    EVP_DigestUpdate(md, salt, 16);
    EVP_DigestUpdate(md, secret.data(), secret.size());
    EVP_DigestFinal_ex(md, b.key, &hl);
    EVP_MD_CTX_free(md);
    // ES384 JWT: header {alg, x5u}, payload {salt}
    std::string h = "{\"alg\":\"ES384\",\"x5u\":\"" + server_pub_b64_ + "\"}";
    std::string pl = "{\"salt\":\"" + b64enc(salt, 16, false) + "\"}";
    std::string signing = b64enc(reinterpret_cast<const uint8_t*>(h.data()), h.size(), true) + "." + b64enc(reinterpret_cast<const uint8_t*>(pl.data()), pl.size(), true);
    EVP_MD_CTX* sc = EVP_MD_CTX_new();
    size_t sigl = 0;
    std::vector<uint8_t> sig;
    bool sok = EVP_DigestSignInit(sc, nullptr, EVP_sha384(), nullptr, server_key_) > 0 &&
               EVP_DigestSign(sc, nullptr, &sigl, reinterpret_cast<const uint8_t*>(signing.data()), signing.size()) > 0;
    if (sok) { sig.resize(sigl); sok = EVP_DigestSign(sc, sig.data(), &sigl, reinterpret_cast<const uint8_t*>(signing.data()), signing.size()) > 0; sig.resize(sigl); }
    EVP_MD_CTX_free(sc);
    if (!sok) return false;
    const uint8_t* sp = sig.data();
    ECDSA_SIG* es = d2i_ECDSA_SIG(nullptr, &sp, static_cast<long>(sig.size()));
    if (!es) return false;
    uint8_t raw[96];
    const BIGNUM *r, *s;
    ECDSA_SIG_get0(es, &r, &s);
    BN_bn2binpad(r, raw, 48);
    BN_bn2binpad(s, raw + 48, 48);
    ECDSA_SIG_free(es);
    jwt = signing + "." + b64enc(raw, 96, true);
    return true;
}

static uint32_t le32(Rd& r) { uint32_t v = r.u8(); v |= static_cast<uint32_t>(r.u8()) << 8; v |= static_cast<uint32_t>(r.u8()) << 16; v |= static_cast<uint32_t>(r.u8()) << 24; return v; }

void Bedrock::on_login(BSession& b, const uint8_t* p, size_t n) {
    Rd r(p, n);
    int proto = static_cast<int32_t>(r.be32());
    r.varint();
    uint32_t il = le32(r);
    if (r.bad || il > r.left()) { rak_->close(*b.rak, false); return; }
    std::string identity(reinterpret_cast<const char*>(r.p), il);
    r.skip(il);
    uint32_t cl = le32(r);
    if (r.bad || cl > r.left()) { rak_->close(*b.rak, false); return; }
    std::string client(reinterpret_cast<const char*>(r.p), cl);
    if (!data_.count(proto)) {
        std::vector<uint8_t> pl(4);
        uint32_t st = data_.empty() || proto < data_.begin()->first ? 1 : 2;
        for (int i = 0; i < 4; i++) pl[i] = static_cast<uint8_t>(st >> (24 - 8 * i));
        send_packets(b, {{2, pl}});
        rak_->close(*b.rak, true);
        return;
    }
    b.proto = proto;
    // identity: {"Certificate":"{\"chain\":[...]}","AuthenticationType":n,"Token":"..."} (protocol >= 818)
    std::string name, uuid_s, xuid;
    JP ip{identity.data(), identity.data() + identity.size()};
    JV idv = ip.val();
    if (ip.bad || idv.t != JV::Obj) { disconnect(b, "Invalid login"); return; }
    if (const JV* cert = idv.get("Certificate")) {
        if (!cert->s.empty()) {
            JP cp{cert->s.data(), cert->s.data() + cert->s.size()};
            JV cv = cp.val();
            if (const JV* chain = cv.get("chain"))
                for (auto& tok : chain->a) {
                    auto parts = split_jwt(tok.s);
                    JV pl;
                    if (parts.size() == 3 && jwt_json(parts[1], pl))
                        if (const JV* ed = pl.get("extraData")) {
                            if (auto* v = ed->get("displayName")) name = v->s;
                            if (auto* v = ed->get("identity")) uuid_s = v->s;
                            if (auto* v = ed->get("XUID")) xuid = v->s;
                        }
                }
        }
    }
    if (const JV* tok = idv.get("Token")) {
        auto parts = split_jwt(tok->s);
        JV pl;
        if (parts.size() == 3 && jwt_json(parts[1], pl)) {
            if (name.empty()) if (auto* v = pl.get("xname")) name = v->s;
            if (uuid_s.empty()) if (auto* v = pl.get("identity")) uuid_s = v->s;
            if (xuid.empty()) if (auto* v = pl.get("xid")) xuid = v->s;
        }
    }
    auto cparts = split_jwt(client);
    JV chdr;
    if (cparts.size() != 3 || !jwt_json(cparts[0], chdr)) { disconnect(b, "Invalid login"); return; }
    std::string client_pub;
    if (const JV* x = chdr.get("x5u")) client_pub = x->s;
    {
        JV cdv;
        if (jwt_json(cparts[1], cdv) && cdv.t == JV::Obj) {
            std::vector<uint8_t> sk;
            if (bedrock_skin_from_client(cdv, sk) && sk.size() < 512 * 1024) b.skin = std::move(sk);
        }
    }
    if (name.empty()) { JV cp; if (jwt_json(cparts[1], cp)) if (auto* v = cp.get("ThirdPartyName")) name = v->s; }

    bool need_xbox = s_.cfg.online_mode && !s_.cfg.bedrock_unauthenticated;
    if (need_xbox) {
        const JV* tok = idv.get("Token");
        if (!tok || tok->s.empty() || client_pub.empty()) { disconnect(b, "This server requires an Xbox Live login"); return; }
        b.st = BSession::St::Authing;
        b.pending_pub = client_pub;
        uint64_t sid = b.id;
        std::string token = tok->s;
        s_.auth.post_task([this, sid, token, client_pub] {
            XboxResult res = xbox_verify(s_.auth, token, client_pub);
            s_.post_to_worker(0, [sid, res](Worker& w) { w.bedrock_xbox_result(sid, res); });
        });
        return;
    }
    continue_login(b, name, uuid_s, xuid, client_pub);
}

void Bedrock::on_xbox_result(uint64_t id, const XboxResult& res) {
    BSession* b = by_id(id);
    if (!b || b->rak->closed || b->st != BSession::St::Authing) return;
    if (!res.ok) {
        warn("Xbox login failed for a Bedrock client: " + res.error);
        disconnect(*b, "Failed to verify your Xbox login: " + res.error);
        return;
    }
    // Floodgate's UUID scheme: 00000000-0000-0000-<xuid as 64 bit hex>
    char u[40];
    snprintf(u, sizeof u, "00000000-0000-0000-%04llx-%012llx", static_cast<unsigned long long>((strtoull(res.xuid.c_str(), nullptr, 10) >> 48) & 0xffff),
             static_cast<unsigned long long>(strtoull(res.xuid.c_str(), nullptr, 10) & 0xffffffffffffull));
    b->st = BSession::St::LoginWait;
    continue_login(*b, res.gamertag, u, res.xuid, b->pending_pub);
}

void Bedrock::continue_login(BSession& b, const std::string& name, const std::string& uuid_s, const std::string& xuid, const std::string& client_pub) {
    std::string clean;
    for (unsigned char ch : name) clean += (isalnum(ch) || ch == '_') ? static_cast<char>(ch) : (ch == ' ' ? '_' : '\0');
    clean.erase(std::remove(clean.begin(), clean.end(), '\0'), clean.end());
    if (clean.empty() || clean.size() > 16) { disconnect(b, "Invalid username"); return; }
    b.name = s_.cfg.bedrock_prefix + clean;
    b.xuid = xuid;
    if (!parse_uuid(uuid_s, b.uuid)) offline_uuid(b.name, b.uuid);

    std::string why;
    if (s_.lists.banned(b.name, uuid_string(b.uuid), &why)) { disconnect(b, "You are banned from this server.\nReason: " + (why.empty() ? std::string("Banned by an operator.") : why)); return; }
    if (s_.cfg.white_list && !s_.whitelisted(b.name, uuid_string(b.uuid))) { disconnect(b, "You are not white-listed on this server!"); return; }
    if (s_.online.load() >= s_.cfg.max_players) { disconnect(b, "The server is full!"); return; }
    if (!s_.reserve_name(b.name)) { disconnect(b, "A player with that name is already online."); return; }
    b.reserved = true;
    b.eid = s_.next_entity++;
    {
        auto sp = s_.world.pick_spawn(uuid_string(b.uuid), s_.cfg.gamemode);
        b.x = sp.x; b.y = sp.y; b.z = sp.z; b.yaw = sp.yaw; b.pitch = sp.pitch; b.gamemode = sp.gamemode; b.dim = sp.dim;
        b.cx = static_cast<int>(std::floor(b.x / 16));
        b.cz = static_cast<int>(std::floor(b.z / 16));
    }
    info("UUID of player " + b.name + " is " + uuid_from_bytes(b.uuid));
    if (s_.cfg.bedrock_encryption && !client_pub.empty()) {
        std::string jwt;
        if (!start_encryption(b, client_pub, jwt)) { disconnect(b, "Encryption handshake failed"); return; }
        std::vector<uint8_t> pl;
        put_str(pl, jwt);
        b.st = BSession::St::Handshake;
        send_packets(b, {{3, pl}});
        // everything after this packet is encrypted in both directions
        uint8_t iv[16];
        memcpy(iv, b.key, 12);
        iv[12] = iv[13] = iv[14] = 0;
        iv[15] = 2;
        b.enc = EVP_CIPHER_CTX_new();
        b.dec = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(b.enc, EVP_aes_256_ctr(), nullptr, b.key, iv);
        EVP_DecryptInit_ex(b.dec, EVP_aes_256_ctr(), nullptr, b.key, iv);
        return;
    }
    after_handshake(b);
}

void Bedrock::after_handshake(BSession& b) {
    std::vector<uint8_t> ok(4, 0);
    b.st = BSession::St::Packs;
    send_packets(b, {{2, ok}, {6, dat(b).info}});
}

void Bedrock::on_pack_response(BSession& b, const uint8_t* p, size_t n) {
    const uint8_t* e = p + n;
    uint32_t st;
    if (b.proto < 2168) {   // until 2167: u8 status (1 refused, 2 send packs, 3 have all, 4 completed), u16 count, pack ids
        if (p >= e) return;
        st = *p++;
        st = st == 0 ? 1 : st - 1;   // the same numbers as 2168+: 0 cancel, 1 downloading, 2 downloaded, 3 stack finished
    } else if (!read_varuint(p, e, st)) return;
    if (st == 0) { rak_->close(*b.rak, true); return; }
    if (st == 2 && b.st == BSession::St::Packs) {
        b.st = BSession::St::Stack;
        send_packets(b, {{7, dat(b).stack}});
    } else if (st == 3 && b.st == BSession::St::Stack) {
        b.st = BSession::St::Start;
        send_start(b);
    }
}

void Bedrock::send_start(BSession& b) {
    for (auto& seg : dat(b).start) {
        if (seg.patch_player_list) send_packets(b, {{63, patched_player_list(b)}});
        else if (seg.patch == 2) send_packets(b, {{11, patched_start_game(b, seg.raw)}});
        else if (seg.patch == 3) send_packets(b, {{45, patched_respawn(b, seg.raw)}});
        else send_batch(b, seg.batch);
    }
    send_time_to(b, s_.game.day_time());
    rak_->flush(*b.rak, now_ms());
}

// ---------------------------------------------------------------- chunks

static int64_t pack(int x, int z) { return (static_cast<int64_t>(x) << 32) | static_cast<uint32_t>(z); }

void Bedrock::on_radius(BSession& b, const uint8_t* p, size_t n) {
    const uint8_t* e = p + n;
    uint32_t v;
    if (!read_varuint(p, e, v)) return;
    int req = zz_dec(v);
    int r = std::max(2, std::min(req, s_.cfg.view_distance));
    b.radius = r;
    std::vector<uint8_t> pl;
    put_varuint(pl, zz_enc(r));
    send_packets(b, {{70, pl}});
    if (b.st == BSession::St::Start) begin_chunks(b);
    else queue_chunks(b);
}

void Bedrock::send_npu(BSession& b) {
    std::vector<uint8_t> pl;
    put_varuint(pl, zz_enc(b.cx * 16 + 8));
    put_varuint(pl, 0);
    put_varuint(pl, zz_enc(b.cz * 16 + 8));
    put_varuint(pl, static_cast<uint32_t>(b.radius * 16));
    pl.insert(pl.end(), 4, 0);
    send_packets(b, {{121, pl}});
}

void Bedrock::queue_chunks(BSession& b) {
    b.pending.clear();
    int r = b.radius;
    for (int dx = -r; dx <= r; dx++)
        for (int dz = -r; dz <= r; dz++) {
            if (dx * dx + dz * dz > r * r) continue;
            int x = b.cx + dx, z = b.cz + dz;
            if (!b.sent.count(pack(x, z))) b.pending.emplace_back(x, z);
        }
    int cx = b.cx, cz = b.cz;
    std::sort(b.pending.begin(), b.pending.end(), [&](const auto& a, const auto& c) {
        long da = static_cast<long>(a.first - cx) * (a.first - cx) + static_cast<long>(a.second - cz) * (a.second - cz);
        long dc = static_cast<long>(c.first - cx) * (c.first - cx) + static_cast<long>(c.second - cz) * (c.second - cz);
        return da > dc;
    });
}

void Bedrock::begin_chunks(BSession& b) {
    b.st = BSession::St::Spawning;
    send_npu(b);
    queue_chunks(b);
    pump_chunks(b);
}

void Bedrock::pump_chunks(BSession& b) {
    if (b.rak->closed) return;
    size_t cap = static_cast<size_t>(s_.cfg.backlog_cap_kb) * 1024;
    int budget = 24;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> batch;
    while (!b.pending.empty() && budget-- > 0 && b.rak->queued_bytes() < cap) {
        auto [x, z] = b.pending.back();
        b.pending.pop_back();
        std::vector<uint8_t> pl;
        put_varuint(pl, zz_enc(x));
        put_varuint(pl, zz_enc(z));
        const auto& tail = level_chunk_tail(b.dim, x, z, b.proto < 2168);
        pl.insert(pl.end(), tail.begin(), tail.end());
        b.sent.insert(pack(x, z));
        batch.emplace_back(58, std::move(pl));
    }
    if (!batch.empty()) send_packets(b, batch);
    if (b.pending.empty() && b.st == BSession::St::Spawning && !b.spawn_sent) finish_spawn(b);
    rak_->flush(*b.rak, now_ms());
}

void Bedrock::finish_spawn(BSession& b) {
    b.spawn_sent = true;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> pk;
    const BedrockData& d = dat(b);
    for (auto& ex : d.extras) {
        if (ex.first == 42) continue;
        pk.emplace_back(ex.first, ex.first == 39 ? replace_string_bytes(ex.second, d.capture_name, b.name) : ex.second);
    }
    for (auto& ex : d.extras) if (ex.first == 42) pk.emplace_back(42, ex.second);
    pk.emplace_back(2, std::vector<uint8_t>{0, 0, 0, 3});   // PlayStatus: player spawn
    send_packets(b, pk);
}

void Bedrock::on_subchunk_request(BSession& b, const uint8_t* p, size_t n) {
    const uint8_t* e = p + n;
    uint32_t dim_wire, cnt;
    if (!read_varuint(p, e, dim_wire) || !read_varuint(p, e, cnt) || cnt > 8192 || static_cast<size_t>(e - p) < cnt * 3 + 12) return;
    const uint32_t dim = (dim_wire >> 1) ^ (0u - (dim_wire & 1));   // the dimension is a signed varint
    const uint8_t* offs = p;
    p += cnt * 3;
    int32_t c[3];
    memcpy(c, p, 12);
    std::vector<uint8_t> pl;
    pl.push_back(0);                      // cache disabled
    put_varuint(pl, dim_wire);
    if (b.proto >= 2168) {
        pl.insert(pl.end(), p, p + 12);   // the centre as three little endian ints
        put_varuint(pl, cnt);
    } else {                              // until 2167: three zigzag varints and a little endian count
        for (int i = 0; i < 3; i++) put_varuint(pl, zz_enc(c[i]));
        for (int i = 0; i < 4; i++) pl.push_back(static_cast<uint8_t>(cnt >> (8 * i)));
    }
    std::vector<uint8_t> entry;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t* o = offs + i * 3;
        int k = c[1] + static_cast<int8_t>(o[1]) + 4;
        int cx = c[0] + static_cast<int8_t>(o[0]), cz = c[2] + static_cast<int8_t>(o[2]);
        pl.insert(pl.end(), o, o + 3);
        int kmin = dim == 0 ? 0 : 4, kmax = dim == 0 ? kSections : dim == 1 ? 12 : 20;
        if (dim < kDimensions && k >= kmin && k < kmax) {
            auto ch = s_.dim(static_cast<int>(dim)).chunk(cx, cz);
            encode_bedrock_subchunk(*ch, k, entry, b.proto);
            pl.insert(pl.end(), entry.begin(), entry.end());
        } else pl.insert(pl.end(), {5, 0, 0, 0, 0, 0, 0, 0});   // Y_INDEX_OUT_OF_BOUNDS, no data
    }
    send_packets(b, {{174, pl}});
}

void Bedrock::on_move(BSession& b, float x, float z) {
    if (!std::isfinite(x) || !std::isfinite(z) || std::fabs(x) > 3e7f || std::fabs(z) > 3e7f) return;
    if (b.st != BSession::St::Playing && b.st != BSession::St::Spawning) return;
    int ncx = static_cast<int>(std::floor(x / 16)), ncz = static_cast<int>(std::floor(z / 16));
    if (ncx == b.cx && ncz == b.cz) return;
    b.cx = ncx;
    b.cz = ncz;
    send_npu(b);
    queue_chunks(b);
    pump_chunks(b);
}

// ---------------------------------------------------------------- chat

void Bedrock::send_text(BSession& b, const std::string& text) {
    std::vector<uint8_t> pl;
    pl.push_back(0);   // needs translation
    pl.push_back(0);   // category: message only
    pl.push_back(0);   // type: raw
    put_str(pl, text.substr(0, 512));
    put_str(pl, "");
    put_str(pl, "");
    pl.push_back(0);   // no filtered message
    send_packets(b, {{9, pl}});
}

void Bedrock::broadcast_chat(const std::string& text) {
    rak_->for_each([&](RakSession& r) {
        auto* b = static_cast<BSession*>(r.user);
        if (b && !r.closed && b->st == BSession::St::Playing) { send_text(*b, text); rak_->flush(r, now_ms()); }
    });
}

void Bedrock::on_text(BSession& b, const uint8_t* p, size_t n) {
    if (b.st != BSession::St::Playing) return;
    Rd r(p, n);
    r.u8();
    r.u8();
    uint8_t type = r.u8();
    std::string msg;
    if (type == 1) { r.str(64); msg = r.str(512); }
    else if (type == 0) msg = r.str(512);
    if (r.bad || msg.empty() || !b.gid) return;
    if (msg[0] == '/') { s_.game.command(b.gid, msg.substr(1)); return; }
    s_.game.chat(b.gid, msg);
}

// ---------------------------------------------------------------- dispatch

void Bedrock::on_packet(BSession& b, uint16_t id, const uint8_t* p, size_t n) {
    switch (id) {
        case 193: {  // RequestNetworkSettings
            if (b.st != BSession::St::Raw || n < 4) { rak_->close(*b.rak, false); return; }
            int proto = static_cast<int32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
            if (!data_.count(proto)) {
                std::vector<uint8_t> pl(4);
                uint32_t st = data_.empty() || proto < data_.begin()->first ? 1 : 2;
                for (int i = 0; i < 4; i++) pl[i] = static_cast<uint8_t>(st >> (24 - 8 * i));
                send_packets(b, {{2, pl}});
                rak_->close(*b.rak, true);
                return;
            }
            b.proto = proto;
            std::vector<uint8_t> pl;
            pl.push_back(static_cast<uint8_t>(b.threshold));
            pl.push_back(static_cast<uint8_t>(b.threshold >> 8));
            pl.insert(pl.end(), {0, 0, 0, 0, 0, 0, 0, 0});   // algorithm zlib, no client throttle
            send_packets(b, {{143, pl}});
            b.compress = true;
            b.st = BSession::St::LoginWait;
            break;
        }
        case 1:
            if (b.st == BSession::St::LoginWait) on_login(b, p, n);
            break;
        case 4:
            if (b.st == BSession::St::Handshake) after_handshake(b);
            break;
        case 8:
            on_pack_response(b, p, n);
            break;
        case 69:
            if (b.st == BSession::St::Start || b.st == BSession::St::Spawning || b.st == BSession::St::Playing) on_radius(b, p, n);
            break;
        case 113:
            if (b.st == BSession::St::Spawning || b.st == BSession::St::Playing) {
                if (!b.counted) {
                    b.counted = true;
                    s_.online++;
                    playing_++;
                    s_.register_player(b.name);
                    info(b.name + " joined the game");
                    char loc[96];
                    snprintf(loc, sizeof loc, "([minecraft:overworld]%.1f, %.1f, %.1f)", b.x, b.y, b.z);
                    info(b.name + "[/" + std::string("bedrock") + "] logged in with Bedrock edition at " + loc);
                    s_.broadcast(b.name + " joined the game");
                }
                b.st = BSession::St::Playing;
                begin_play(b);
            }
            break;
        case 175:
            on_subchunk_request(b, p, n);
            break;
        case 144:
            on_auth_input(b, p, n);
            break;
        case 9:
            on_text(b, p, n);
            break;
        case 77:
            on_command_request(b, p, n);
            break;
        case 44:   // Animate: arm swing
            if (b.st == BSession::St::Playing && b.gid && n >= 1 && p[0] == 1) s_.game.swing(b.gid);
            break;
        default:
            break;
    }
}

}  // namespace pn

