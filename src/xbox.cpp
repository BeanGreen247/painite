#include "xbox.hpp"
#include "json.hpp"
#include "log.hpp"
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace pn {

namespace {


std::string b64url_decode(const std::string& s) {
    static int8_t t[256];
    static bool init = false;
    if (!init) {
        memset(t, -1, sizeof t);
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; i++) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        t[static_cast<uint8_t>('+')] = 62;
        t[static_cast<uint8_t>('/')] = 63;
        init = true;
    }
    std::string o;
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=') break;
        int8_t v = t[c];
        if (v < 0) return {};
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back(static_cast<char>((acc >> bits) & 0xff)); }
    }
    return o;
}

struct Key {
    EVP_PKEY* pkey = nullptr;
};

std::mutex g_mu;
std::map<std::string, EVP_PKEY*> g_keys;   // kid -> RSA public key
int64_t g_fetched = 0, g_tried = 0;

EVP_PKEY* rsa_from_jwk(const std::string& n_b64, const std::string& e_b64) {
    std::string n = b64url_decode(n_b64), e = b64url_decode(e_b64);
    if (n.empty() || e.empty()) return nullptr;
    BIGNUM* bn = BN_bin2bn(reinterpret_cast<const uint8_t*>(n.data()), static_cast<int>(n.size()), nullptr);
    BIGNUM* be = BN_bin2bn(reinterpret_cast<const uint8_t*>(e.data()), static_cast<int>(e.size()), nullptr);
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn);
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, be);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY* pk = nullptr;
    if (!ctx || EVP_PKEY_fromdata_init(ctx) <= 0 || EVP_PKEY_fromdata(ctx, &pk, EVP_PKEY_PUBLIC_KEY, params) <= 0) pk = nullptr;
    if (ctx) EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    BN_free(bn);
    BN_free(be);
    return pk;
}

bool refresh_keys(const Auth& auth, std::string& err) {
    int status = 0;
    std::string body;
    if (!auth.https_get(auth.xbox_host(), "/.well-known/keys", status, body, err)) return false;
    if (status != 200) { err = "key server returned " + std::to_string(status); return false; }
    JP jp{body.data(), body.data() + body.size()};
    JV v = jp.val();
    const JV* keys = v.get("keys");
    if (jp.bad || !keys || keys->t != JV::Arr) { err = "bad key set"; return false; }
    std::map<std::string, EVP_PKEY*> fresh;
    for (auto& k : keys->a) {
        const JV *kid = k.get("kid"), *kty = k.get("kty"), *n = k.get("n"), *e = k.get("e");
        if (!kid || !kty || kty->s != "RSA" || !n || !e) continue;
        if (EVP_PKEY* pk = rsa_from_jwk(n->s, e->s)) fresh[kid->s] = pk;
    }
    if (fresh.empty()) { err = "no usable signing keys"; return false; }
    for (auto& kv : g_keys) EVP_PKEY_free(kv.second);
    g_keys.swap(fresh);
    g_fetched = time(nullptr);
    return true;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> o;
    size_t i = 0;
    while (true) {
        size_t d = s.find('.', i);
        o.push_back(s.substr(i, d == std::string::npos ? std::string::npos : d - i));
        if (d == std::string::npos) break;
        i = d + 1;
    }
    return o;
}

}  // namespace

XboxResult xbox_verify(const Auth& auth, const std::string& token, const std::string& client_pub_b64) {
    XboxResult r;
    auto parts = split(token);
    if (parts.size() != 3) { r.error = "malformed token"; return r; }
    std::string hdr = b64url_decode(parts[0]), pl = b64url_decode(parts[1]), sig = b64url_decode(parts[2]);
    JP hp{hdr.data(), hdr.data() + hdr.size()}, pp{pl.data(), pl.data() + pl.size()};
    JV h = hp.val(), p = pp.val();
    if (hp.bad || pp.bad || h.t != JV::Obj || p.t != JV::Obj) { r.error = "malformed token"; return r; }
    const JV *alg = h.get("alg"), *kid = h.get("kid");
    if (!alg || alg->s != "RS256" || !kid) { r.error = "unsupported token (alg " + (alg ? alg->s : std::string("?")) + ")"; return r; }
    EVP_PKEY* key = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mu);
        int64_t now = time(nullptr);
        auto find = [&]() { auto it = g_keys.find(kid->s); return it == g_keys.end() ? nullptr : it->second; };
        key = find();
        if ((!key && now - g_tried > 30) || now - g_fetched > 6 * 3600) {
            g_tried = now;
            std::string err;
            if (!refresh_keys(auth, err) && !key) { r.error = "cannot fetch signing keys: " + err; return r; }
            key = find();
        }
        if (!key) { r.error = "unknown signing key"; return r; }
        // verify under the lock: the key object lives in the cache and may be replaced by another thread
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        std::string signing = parts[0] + "." + parts[1];
        bool ok = EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, key) > 0 &&
                  EVP_DigestVerify(md, reinterpret_cast<const uint8_t*>(sig.data()), sig.size(), reinterpret_cast<const uint8_t*>(signing.data()), signing.size()) == 1;
        EVP_MD_CTX_free(md);
        if (!ok) { r.error = "bad token signature"; return r; }
    }
    auto num = [&](const char* k) -> int64_t { const JV* v = p.get(k); return v && v->t == JV::Num ? strtoll(v->s.c_str(), nullptr, 10) : 0; };
    auto str = [&](const char* k) -> std::string { const JV* v = p.get(k); return v && v->t == JV::Str ? v->s : std::string(); };
    int64_t now = time(nullptr);
    if (str("iss") != "https://" + auth.xbox_host() + "/") { r.error = "token not issued by Minecraft services"; return r; }
    if (num("exp") && num("exp") < now - 60) { r.error = "token expired"; return r; }
    if (num("nbf") && num("nbf") > now + 60) { r.error = "token not valid yet"; return r; }
    if (str("cpk") != client_pub_b64) { r.error = "token is not bound to this client's key"; return r; }
    r.xuid = str("xid");
    r.gamertag = str("xname");
    if (r.xuid.empty() || r.gamertag.empty()) { r.error = "token carries no Xbox identity"; return r; }
    r.ok = true;
    return r;
}

}  // namespace pn
