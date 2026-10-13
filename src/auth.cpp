#include "auth.hpp"
#include "hash.hpp"
#include "json.hpp"
#include "log.hpp"
#include "skin.hpp"
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

void Auth::post_task(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> l(mu_);
        tasks_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void Auth::loop() {
    for (;;) {
        AuthRequest r;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return stop_ || !q_.empty() || !tasks_.empty(); });
            if (stop_) return;
            if (!tasks_.empty()) {
                auto t = std::move(tasks_.front());
                tasks_.pop_front();
                l.unlock();
                t();
                continue;
            }
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

bool Auth::https_get(const std::string& host_port, const std::string& path, int& status, std::string& body, std::string& err) const {
    std::string host = host_port, port = "443";
    if (size_t c = host.rfind(':'); c != std::string::npos) { port = host.substr(c + 1); host.resize(c); }
    addrinfo hints{}, *ai = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &ai) != 0 || !ai) { err = "lookup of " + host + " failed"; return false; }
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
    if (fd < 0) { err = host + " unreachable"; return false; }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ca_file_.empty()) SSL_CTX_set_default_verify_paths(ctx);
    else SSL_CTX_load_verify_locations(ctx, ca_file_.c_str(), nullptr);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());
    std::string resp;
    status = 0;
    bool ok = false;
    if (SSL_connect(ssl) == 1) {
        std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: Painite\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
        SSL_write(ssl, req.data(), static_cast<int>(req.size()));
        char buf[4096];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof buf)) > 0 && resp.size() < 1024 * 1024) resp.append(buf, static_cast<size_t>(n));
        size_t sp = resp.find(' ');
        if (sp != std::string::npos) status = atoi(resp.c_str() + sp + 1);
        size_t hb = resp.find("\r\n\r\n");
        body = hb != std::string::npos ? resp.substr(hb + 4) : std::string();
        ok = true;
    } else err = "TLS handshake with " + host + " failed";
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    ERR_clear_error();
    return ok;
}

bool Auth::lookup(const AuthRequest& r, AuthResult& out) {
    std::string path = "/session/minecraft/hasJoined?username=" + url_enc(r.username) + "&serverId=" + url_enc(r.server_hash);
    if (prevent_proxy_ && !r.ip.empty()) path += "&ip=" + url_enc(r.ip);
    std::string body, err;
    int status = 0;
    std::string hp = host_ + ":" + port_;
    if (!https_get(hp, path, status, body, err)) { out.error = err; return false; }
    if (status == 204) { out.error = "not authenticated"; return false; }
    if (status != 200) { out.error = "session server returned " + std::to_string(status); return false; }
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
    for (auto& pr : out.props) {
        if (pr.name != "textures") continue;
        std::string host, tpath, tb, terr;
        bool slim = false;
        int tstatus = 0;
        if (parse_textures_property(pr.value, host, tpath, slim) && https_get(host, tpath, tstatus, tb, terr) && tstatus == 200) {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (decode_png(reinterpret_cast<const uint8_t*>(tb.data()), tb.size(), w, h, rgba)) {
                upgrade_legacy_skin(w, h, rgba);
                if (w == 64 && h == 64) { out.skin_rgba = std::move(rgba); out.skin_slim = slim; }
            }
        }
        break;
    }
    return true;
}

}  // namespace pn
