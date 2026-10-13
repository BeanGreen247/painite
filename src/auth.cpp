#include "auth.hpp"
#include "hash.hpp"
#include "log.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pn {

// ---- minimal JSON (enough for the hasJoined response) ----
struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    std::string s;
    std::vector<JV> a;
    std::vector<std::pair<std::string, JV>> o;
    const JV* get(const char* k) const {
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct JP {
    const char* p;
    const char* e;
    bool bad = false;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) { o += static_cast<char>(0xc0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += static_cast<char>(0xe0 | (c >> 12)); o += static_cast<char>(0x80 | ((c >> 6) & 63)); o += static_cast<char>(0x80 | (c & 63)); }
        else { o += static_cast<char>(0xf0 | (c >> 18)); o += static_cast<char>(0x80 | ((c >> 12) & 63)); o += static_cast<char>(0x80 | ((c >> 6) & 63)); o += static_cast<char>(0x80 | (c & 63)); }
    }
    std::string str() {
        std::string o;
        if (p >= e || *p != '"') { bad = true; return o; }
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                    case 'n': o += '\n'; break;
                    case 't': o += '\t'; break;
                    case 'r': o += '\r'; break;
                    case 'b': o += '\b'; break;
                    case 'f': o += '\f'; break;
                    case 'u': {
                        if (e - p < 5) { bad = true; return o; }
                        uint32_t c = static_cast<uint32_t>(strtoul(std::string(p + 1, 4).c_str(), nullptr, 16));
                        p += 4;
                        utf8(o, c);
                        break;
                    }
                    default: o += *p;
                }
                p++;
            } else o += *p++;
        }
        if (p >= e) bad = true; else p++;
        return o;
    }
    JV val(int depth = 0) {
        JV v;
        if (depth > 16) { bad = true; return v; }
        ws();
        if (p >= e) { bad = true; return v; }
        if (*p == '{') {
            v.t = JV::Obj; p++; ws();
            if (p < e && *p == '}') { p++; return v; }
            while (!bad) {
                ws();
                std::string k = str();
                ws();
                if (p >= e || *p != ':') { bad = true; break; }
                p++;
                v.o.emplace_back(std::move(k), val(depth + 1));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                bad = true;
            }
        } else if (*p == '[') {
            v.t = JV::Arr; p++; ws();
            if (p < e && *p == ']') { p++; return v; }
            while (!bad) {
                v.a.push_back(val(depth + 1));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                bad = true;
            }
        } else if (*p == '"') { v.t = JV::Str; v.s = str(); }
        else {
            const char* b = p;
            while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n') p++;
            v.s.assign(b, p);
            v.t = (v.s == "true" || v.s == "false") ? JV::Bool : v.s == "null" ? JV::Null : JV::Num;
        }
        return v;
    }
};

Auth::~Auth() {
    stop();
    if (key_) EVP_PKEY_free(key_);
}

bool Auth::init(const std::string& session_host, const std::string& ca_file, bool prevent_proxy, std::string& err) {
    host_ = session_host.empty() ? "sessionserver.mojang.com" : session_host;
    port_ = "443";
    if (size_t c = host_.rfind(':'); c != std::string::npos) { port_ = host_.substr(c + 1); host_.resize(c); }
    ca_file_ = ca_file;
    prevent_proxy_ = prevent_proxy;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 1024) <= 0 || EVP_PKEY_keygen(ctx, &key_) <= 0) {
        err = "RSA key generation failed";
        if (ctx) EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);
    int n = i2d_PUBKEY(key_, nullptr);
    if (n <= 0) { err = "cannot encode public key"; return false; }
    der_.resize(static_cast<size_t>(n));
    uint8_t* p = der_.data();
    i2d_PUBKEY(key_, &p);
    return true;
}

bool Auth::rsa_decrypt(const uint8_t* in, size_t n, std::vector<uint8_t>& out) const {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(key_, nullptr);
    if (!ctx) return false;
    bool ok = false;
    size_t olen = 0;
    if (EVP_PKEY_decrypt_init(ctx) > 0 && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) > 0 && EVP_PKEY_decrypt(ctx, nullptr, &olen, in, n) > 0) {
        out.resize(olen);
        if (EVP_PKEY_decrypt(ctx, out.data(), &olen, in, n) > 0) { out.resize(olen); ok = true; }
    }
    EVP_PKEY_CTX_free(ctx);
    ERR_clear_error();
    return ok;
}

// Minecraft's "twos-complement hex digest": SHA-1 as a signed big-endian integer.
std::string Auth::server_hash(const uint8_t* secret, size_t n) const {
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    uint8_t d[20];
    unsigned dl = 0;
    EVP_DigestInit_ex(c, EVP_sha1(), nullptr);
    EVP_DigestUpdate(c, secret, n);  // server id is always empty
    EVP_DigestUpdate(c, der_.data(), der_.size());
    EVP_DigestFinal_ex(c, d, &dl);
    EVP_MD_CTX_free(c);
    return mc_hex_digest(d);
}

void Auth::start(int threads, std::function<void(const AuthRequest&, AuthResult&&)> done) {
    done_ = std::move(done);
    for (int i = 0; i < threads; i++) th_.emplace_back([this] { loop(); });
}

void Auth::submit(AuthRequest r) {
    {
        std::lock_guard<std::mutex> l(mu_);
        q_.push_back(std::move(r));
    }
    cv_.notify_one();
}

void Auth::stop() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : th_) if (t.joinable()) t.join();
    th_.clear();
}

void Auth::loop() {
    for (;;) {
        AuthRequest r;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return stop_ || !q_.empty(); });
            if (stop_) return;
            r = std::move(q_.front());
            q_.pop_front();
        }
        AuthResult res;
        lookup(r, res);
        done_(r, std::move(res));
    }
}

static std::string url_enc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

bool Auth::lookup(const AuthRequest& r, AuthResult& out) {
    std::string path = "/session/minecraft/hasJoined?username=" + url_enc(r.username) + "&serverId=" + url_enc(r.server_hash);
    if (prevent_proxy_ && !r.ip.empty()) path += "&ip=" + url_enc(r.ip);

    addrinfo hints{}, *ai = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &ai) != 0 || !ai) { out.error = "session server lookup failed"; return false; }
    int fd = -1;
    for (addrinfo* a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) { out.error = "session server unreachable"; return false; }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ca_file_.empty()) SSL_CTX_set_default_verify_paths(ctx);
    else SSL_CTX_load_verify_locations(ctx, ca_file_.c_str(), nullptr);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host_.c_str());
    SSL_set1_host(ssl, host_.c_str());
    std::string body, resp;
    int status = 0;
    if (SSL_connect(ssl) == 1) {
        std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host_ + "\r\nUser-Agent: Painite\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
        SSL_write(ssl, req.data(), static_cast<int>(req.size()));
        char buf[4096];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof buf)) > 0 && resp.size() < 256 * 1024) resp.append(buf, static_cast<size_t>(n));
        size_t sp = resp.find(' ');
        if (sp != std::string::npos) status = atoi(resp.c_str() + sp + 1);
        size_t hb = resp.find("\r\n\r\n");
        if (hb != std::string::npos) body = resp.substr(hb + 4);
    } else out.error = "TLS handshake with session server failed";
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    ERR_clear_error();
    if (status == 204) { out.error = "not authenticated"; return false; }
    if (status != 200) { if (out.error.empty()) out.error = "session server returned " + std::to_string(status); return false; }
    JP jp{body.data(), body.data() + body.size()};
    JV v = jp.val();
    const JV* id = v.get("id");
    const JV* name = v.get("name");
    if (jp.bad || v.t != JV::Obj || !id || !name || id->s.size() != 32) { out.error = "bad session server response"; return false; }
    out.uuid_hex = id->s;
    out.name = name->s;
    if (const JV* pr = v.get("properties"))
        for (auto& e : pr->a) {
            AuthProp ap;
            if (auto* n = e.get("name")) ap.name = n->s;
            if (auto* x = e.get("value")) ap.value = x->s;
            if (auto* s = e.get("signature")) ap.signature = s->s;
            out.props.push_back(std::move(ap));
        }
    out.ok = true;
    return true;
}

}  // namespace pn
