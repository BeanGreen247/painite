// Online mode against local fake services: Java (encryption, session server hasJoined) and Bedrock (Xbox-signed identity tokens).
#include "bclient.hpp"
#include "fakehttps.hpp"
#include "harness.hpp"
#include <cmath>
#include <mutex>
#include <unistd.h>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

namespace pt {

namespace {

std::string mc_digest(const Bytes& data) {
    uint8_t h[20];
    SHA1(data.data(), data.size(), h);
    bool neg = h[0] & 0x80;
    if (neg) {   // two's complement negate
        int carry = 1;
        for (int i = 19; i >= 0; i--) { int v = (uint8_t(~h[i])) + carry; h[i] = uint8_t(v); carry = v >> 8; }
    }
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
    std::string s = hex;
    size_t nz = s.find_first_not_of('0');
    s = nz == std::string::npos ? "0" : s.substr(nz);
    return neg ? "-" + s : s;
}

std::string query_param(const std::string& path, const std::string& key) {
    size_t q = path.find('?');
    if (q == std::string::npos) return "";
    std::string rest = path.substr(q + 1);
    size_t pos = 0;
    while (pos < rest.size()) {
        size_t amp = rest.find('&', pos);
        std::string kv = rest.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? "" : kv.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

Bytes rsa_encrypt(const Bytes& der_pub, const Bytes& msg) {
    const uint8_t* p = der_pub.data();
    EVP_PKEY* k = d2i_PUBKEY(nullptr, &p, long(der_pub.size()));
    if (!k) throw std::runtime_error("bad RSA public key");
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(k, nullptr);
    EVP_PKEY_encrypt_init(c);
    EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_PADDING);
    size_t n = 0;
    EVP_PKEY_encrypt(c, nullptr, &n, msg.data(), msg.size());
    Bytes out(n);
    EVP_PKEY_encrypt(c, out.data(), &n, msg.data(), msg.size());
    out.resize(n);
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(k);
    return out;
}

ServerOpts none(const Env&) { return {}; }

void java_online(Ctx& t, Env& e) {
    auto ids = e.ids("26.3");
    std::mutex m;
    std::string expect, req_user;
    std::string tmp = "/tmp/painite-online-" + std::to_string(getpid());
    system(("mkdir -p " + tmp).c_str());
    FakeHttps fake(tmp, [&](const std::string& path) -> std::pair<int, std::string> {
        std::lock_guard<std::mutex> l(m);
        std::string user = query_param(path, "username"), sid = query_param(path, "serverId");
        req_user = user;
        if (user == "BadGuy" || sid != expect) return {204, ""};
        return {200, "{\"id\":\"069a79f444e94726a5befca90e38aaf5\",\"name\":\"" + user + "\",\"properties\":[{\"name\":\"textures\",\"value\":\"dGV4dA==\",\"signature\":\"c2ln\"}]}"};
    });
    ServerOpts o;
    o.properties = "online-mode=true\n";
    o.bedrock = false;
    o.toml = "[auth]\nsession_host = \"localhost:" + std::to_string(fake.port) + "\"\nsession_ca_file = \"" + fake.cert_path + "\"\n";
    Server srv(e.binary, "online", o);
    for (auto pr : {std::make_pair("Online1", true), std::make_pair("BadGuy", false)}) {
        std::string name = pr.first;
        bool expect_ok = pr.second;
        JClient c;
        c.connect(srv.port);
        c.send(0, Wr().varint(777).str("localhost").be16(uint16_t(srv.port)).varint(2).b);
        c.send(0, Wr().str(name).zeros(16).b);
        Packet p;
        if (!c.recv(p, 8000)) { t.check(false, name + ": no answer to login start"); continue; }
        t.check(p.id == 1, name + ": encryption request");
        if (p.id != 1) continue;
        Rd r(p.d);
        r.skip(r.varint());   // server id
        Bytes pub = r.take(r.varint());
        Bytes token = r.take(r.varint());
        t.check(r.u8() == 1, name + ": should-authenticate flag set");
        uint8_t secret[16];
        RAND_bytes(secret, 16);
        {
            std::lock_guard<std::mutex> l(m);
            Bytes in(secret, secret + 16);
            in.insert(in.end(), pub.begin(), pub.end());
            expect = mc_digest(in);
        }
        Bytes es = rsa_encrypt(pub, Bytes(secret, secret + 16)), et = rsa_encrypt(pub, token);
        c.send(1, Wr().varint(uint32_t(es.size())).raw(es).varint(uint32_t(et.size())).raw(et).b);
        c.start_crypto(secret);
        if (!c.recv(p, 8000)) { t.check(false, name + ": nothing after the encryption response"); continue; }
        while (p.id == 3) { c.threshold = int(Rd(p.d).varint()); if (!c.recv(p, 8000)) break; }
        if (expect_ok) {
            t.check(p.id == 2, name + ": login success received over the encrypted stream");
            char u[33];
            for (int i = 0; i < 16 && i < int(p.d.size()); i++) snprintf(u + i * 2, 3, "%02x", p.d[size_t(i)]);
            t.check(std::string(u) == "069a79f444e94726a5befca90e38aaf5", "uuid taken from the session server");
            Rd q(p.d);
            q.skip(16);
            t.check(q.str() == name, "name from session server");
            t.check(q.varint() == 1, "skin property forwarded");
            c.send(3);
            int got = 0;
            bool finished = false;
            while (!finished) {
                if (!c.recv(p, 8000)) break;
                got++;
                if (p.id == (*ids)("configuration", "c", "select_known_packs")) c.send((*ids)("configuration", "s", "select_known_packs"), p.d);
                else if (p.id == (*ids)("configuration", "c", "finish_configuration")) finished = true;
            }
            t.check(finished && got > 10, "full configuration (" + std::to_string(got) + " packets) over the encrypted stream");
            c.send((*ids)("configuration", "s", "finish_configuration"));
            bool play = c.recv(p, 8000) && p.id == (*ids)("play", "c", "login");
            t.check(play, "play login after configuration");
        } else {
            t.check(p.id == 0 && contains(p.d, "Failed to verify username"), name + ": rejected when the session server answers 204");
        }
    }
    {
        std::lock_guard<std::mutex> l(m);
        t.check(req_user == "Online1" || req_user == "BadGuy", "session server was queried");
    }
}

// ---- Bedrock: Xbox identity tokens -------------------------------------------------------------------------------

std::string b64u_str(const std::string& s) {
    std::string o = b64encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    for (auto& c : o) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    while (!o.empty() && o.back() == '=') o.pop_back();
    return o;
}
std::string b64u_bytes(const Bytes& b) { return b64u_str(std::string(b.begin(), b.end())); }

std::string rs256_token(EVP_PKEY* key, const std::string& payload) {
    std::string h = b64u_str("{\"alg\":\"RS256\",\"kid\":\"TESTKEY\",\"typ\":\"JWT\"}"), p = b64u_str(payload);
    std::string signing = h + "." + p;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key);
    size_t sl = 0;
    EVP_DigestSign(ctx, nullptr, &sl, reinterpret_cast<const uint8_t*>(signing.data()), signing.size());
    Bytes sig(sl);
    EVP_DigestSign(ctx, sig.data(), &sl, reinterpret_cast<const uint8_t*>(signing.data()), signing.size());
    sig.resize(sl);
    EVP_MD_CTX_free(ctx);
    return signing + "." + b64u_bytes(sig);
}

void xbox_login(Ctx& t, Env& e) {
    EVP_PKEY* signing = EVP_RSA_gen(2048);
    EVP_PKEY* other = EVP_RSA_gen(2048);
    BIGNUM *n = nullptr, *ex = nullptr;
    EVP_PKEY_get_bn_param(signing, "n", &n);
    EVP_PKEY_get_bn_param(signing, "e", &ex);
    Bytes nb(256), eb(3);
    BN_bn2binpad(n, nb.data(), 256);
    BN_bn2binpad(ex, eb.data(), 3);
    BN_free(n);
    BN_free(ex);
    std::string jwks = "{\"keys\":[{\"kty\":\"RSA\",\"use\":\"sig\",\"kid\":\"TESTKEY\",\"n\":\"" + b64u_bytes(nb) + "\",\"e\":\"" + b64u_bytes(eb) + "\"}]}";
    std::string tmp = "/tmp/painite-xbox-" + std::to_string(getpid());
    system(("mkdir -p " + tmp).c_str());
    FakeHttps fake(tmp, [&](const std::string& path) -> std::pair<int, std::string> {
        if (path.size() >= 17 && path.compare(path.size() - 17, 17, "/.well-known/keys") == 0) return {200, jwks};
        return {200, "{}"};
    });
    std::string host = "localhost:" + std::to_string(fake.port);
    ServerOpts o;
    o.properties = "online-mode=true\n";
    o.toml = "[auth]\nxbox_host = \"" + host + "\"\nsession_ca_file = \"" + fake.cert_path + "\"\n";
    Server srv(e.binary, "xbox", o);
    auto token = [&](EVP_PKEY* key, const std::string& gamertag, const std::string& xuid, const std::string& cpk_override, int exp_delta, const std::string& iss) {
        return [=](const std::string& pub) {
            int64_t now = unix_ms() / 1000;
            char buf[1024];
            snprintf(buf, sizeof buf, "{\"iss\":\"%s\",\"aud\":\"api://auth-minecraft-services/multiplayer\",\"xid\":\"%s\",\"xname\":\"%s\",\"cpk\":\"%s\",\"nbf\":%lld,\"iat\":%lld,\"exp\":%lld}",
                     iss.empty() ? ("https://" + host + "/").c_str() : iss.c_str(), xuid.c_str(), gamertag.c_str(), cpk_override.empty() ? pub.c_str() : cpk_override.c_str(),
                     (long long)now - 10, (long long)now, (long long)now + exp_delta);
            return rs256_token(key, buf);
        };
    };
    auto attempt = [&](const std::string& label, std::function<std::string(const std::string&)> tok, bool expect_ok) {
        BedrockClient c("127.0.0.1", srv.port);
        c.connect();
        c.request_settings();
        c.login("Xbx" + label.substr(0, 4), "127.0.0.1:" + std::to_string(srv.port), tok);
        bool got_ok = false, got_bad = false;
        std::string msg;
        int64_t end = now_ms() + 10000;
        while (now_ms() < end && !(got_ok || got_bad)) {
            std::vector<BPacket> pk;
            if (!c.recv_packets(pk, 500)) continue;
            for (auto& p : pk) {
                if (p.id == 3) got_ok = true;   // the server-to-client handshake means the login was accepted
                else if (p.id == 5) { got_bad = true; if (p.d.size() > 3) msg.assign(p.d.begin() + 3, p.d.end()); }
            }
        }
        t.check(got_ok == expect_ok && got_bad == !expect_ok, label + ": " + (expect_ok ? "accepted" : "refused") + (got_bad ? " (" + msg.substr(0, 70) + ")" : ""));
    };
    attempt("valid", token(signing, "RealGamer", "2535400000000001", "", 3600, ""), true);
    attempt("forged signature", token(other, "Forger", "2535400000000002", "", 3600, ""), false);
    attempt("expired", token(signing, "Slowpoke", "2535400000000003", "", -7200, ""), false);
    attempt("unbound key", token(signing, "Thief", "2535400000000004", "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEfake", 3600, ""), false);
    attempt("wrong issuer", token(signing, "Impostor", "2535400000000005", "", 3600, "https://evil.example/"), false);
    EVP_PKEY_free(signing);
    EVP_PKEY_free(other);
}

}  // namespace

void reg_online(Registry& reg) {
    Group a;
    a.name = "java-online";
    a.needs_server = false;
    a.opts = none;
    a.tests.push_back({"session-server", java_online});
    reg.push_back(std::move(a));
    Group b;
    b.name = "bedrock-online";
    b.needs_server = false;
    b.opts = none;
    b.tests.push_back({"xbox-tokens", xbox_login});
    reg.push_back(std::move(b));
}

}  // namespace pt
