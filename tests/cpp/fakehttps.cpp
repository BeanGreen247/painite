#include "fakehttps.hpp"
#include <arpa/inet.h>
#include <fstream>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace pt {

FakeHttps::FakeHttps(const std::string& dir, Handler h) : handler_(std::move(h)) {
    EVP_PKEY* key = EVP_RSA_gen(2048);
    X509* x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 86400);
    X509_set_pubkey(x, key);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const uint8_t*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
    X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, "DNS:localhost");
    X509_add_ext(x, san, -1);
    X509_EXTENSION_free(san);
    X509_sign(x, key, EVP_sha256());
    cert_path = dir + "/fake-ca.pem";
    {
        FILE* f = fopen(cert_path.c_str(), "w");
        PEM_write_X509(f, x);
        fclose(f);
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(ctx, x);
    SSL_CTX_use_PrivateKey(ctx, key);
    ctx_ = ctx;
    X509_free(x);
    EVP_PKEY_free(key);
    lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd_, 16) != 0) throw std::runtime_error("fake https server cannot listen");
    socklen_t sl = sizeof a;
    getsockname(lfd_, reinterpret_cast<sockaddr*>(&a), &sl);
    port = ntohs(a.sin_port);
    th_ = std::thread([this] { run(); });
}

FakeHttps::~FakeHttps() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    if (lfd_ >= 0) ::close(lfd_);
    if (ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
}

void FakeHttps::run() {
    SSL_CTX* ctx = static_cast<SSL_CTX*>(ctx_);
    while (!stop_) {
        pollfd pf{lfd_, POLLIN, 0};
        if (::poll(&pf, 1, 100) <= 0) continue;
        int fd = ::accept(lfd_, nullptr, nullptr);
        if (fd < 0) continue;
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) == 1) {
            std::string req;
            char buf[2048];
            while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
                int n = SSL_read(ssl, buf, sizeof buf);
                if (n <= 0) break;
                req.append(buf, size_t(n));
            }
            size_t a = req.find(' '), b = req.find(' ', a + 1);
            std::string path = a == std::string::npos || b == std::string::npos ? "" : req.substr(a + 1, b - a - 1);
            auto res = handler_(path);
            std::string head = "HTTP/1.1 " + std::to_string(res.first) + (res.first == 200 ? " OK" : res.first == 204 ? " No Content" : " Error") + "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(res.second.size()) + "\r\n\r\n";
            std::string out = head + res.second;
            SSL_write(ssl, out.data(), int(out.size()));
            SSL_shutdown(ssl);
        }
        SSL_free(ssl);
        ::close(fd);
    }
}

}  // namespace pt
