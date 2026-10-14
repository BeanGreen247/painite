#include "bclient.hpp"
#include <arpa/inet.h>
#include <fstream>
#include <netinet/in.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

namespace pt {

static const uint8_t kMagic[16] = {0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};

std::string b64encode(const uint8_t* d, size_t n) {
    std::string out(4 * ((n + 2) / 3), '\0');
    int len = EVP_EncodeBlock(reinterpret_cast<uint8_t*>(out.data()), d, int(n));
    out.resize(size_t(len));
    return out;
}

Bytes b64decode(const std::string& in) {
    std::string s = in;
    for (auto& c : s) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (s.size() % 4) s.push_back('=');
    Bytes out(s.size());
    int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const uint8_t*>(s.data()), int(s.size()));
    if (n < 0) return {};
    size_t pad = 0;
    for (size_t i = s.size(); i > 0 && s[i - 1] == '='; i--) pad++;
    out.resize(size_t(n) - pad);
    return out;
}

static std::string b64url(const uint8_t* d, size_t n) {
    std::string s = b64encode(d, n);
    for (auto& c : s) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}
static std::string b64url(const std::string& s) { return b64url(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

static Bytes addr7(const char* ip, int port) {
    in_addr a;
    inet_pton(AF_INET, ip, &a);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&a);
    Wr w;
    w.u8(4);
    for (int i = 0; i < 4; i++) w.u8(uint8_t(~b[i]));
    w.be16(uint16_t(port));
    return w.b;
}

RakClient::RakClient(const std::string& host, int port, int m) : mtu(m), host_(host), port_(port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    uint64_t g;
    RAND_bytes(reinterpret_cast<uint8_t*>(&g), 8);
    guid_ = g >> 1;
}

RakClient::~RakClient() { close_socket(); }

void RakClient::close_socket() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool RakClient::recv_raw(Bytes& out, int timeout_ms) {
    pollfd pf{fd_, POLLIN, 0};
    if (::poll(&pf, 1, timeout_ms) <= 0) return false;
    out.resize(4096);
    ssize_t n = ::recv(fd_, out.data(), out.size(), 0);
    if (n <= 0) return false;
    out.resize(size_t(n));
    return true;
}

void RakClient::connect() {
    Bytes d;
    bool ok = false;
    for (int i = 0; i < 8 && !ok; i++) {   // real RakNet clients retry the open connection requests
        Wr w;
        w.u8(5).raw(kMagic, 16).u8(11).zeros(size_t(mtu - 28 - 1 - 16 - 1));
        ::send(fd_, w.b.data(), w.b.size(), 0);
        ok = recv_raw(d, 1000) && !d.empty() && d[0] == 0x06;
    }
    if (!ok) throw std::runtime_error("no open connection reply 1");
    mtu = std::min<int>(mtu, d[d.size() - 2] << 8 | d[d.size() - 1]);
    ok = false;
    for (int i = 0; i < 8 && !ok; i++) {
        Wr w;
        w.u8(7).raw(kMagic, 16).raw(addr7(host_.c_str(), port_)).be16(uint16_t(mtu)).be64(guid_);
        ::send(fd_, w.b.data(), w.b.size(), 0);
        ok = recv_raw(d, 1000) && !d.empty() && d[0] == 0x08;
    }
    if (!ok) throw std::runtime_error("no open connection reply 2");
    Wr w;
    w.u8(0x09).be64(guid_).be64(uint64_t(unix_ms())).u8(0);
    send_frame(w.b, 3);
    int64_t end = now_ms() + 15000;
    while (!connected && now_ms() < end) poll(500);
    if (!connected) throw std::runtime_error("RakNet connection request not accepted");
}

Bytes RakClient::make_frame(const Bytes& payload, int rel, const uint8_t* split) {
    Wr h;
    h.u8(uint8_t((rel << 5) | (split ? 0x10 : 0))).be16(uint16_t(payload.size() * 8));
    if (rel == 2 || rel == 3 || rel == 4 || rel == 6 || rel == 7) h.le24(rel_++);
    if (rel == 1 || rel == 4) h.le24(0);
    if (rel == 1 || rel == 3 || rel == 4 || rel == 7) h.le24(ordidx_).u8(0);
    if (split) h.raw(split, 10);
    h.raw(payload);
    return h.b;
}

void RakClient::send_datagram(const Bytes& frames) {
    Wr d;
    d.u8(0x84).le24(seq_).raw(frames);
    sent_[seq_] = {d.b, now_ms()};
    seq_++;
    ::send(fd_, d.b.data(), d.b.size(), 0);
}

void RakClient::send_frame(const Bytes& payload, int rel) {
    size_t maxp = size_t(mtu - 28 - 4 - 24);
    bool ordered = rel == 1 || rel == 3 || rel == 4 || rel == 7;
    if (payload.size() <= maxp) {
        send_datagram(make_frame(payload, rel, nullptr));
        if (ordered) ordidx_++;
        return;
    }
    size_t parts = (payload.size() + maxp - 1) / maxp;
    uint32_t sid = split_id_;
    split_id_ = (split_id_ + 1) & 0xFFFF;
    for (size_t i = 0; i < parts; i++) {
        Bytes part(payload.begin() + long(i * maxp), payload.begin() + long(std::min(payload.size(), (i + 1) * maxp)));
        Wr sp;
        sp.be32(uint32_t(parts)).be16(uint16_t(sid)).be32(uint32_t(i));
        send_datagram(make_frame(part, rel, sp.b.data()));
    }
    if (ordered) ordidx_++;
}

void RakClient::ack(uint32_t seq) {
    Wr w;
    w.u8(0xc0).be16(1).u8(1).le24(seq);
    ::send(fd_, w.b.data(), w.b.size(), 0);
}

void RakClient::handle_payload(const Bytes& p) {
    if (p.empty()) return;
    uint8_t t = p[0];
    if (t == 0xFE) game.emplace_back(p.begin() + 1, p.end());
    else if (t == 0x00 && p.size() >= 9) {
        Wr w;
        w.u8(0x03).raw(p.data() + 1, 8).be64(uint64_t(unix_ms()));
        send_frame(w.b, 0);
    } else if (t == 0x10) {
        Wr w;
        w.u8(0x13).raw(addr7(host_.c_str(), port_));
        for (int i = 0; i < 10; i++) w.raw(addr7("127.0.0.1", 0));
        w.be64(0).be64(uint64_t(unix_ms()));
        send_frame(w.b, 3);
        connected = true;
    } else if (t == 0x15) closed = true;
}

void RakClient::resend_stale() {
    int64_t now = now_ms();
    for (auto& kv : sent_)
        if (now - kv.second.second > 600) { ::send(fd_, kv.second.first.data(), kv.second.first.size(), 0); kv.second.second = now; }
}

bool RakClient::poll(int timeout_ms) {
    resend_stale();
    Bytes d;
    if (!recv_raw(d, timeout_ms)) return false;
    uint8_t t = d[0];
    if (t == 0xC0 || t == 0xA0) {
        Rd r(d, 1);
        int cnt = r.be16();
        for (int k = 0; k < cnt && !r.bad; k++) {
            bool single = r.u8() != 0;
            uint32_t lo = r.le24(), hi = lo;
            if (!single) hi = r.le24();
            for (uint32_t q = lo; q <= hi && q - lo < 4096; q++) {
                if (t == 0xC0) sent_.erase(q);
                else { auto it = sent_.find(q); if (it != sent_.end()) ::send(fd_, it->second.first.data(), it->second.first.size(), 0); }
            }
        }
        return true;
    }
    if (!(t & 0x80)) return true;
    Rd r(d, 1);
    uint32_t seq = r.le24();
    ack(seq);
    while (r.left() > 0 && !r.bad) {
        uint8_t fl = r.u8();
        int rel = fl >> 5;
        bool split = fl & 0x10;
        size_t ln = (r.be16() + 7) / 8;
        bool has_rel = rel == 2 || rel == 3 || rel == 4 || rel == 6 || rel == 7;
        uint32_t rel_idx = has_rel ? r.le24() : 0;
        if (rel == 1 || rel == 4) r.le24();
        bool ordered = rel == 1 || rel == 3 || rel == 4 || rel == 7;
        uint32_t order_idx = 0;
        if (ordered) { order_idx = r.le24(); r.u8(); }
        uint32_t scnt = 0, sid = 0, sidx = 0;
        if (split) { scnt = r.be32(); sid = r.be16(); sidx = r.be32(); }
        Bytes body = r.take(ln);
        if (r.bad) break;
        if (has_rel) { if (!seen_rel_.insert(rel_idx).second) continue; }
        if (split) {
            Split& s = splits_[sid];
            s.n = scnt;
            s.parts[sidx] = std::move(body);
            if (s.parts.size() != s.n) continue;
            Bytes whole;
            for (auto& kv : s.parts) whole.insert(whole.end(), kv.second.begin(), kv.second.end());
            splits_.erase(sid);
            body = std::move(whole);
        }
        if (!ordered) handle_payload(body);
        else {
            order_buf_[order_idx] = std::move(body);
            while (order_buf_.count(next_order_)) { handle_payload(order_buf_[next_order_]); order_buf_.erase(next_order_); next_order_++; }
        }
    }
    return true;
}

// ---- Bedrock layer ----------------------------------------------------------------------------------------

BedrockClient::~BedrockClient() {
    if (key_) EVP_PKEY_free(key_);
    if (enc_) EVP_CIPHER_CTX_free(enc_);
    if (dec_) EVP_CIPHER_CTX_free(dec_);
}

Bytes BedrockClient::trailer(const Bytes& data, uint64_t counter) const {
    Wr w;
    w.le64(counter).raw(data).raw(enc_key_);
    uint8_t h[32];
    SHA256(w.b.data(), w.b.size(), h);
    return Bytes(h, h + 8);
}

static Bytes deflate_raw(const Bytes& in) {
    z_stream z{};
    deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&z, uLong(in.size())) + 16);
    z.next_in = const_cast<uint8_t*>(in.data());
    z.avail_in = uInt(in.size());
    z.next_out = out.data();
    z.avail_out = uInt(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

static Bytes inflate_raw(const uint8_t* in, size_t n) {
    z_stream z{};
    inflateInit2(&z, -15);
    Bytes out;
    uint8_t buf[65536];
    z.next_in = const_cast<uint8_t*>(in);
    z.avail_in = uInt(n);
    int r;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
    } while (r == Z_OK);
    inflateEnd(&z);
    if (r != Z_STREAM_END) throw std::runtime_error("bad deflate data in a Bedrock batch");
    return out;
}

void BedrockClient::send_packets(const std::vector<BPacket>& pkts) {
    Wr body;
    for (auto& p : pkts) {
        Wr pk;
        pk.varuint(uint64_t(p.id)).raw(p.d);
        body.varuint(pk.b.size()).raw(pk.b);
    }
    Bytes data;
    if (compress_) {
        if (body.b.size() >= size_t(threshold_) && algo_ == 0) { data.push_back(0); Bytes c = deflate_raw(body.b); data.insert(data.end(), c.begin(), c.end()); }
        else { data.push_back(0xff); data.insert(data.end(), body.b.begin(), body.b.end()); }
        if (enc_) {
            Bytes t = trailer(data, send_count_++);
            data.insert(data.end(), t.begin(), t.end());
            Bytes o(data.size());
            int n = 0;
            EVP_EncryptUpdate(enc_, o.data(), &n, data.data(), int(data.size()));
            data.swap(o);
        }
    } else data = body.b;
    Wr f;
    f.u8(0xfe).raw(data);
    send_frame(f.b, 3);
}

bool BedrockClient::recv_packets(std::vector<BPacket>& out, int timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    while (game.empty() && now_ms() < end && !closed) poll(int(std::min<int64_t>(200, std::max<int64_t>(1, end - now_ms()))));
    if (game.empty()) return false;
    Bytes data = std::move(game.front());
    game.pop_front();
    if (dec_) {
        Bytes o(data.size());
        int n = 0;
        EVP_DecryptUpdate(dec_, o.data(), &n, data.data(), int(data.size()));
        o.resize(o.size() >= 8 ? o.size() - 8 : 0);   // the trailer is not verified here
        data.swap(o);
        recv_count_++;
    }
    if (compress_ && !data.empty()) {
        uint8_t algo = data[0];
        if (algo == 0) data = inflate_raw(data.data() + 1, data.size() - 1);
        else if (algo == 0xff) data.erase(data.begin());
        else throw std::runtime_error("unsupported compression");
    }
    out.clear();
    Rd r(data);
    while (r.left() > 0 && !r.bad) {
        size_t ln = size_t(r.varuint());
        Bytes pk = r.take(ln);
        if (r.bad) break;
        Rd pr(pk);
        uint64_t hdr = pr.varuint();
        out.push_back({int(hdr & 0x3ff), Bytes(pr.p, pr.e)});
    }
    return true;
}

void BedrockClient::request_settings() {
    send_packets({{193, Wr().be32(uint32_t(protocol_)).b}});
    std::vector<BPacket> pk;
    if (!recv_packets(pk, 5000) || pk.empty() || pk[0].id != 143) throw std::runtime_error("expected NetworkSettings");
    Rd r(pk[0].d);
    threshold_ = r.le16();
    algo_ = r.le16();
    compress_ = true;
}

static std::string random_uuid() {
    uint8_t b[16];
    RAND_bytes(b, 16);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    char s[40];
    snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

static std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o.push_back('\\'); o.push_back(c); }
    return o;
}

static std::string steve_skin_fields() {
    std::ifstream f("data/steve-skin.json");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t a = s.find('{'), b = s.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b <= a) throw std::runtime_error("data/steve-skin.json is missing (the Bedrock tests need it)");
    return s.substr(a + 1, b - a - 1);   // the members without the braces
}

std::string BedrockClient::login(const std::string& name, const std::string& server, std::function<std::string(const std::string&)> token_for) {
    key_ = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-384");
    uint8_t* der = nullptr;
    int dl = i2d_PUBKEY(key_, &der);
    std::string pub = b64encode(der, size_t(dl));
    OPENSSL_free(der);
    auto jwt = [&](const std::string& payload) {
        std::string h = b64url("{\"alg\":\"ES384\",\"x5u\":\"" + pub + "\"}");
        std::string p = b64url(payload);
        std::string signing = h + "." + p;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, EVP_sha384(), nullptr, key_);
        size_t sl = 0;
        EVP_DigestSign(ctx, nullptr, &sl, reinterpret_cast<const uint8_t*>(signing.data()), signing.size());
        Bytes sig(sl);
        EVP_DigestSign(ctx, sig.data(), &sl, reinterpret_cast<const uint8_t*>(signing.data()), signing.size());
        EVP_MD_CTX_free(ctx);
        const uint8_t* sp = sig.data();
        ECDSA_SIG* es = d2i_ECDSA_SIG(nullptr, &sp, long(sl));
        const BIGNUM *r, *s;
        ECDSA_SIG_get0(es, &r, &s);
        uint8_t raw[96];
        BN_bn2binpad(r, raw, 48);
        BN_bn2binpad(s, raw + 48, 48);
        ECDSA_SIG_free(es);
        return signing + "." + b64url(raw, 96);
    };
    int64_t now = unix_ms() / 1000;
    std::string ident = random_uuid();
    char buf[1024];
    snprintf(buf, sizeof buf, "{\"cpk\":\"%s\",\"xid\":\"0\",\"xname\":\"%s\",\"identity\":\"%s\",\"leguuid\":\"%s\",\"mid\":\"\",\"iss\":\"self\",\"aud\":\"api://auth-minecraft-services/multiplayer\",\"nbf\":%lld,\"iat\":%lld,\"exp\":%lld}",
             pub.c_str(), json_escape(name).c_str(), ident.c_str(), ident.c_str(), (long long)now, (long long)now, (long long)now + 3600);
    std::string token = token_for ? token_for(pub) : jwt(buf);
    std::string client = "{" + steve_skin_fields() +
        ",\"ClientRandomId\":" + std::to_string(unix_ms()) + ",\"CurrentInputMode\":1,\"DefaultInputMode\":1,\"DeviceId\":\"" + random_uuid() + "\",\"DeviceModel\":\"PainiteBot\",\"DeviceOS\":12,"
        "\"FilterProfanity\":false,\"GameVersion\":\"1.26.51\",\"GuiScale\":-1,\"LanguageCode\":\"en_GB\",\"GraphicsMode\":1,\"PlatformOfflineId\":\"\",\"PlatformOnlineId\":\"\",\"PlayFabId\":\"" + random_uuid().substr(0, 16) +
        "\",\"SelfSignedId\":\"" + random_uuid() + "\",\"ServerAddress\":\"" + server + "\",\"ThirdPartyName\":\"" + json_escape(name) + "\",\"UIProfile\":0,\"IsEditorMode\":false,\"ClientIsEditorCapable\":false,"
        "\"ClientEditorConnectionIntent\":0,\"TrustedSkin\":false,\"OverrideSkin\":false,\"CompatibleWithClientSideChunkGen\":false,\"MaxViewDistance\":0,\"MemoryTier\":0,\"PlatformType\":0}";
    std::string clientjwt = jwt(client);
    std::string certificate = "{\"chain\":[\"\"]}";
    std::string auth = "{\"Certificate\":\"" + json_escape(certificate) + "\",\"AuthenticationType\":" + std::string(token_for ? "0" : "2") + ",\"Token\":\"" + token + "\"}";
    Wr inner;
    inner.le32(uint32_t(auth.size())).raw(auth.data(), auth.size()).le32(uint32_t(clientjwt.size())).raw(clientjwt.data(), clientjwt.size());
    Wr pl;
    pl.be32(uint32_t(protocol_)).varuint(inner.b.size()).raw(inner.b);
    send_packets({{1, pl.b}});
    return ident;
}

static std::string json_member(const std::string& js, const std::string& key) {
    size_t k = js.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    size_t c = js.find(':', k);
    size_t a = js.find('"', c);
    size_t b = js.find('"', a + 1);
    return js.substr(a + 1, b - a - 1);
}

void BedrockClient::start_encryption(const std::string& jwt) {
    size_t d1 = jwt.find('.'), d2 = jwt.find('.', d1 + 1);
    Bytes hb = b64decode(jwt.substr(0, d1)), pb = b64decode(jwt.substr(d1 + 1, d2 - d1 - 1));
    std::string header(hb.begin(), hb.end()), payload(pb.begin(), pb.end());
    Bytes salt = b64decode(json_member(payload, "salt"));
    Bytes spub = b64decode(json_member(header, "x5u"));
    const uint8_t* sp = spub.data();
    EVP_PKEY* peer = d2i_PUBKEY(nullptr, &sp, long(spub.size()));
    if (!peer) throw std::runtime_error("bad server key");
    EVP_PKEY_CTX* dctx = EVP_PKEY_CTX_new(key_, nullptr);
    EVP_PKEY_derive_init(dctx);
    EVP_PKEY_derive_set_peer(dctx, peer);
    size_t sl = 0;
    EVP_PKEY_derive(dctx, nullptr, &sl);
    Bytes secret(sl);
    EVP_PKEY_derive(dctx, secret.data(), &sl);
    secret.resize(sl);
    EVP_PKEY_CTX_free(dctx);
    EVP_PKEY_free(peer);
    Bytes in = salt;
    in.insert(in.end(), secret.begin(), secret.end());
    uint8_t h[32];
    SHA256(in.data(), in.size(), h);
    enc_key_.assign(h, h + 32);
    uint8_t iv[16];
    memcpy(iv, h, 12);
    iv[12] = iv[13] = iv[14] = 0;
    iv[15] = 2;
    enc_ = EVP_CIPHER_CTX_new();
    dec_ = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(enc_, EVP_aes_256_ctr(), nullptr, h, iv);
    EVP_DecryptInit_ex(dec_, EVP_aes_256_ctr(), nullptr, h, iv);
}

Bytes BedrockClient::zz_pstr(const std::string& s) const { return Wr().bstr(s).b; }

// ---- scripted player --------------------------------------------------------------------------------------

BedBot::BedBot(int port, const std::string& n) : c("127.0.0.1", port), name(n) {
    c.connect();
    c.request_settings();
    c.login(n, "127.0.0.1:" + std::to_string(port));
}

bool BedBot::step(int timeout_ms) {
    std::vector<BPacket> pk;
    if (!c.recv_packets(pk, timeout_ms)) return false;
    for (auto& p : pk) {
        Rd r(p.d);
        if (p.id == 3) {
            uint64_t ln = r.varuint();
            c.start_encryption(std::string(reinterpret_cast<const char*>(r.p), size_t(ln)));
            c.send_packets({{4, {}}});
        } else if (p.id == 6) c.send_packets({{8, Wr().varuint(2).bstr("downloadingfinished").b}});
        else if (p.id == 7) c.send_packets({{8, Wr().varuint(3).bstr("resourcepackstackfinished").b}});
        else if (p.id == 11) {
            r.varuint();
            rt = int64_t(r.varuint());
            r.varuint();
            pos[0] = r.lef32(); pos[1] = r.lef32(); pos[2] = r.lef32();
            has_pos = true;
            c.send_packets({{129, {0}}, {69, Wr().zz32(4).u8(4).b}});
        } else if (p.id == 45 && p.d.size() >= 13 && p.d[12] == 1) {
            Rd q(p.d);
            pos[0] = q.lef32(); pos[1] = q.lef32(); pos[2] = q.lef32();
        } else if (p.id == 58) {
            chunks++;
            Rd q(p.d);
            q.varuint(); q.varuint();
            chunk_dims[int(q.varuint())]++;
        } else if (p.id == 61) {
            dim_changes[r.zz32()]++;
            log.push_back(p);
        } else if (p.id == 2 && p.d.size() == 4 && Rd(p.d).be32() == 3 && !spawned) {
            spawned = true;
            c.send_packets({{113, Wr().varuint(uint64_t(rt < 0 ? 1 : rt)).b}});
        } else log.push_back(p);
        if (on_packet) on_packet(p);
    }
    return true;
}

void BedBot::pump(int ms) {
    int64_t end = now_ms() + ms;
    for (;;) {
        int left = int(end - now_ms());
        if (left <= 0) return;
        step(std::min(left, 250));
    }
}

bool BedBot::until(const std::function<bool()>& pred, int timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    if (pred()) return true;
    for (;;) {
        int left = int(end - now_ms());
        if (left <= 0) return pred();
        if (step(std::min(left, 250)) && pred()) return true;
    }
}

std::vector<Bytes> BedBot::take(int pid) {
    std::vector<Bytes> out;
    std::deque<BPacket> keep;
    for (auto& p : log) { if (p.id == pid) out.push_back(std::move(p.d)); else keep.push_back(std::move(p)); }
    log.swap(keep);
    return out;
}

size_t BedBot::count(int pid) const {
    size_t n = 0;
    for (auto& p : log) if (p.id == pid) n++;
    return n;
}

void BedBot::auth_input(float x, float y, float z, float yaw, float pitch, uint64_t tick) {
    Wr d;
    d.lef32(pitch).lef32(yaw).lef32(x).lef32(y).lef32(z).lef32(0).lef32(0).lef32(yaw);
    d.varuint(0);   // no input flags (the Python test passed an empty tuple)
    d.varuint(1).varuint(0).varuint(2).lef32(0).lef32(0).varuint(tick).lef32(0).lef32(0).lef32(0);
    d.zeros(5).lef32(0).lef32(0).lef32(0).lef32(0).lef32(0).lef32(0).lef32(0);
    c.send_packets({{144, d.b}});
}

void BedBot::chat(const std::string& text) {
    Wr w;
    w.u8(0).u8(1).u8(1).bstr(name).bstr(text).bstr("").bstr("").u8(0);
    c.send_packets({{9, w.b}});
}

void BedBot::command(const std::string& text) {
    Wr w;
    w.bstr(text).u8(0).zeros(24);
    c.send_packets({{77, w.b}});
}

}  // namespace pt
