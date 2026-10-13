// Bedrock Edition server side: batch/compression/encryption, login, the recorded join sequence and chunk streaming.
#pragma once
#include "rak.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace pn {

class Server;
struct GEvent;

struct BedrockData {
    int protocol = 0;
    std::vector<uint8_t> info, stack;                      // ResourcePacksInfo / ResourcePackStack payloads
    struct Segment { bool patch_player_list = false; std::vector<uint8_t> batch; };  // batch = algorithm byte + deflate
    std::vector<Segment> start;                            // replayed after the resource pack handshake
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> extras;                   // per-player packets sent just before spawn
    std::vector<uint8_t> player_list_raw;                  // PlayerList payload to patch (uuid/name)
    std::vector<uint8_t> chunk_tail;                       // LevelChunk payload after the x/z varints
    std::unordered_map<int, std::vector<uint8_t>> subchunks;  // sub chunk index (0 = y -4) -> entry bytes after the 3-byte offset
    std::string capture_name;
};

bool load_bedrock_data(const std::string& cfg_dir, const std::string& exe_dir, int level, BedrockData& out, std::string& err);

struct BSession {
    enum class St { Raw, Settings, LoginWait, Handshake, Packs, Stack, Start, Spawning, Playing } st = St::Raw;
    RakSession* rak = nullptr;
    bool compress = false;
    int threshold = 256;
    std::string name, xuid;
    uint8_t uuid[16] = {};
    bool reserved = false, counted = false;
    int radius = 0;
    int cx = 0, cz = 0;
    float x = 0.5f, z = 0.5f;
    std::unordered_set<int64_t> sent;
    std::vector<std::pair<int, int>> pending;   // chunks still to send, nearest last
    bool spawn_sent = false;
    // encryption
    EVP_CIPHER_CTX* enc = nullptr;
    EVP_CIPHER_CTX* dec = nullptr;
    uint8_t key[32] = {};
    uint64_t send_ctr = 0, recv_ctr = 0;
    ~BSession();
};

class Bedrock {
public:
    Bedrock(Server& s, int fd, BedrockData&& data, bool has_data);
    ~Bedrock();
    void on_datagram(const sockaddr_storage& from, socklen_t fl, const uint8_t* p, size_t n, int64_t now);
    void tick(int64_t now);
    void broadcast_chat(const std::string& text);   // worker thread only
    void on_game_event(uint64_t conn, const GEvent& ev);
    void chat_to(uint64_t conn, const std::string& text);
    void kick(uint64_t conn, const std::string& reason);
    void shutdown();
    size_t sessions() const { return rak_->session_count(); }
    int playing() const { return playing_; }

private:
    void on_payload(RakSession& r, const uint8_t* p, size_t n);
    void on_packet(BSession& b, uint16_t id, const uint8_t* p, size_t n);
    void on_login(BSession& b, const uint8_t* p, size_t n);
    void after_handshake(BSession& b);
    void on_pack_response(BSession& b, const uint8_t* p, size_t n);
    void on_radius(BSession& b, const uint8_t* p, size_t n);
    void on_subchunk_request(BSession& b, const uint8_t* p, size_t n);
    void on_text(BSession& b, const uint8_t* p, size_t n);
    void on_move(BSession& b, float x, float z);
    void send_start(BSession& b);
    void begin_chunks(BSession& b);
    void queue_chunks(BSession& b);
    void pump_chunks(BSession& b);
    void send_npu(BSession& b);
    void finish_spawn(BSession& b);
    void on_close(RakSession& r);
    void send_packets(BSession& b, const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& pk);
    void send_batch(BSession& b, const std::vector<uint8_t>& compressed_with_algo);
    void send_raw_body(BSession& b, std::vector<uint8_t> data);
    void send_text(BSession& b, const std::string& text);
    void disconnect(BSession& b, const std::string& msg);
    bool start_encryption(BSession& b, const std::string& client_pub_b64, std::string& jwt);
    std::vector<uint8_t> patched_player_list(const BSession& b) const;
    Server& s_;
    BedrockData d_;
    std::unique_ptr<Rak> rak_;
    EVP_PKEY* server_key_ = nullptr;
    std::string server_pub_b64_;
    bool has_data_ = true;
    int playing_ = 0;
    int64_t last_pong_ms_ = 0;
};

}  // namespace pn
