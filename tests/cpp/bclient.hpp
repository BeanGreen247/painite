// Bedrock Edition test client: RakNet session layer, batches with raw deflate, optional encryption, offline (self-signed) login.
#pragma once
#include "util.hpp"
#include <map>
#include <set>
#include <deque>
#include <functional>
#include <openssl/evp.h>

namespace pt {

class RakClient {
public:
    RakClient(const std::string& host, int port, int mtu = 1400);
    ~RakClient();
    RakClient(const RakClient&) = delete;
    void connect();                         // throws when the server does not answer
    void send_frame(const Bytes& payload, int rel = 3);
    bool poll(int timeout_ms);              // handles one datagram; false on timeout
    void close_socket();
    bool connected = false, closed = false;
    std::deque<Bytes> game;                 // received 0xfe payloads
    int mtu;

private:
    bool recv_raw(Bytes& out, int timeout_ms);
    Bytes make_frame(const Bytes& payload, int rel, const uint8_t* split);
    void send_datagram(const Bytes& frames);
    void ack(uint32_t seq);
    void handle_payload(const Bytes& p);
    void resend_stale();
    int fd_ = -1;
    uint8_t addr_[16];
    std::string host_;
    int port_;
    uint64_t guid_;
    uint32_t seq_ = 0, rel_ = 0, ordidx_ = 0, split_id_ = 0;
    std::map<uint32_t, std::pair<Bytes, int64_t>> sent_;
    std::set<uint32_t> seen_rel_;
    uint32_t next_order_ = 0;
    std::map<uint32_t, Bytes> order_buf_;
    struct Split { uint32_t n = 0; std::map<uint32_t, Bytes> parts; };
    std::map<uint32_t, Split> splits_;
};

struct BPacket {
    int id;
    Bytes d;
};

class BedrockClient : public RakClient {
public:
    BedrockClient(const std::string& host, int port, int protocol = 2193) : RakClient(host, port), protocol_(protocol) {}
    ~BedrockClient();
    void request_settings();
    // token_for(public key) supplies the identity token of an online-mode login (AuthenticationType 0); without it a self-signed offline token is used
    std::string login(const std::string& name, const std::string& server, std::function<std::string(const std::string&)> token_for = nullptr);   // returns the identity uuid
    void start_encryption(const std::string& handshake_jwt);
    void send_packets(const std::vector<BPacket>& pk);
    bool recv_packets(std::vector<BPacket>& out, int timeout_ms);   // false on timeout
    Bytes zz_pstr(const std::string& s) const;

private:
    Bytes trailer(const Bytes& data, uint64_t counter) const;
    int protocol_;
    bool compress_ = false;
    int threshold_ = 0, algo_ = 0;
    EVP_PKEY* key_ = nullptr;
    EVP_CIPHER_CTX* enc_ = nullptr;
    EVP_CIPHER_CTX* dec_ = nullptr;
    Bytes enc_key_;
    uint64_t send_count_ = 0, recv_count_ = 0;
};

// A scripted Bedrock player: answers the join handshake and queues the packets a test cares about.
class BedBot {
public:
    BedBot(int port, const std::string& name);
    BedrockClient c;
    std::string name;
    int64_t rt = -1;
    bool has_pos = false, spawned = false;
    float pos[3] = {0, 0, 0};
    std::deque<BPacket> log;
    void pump(int ms);
    bool until(const std::function<bool()>& pred, int timeout_ms);
    std::vector<Bytes> take(int pid);
    size_t count(int pid) const;
    void send(int pid, const Bytes& d) { c.send_packets({{pid, d}}); }
    void auth_input(float x, float y, float z, float yaw, float pitch, uint64_t tick);
    void chat(const std::string& text);
    void command(const std::string& text);
    bool step(int timeout_ms);
    std::function<void(const BPacket&)> on_packet;   // optional extra handler
    int chunks = 0;
    std::map<int, int> chunk_dims;     // dimension number of each LevelChunk -> count
    std::map<int, int> dim_changes;    // ChangeDimension targets seen
};

std::string b64encode(const uint8_t* d, size_t n);
Bytes b64decode(const std::string& s);

}  // namespace pt
