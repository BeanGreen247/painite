// Online-mode support: RSA key for the encryption handshake and Mojang session-server verification.
// Verification runs on its own threads (like Cuberite's authenticator) so worker loops never block on HTTPS.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct evp_pkey_st EVP_PKEY;

namespace pn {

struct AuthProp {
    std::string name, value, signature;
};

struct AuthRequest {
    uint64_t conn_id = 0;
    int worker_id = 0;
    std::string username, server_hash, ip;
};

struct AuthResult {
    bool ok = false;
    std::string uuid_hex;  // 32 hex chars
    std::string name;
    std::vector<AuthProp> props;
    std::string error;
};

class Auth {
public:
    ~Auth();
    bool init(const std::string& session_host, const std::string& ca_file, bool prevent_proxy, std::string& err);  // generates the RSA key
    const std::vector<uint8_t>& public_key_der() const { return der_; }
    bool rsa_decrypt(const uint8_t* in, size_t n, std::vector<uint8_t>& out) const;
    std::string server_hash(const uint8_t* secret, size_t n) const;
    bool prevent_proxy() const { return prevent_proxy_; }
    void start(int threads, std::function<void(const AuthRequest&, AuthResult&&)> done);
    void submit(AuthRequest r);
    void stop();

private:
    void loop();
    bool lookup(const AuthRequest& r, AuthResult& out);
    EVP_PKEY* key_ = nullptr;
    std::vector<uint8_t> der_;
    std::string host_, port_, ca_file_;
    bool prevent_proxy_ = false;
    std::function<void(const AuthRequest&, AuthResult&&)> done_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<AuthRequest> q_;
    std::vector<std::thread> th_;
    bool stop_ = false;
};

}  // namespace pn
