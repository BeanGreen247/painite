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

struct GPos;
class Server;
struct GEvent;
struct GIdentity;
struct XboxResult;

struct BedrockData {
    int protocol = 0;
    std::vector<uint8_t> info, stack;                      // ResourcePacksInfo / ResourcePackStack payloads
    // batch = algorithm byte + deflate; patch: 0 none, 1 PlayerList, 2 StartGame, 3 Respawn (raw holds the packet payload)
    struct Segment { bool patch_player_list = false; int patch = 0; std::vector<uint8_t> raw; std::vector<uint8_t> batch; };
    std::vector<Segment> start;                            // replayed after the resource pack handshake
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> extras;                   // per-player packets sent just before spawn
    std::vector<uint8_t> player_list_raw;                  // PlayerList payload to patch (uuid/name)
    std::vector<uint8_t> chunk_tail;                       // LevelChunk payload after the x/z varints
    std::unordered_map<int, std::vector<uint8_t>> subchunks;  // sub chunk index (0 = y -4) -> entry bytes after the 3-byte offset
    std::string capture_name;
    std::vector<uint8_t> add_player_tpl;                  // AddPlayer packet recorded from a real BDS (kind 8), patched per player
};

// replaces a length-prefixed string inside a packet
std::vector<uint8_t> replace_string_bytes(const std::vector<uint8_t>& in, const std::string& from, const std::string& to);

bool load_bedrock_data(const std::string& cfg_dir, const std::string& exe_dir, int level, BedrockData& out, std::string& err);

struct BSession {
    enum class St { Raw, Settings, LoginWait, Authing, Handshake, Packs, Stack, Start, Spawning, Playing } st = St::Raw;
    RakSession* rak = nullptr;
    bool compress = false;
    int threshold = 256;
    std::string name, xuid, pending_pub;
    std::vector<uint8_t> skin;   // Bedrock Skin structure from the client's login data
    uint8_t uuid[16] = {};
    bool reserved = false, counted = false;
    int radius = 0;
    int cx = 0, cz = 0;
    uint64_t id = 0;         // connection id (valid on this worker)
    uint64_t gid = 0;        // Game registry id once the player spawned
    std::shared_ptr<GIdentity> ident;
    int dim = 0;             // dimension the player is in
    int eid = 0;             // entity id, both unique and runtime id
    double x = 0.5, y = 0, z = 0.5;   // feet position
    float yaw = 0, pitch = 0;
    int gamemode = 0;
    bool sneaking = false;
    uint64_t break_tick_ = 0;
    int held = 0;
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
    void on_xbox_result(uint64_t id, const XboxResult& res);
    void block_event(uint64_t conn, int x, int y, int z, uint16_t block);
    size_t sessions() const { return rak_->session_count(); }
    int playing() const { return playing_; }

private:
    void on_payload(RakSession& r, const uint8_t* p, size_t n);
    void on_packet(BSession& b, uint16_t id, const uint8_t* p, size_t n);
    void on_login(BSession& b, const uint8_t* p, size_t n);
    void continue_login(BSession& b, const std::string& name, const std::string& uuid_s, const std::string& xuid, const std::string& client_pub);
    void after_handshake(BSession& b);
    void on_pack_response(BSession& b, const uint8_t* p, size_t n);
    void on_radius(BSession& b, const uint8_t* p, size_t n);
    void on_subchunk_request(BSession& b, const uint8_t* p, size_t n);
    void on_text(BSession& b, const uint8_t* p, size_t n);
    void on_move(BSession& b, float x, float z);
    void on_auth_input(BSession& b, const uint8_t* p, size_t n);
    void on_command_request(BSession& b, const uint8_t* p, size_t n);
    void begin_play(BSession& b);
    BSession* by_id(uint64_t id);
    std::vector<uint8_t> patched_start_game(const BSession& b, const std::vector<uint8_t>& raw) const;
    std::vector<uint8_t> patched_respawn(const BSession& b, const std::vector<uint8_t>& raw) const;
    std::vector<uint8_t> player_list_entry(const GIdentity& id, bool add) const;
    void send_time_to(BSession& b, int64_t day_time);
    void send_game_mode(BSession& b, int mode);
    const std::vector<uint8_t>& level_chunk_tail(int dim, int x, int z);
    void change_dimension(BSession& b, const GPos& p);
    std::unordered_map<uint64_t, BSession*> by_id_;
    struct CachedTail { uint32_t ver = 0; std::vector<uint8_t> tail; };
    std::unordered_map<int64_t, CachedTail> tail_cache_;
    size_t tail_cache_bytes_ = 0;
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
    std::vector<uint8_t> player_list_for(const uint8_t uuid[16], int64_t unique_id, const std::string& name, const std::string& xuid, const std::vector<uint8_t>* skin) const;
    std::vector<uint8_t> default_skin_;
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
