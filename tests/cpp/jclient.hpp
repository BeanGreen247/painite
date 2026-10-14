// Java Edition test client: framing, compression, optional AES/CFB8, plus the packet id tables from data/vanilla-*.ids.
#pragma once
#include "util.hpp"
#include <deque>
#include <map>
#include <memory>
#include <openssl/evp.h>
#include <string>

namespace pt {

struct Ids {
    std::map<std::string, int> id;                     // "play/c/level_chunk_with_light" -> number
    std::map<std::string, std::map<int, std::string>> names;   // "play/c" -> number -> name
    std::map<std::string, int> items;                  // item name -> registry id
    std::map<std::string, int> entity_types;
    int protocol = 0;
    static std::shared_ptr<Ids> load(const std::string& path);
    int operator()(const char* state, const char* dir, const std::string& name) const;   // aborts when unknown
    bool has(const std::string& key) const { return id.count(key) != 0; }
    std::string name(const char* state, const char* dir, int pid) const;
};

struct Packet {
    int id = 0;
    Bytes d;
};

class JClient {
public:
    JClient() = default;
    ~JClient();
    JClient(const JClient&) = delete;
    void connect(int port, int timeout_ms = 15000);
    void close();
    void send(int pid, const Bytes& data = {});
    bool recv(Packet& out, int timeout_ms);    // false on timeout; throws when the server closed the connection
    void start_crypto(const uint8_t key[16]);
    int threshold = -1;
    bool eof = false;
    int fd() const { return fd_; }

private:
    bool fill(size_t need, int deadline_ms);
    void write_all(const Bytes& b);
    int fd_ = -1;
    Bytes buf_;
    size_t pos_ = 0;
    EVP_CIPHER_CTX* enc_ = nullptr;
    EVP_CIPHER_CTX* dec_ = nullptr;
};

// Handshake + login (offline mode). Throws with the server's reason when refused.
Bytes java_login(JClient& c, int port, int protocol, const std::string& name, const Bytes& uuid16);   // returns the login success payload
// Configuration phase until play starts (answers known packs / keep alive).
int java_configure(JClient& c, const Ids& ids);   // returns the number of packets seen
Bytes offline_uuid(const std::string& name);

}  // namespace pt
