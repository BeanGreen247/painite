// RakNet session layer for Bedrock: reliability, ordering, split packets, ACK/NACK, resends and a small
// congestion window. One Rak object per UDP socket; it owns the sessions and hands payloads to a callback.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pn {

struct RakFrame {
    uint8_t rel = 3;                 // reliability 0..7 (we use 0 unreliable and 3 reliable ordered)
    uint32_t rel_idx = 0, ord_idx = 0;
    bool split = false;
    uint32_t split_count = 0, split_index = 0;
    uint16_t split_id = 0;
    std::vector<uint8_t> payload;
};

struct RakSession {
    sockaddr_storage addr{};
    socklen_t alen = 0;
    std::string key;
    uint16_t mtu = 1400;
    uint64_t guid = 0;
    bool connected = false, closed = false;
    int64_t last_rx_ms = 0, last_ping_ms = 0;

    // receive side
    uint32_t rel_floor = 0;
    std::unordered_set<uint32_t> rel_seen;
    uint32_t next_ord = 0;
    std::map<uint32_t, std::vector<uint8_t>> ord_buf;
    struct Split { uint32_t n = 0; std::map<uint32_t, std::vector<uint8_t>> parts; };
    std::unordered_map<uint16_t, Split> splits;
    int64_t highest_seq = -1;
    std::vector<uint32_t> ack_q, nack_q;

    // send side
    uint32_t send_seq = 0, rel_idx = 0, ord_idx = 0;
    uint16_t split_id = 0;
    std::deque<RakFrame> out_q;
    struct Inflight { std::vector<RakFrame> frames; int64_t sent_ms; int resends; };
    std::map<uint32_t, Inflight> inflight;
    double cwnd = 16, ssthresh = 256;
    int64_t srtt_ms = 120;

    void* user = nullptr;            // owner's per-session state
    size_t queued_bytes() const;
};

class Rak {
public:
    using OnPayload = std::function<void(RakSession&, const uint8_t*, size_t)>;
    using OnClose = std::function<void(RakSession&)>;
    Rak(int fd, uint64_t guid, std::string pong_info, OnPayload pl, OnClose cl)
        : fd_(fd), guid_(guid), pong_(std::move(pong_info)), on_payload_(std::move(pl)), on_close_(std::move(cl)) {}
    void set_pong(std::string s) { pong_ = std::move(s); }
    void on_datagram(const sockaddr_storage& from, socklen_t fl, const uint8_t* p, size_t n, int64_t now);
    void tick(int64_t now);
    void send(RakSession& s, const uint8_t* p, size_t n, bool reliable = true);
    void flush(RakSession& s, int64_t now);
    void close(RakSession& s, bool notify_peer);
    size_t session_count() const { return sessions_.size(); }
    template <class F> void for_each(F f) { for (auto& kv : sessions_) f(*kv.second); }

private:
    void handle_connected(RakSession& s, const uint8_t* p, size_t n, int64_t now);
    void handle_frame_payload(RakSession& s, const uint8_t* p, size_t n, int64_t now);
    void send_raw(RakSession& s, const uint8_t* p, size_t n);
    void send_acks(RakSession& s);
    void handle_ack(RakSession& s, const uint8_t* p, size_t n, bool nack, int64_t now);
    void requeue(RakSession& s, uint32_t seq, bool shrink);
    static std::string key_of(const sockaddr_storage& a);
    int fd_;
    uint64_t guid_;
    std::string pong_;
    OnPayload on_payload_;
    OnClose on_close_;
    std::unordered_map<std::string, std::unique_ptr<RakSession>> sessions_;
    std::vector<std::string> dead_;
};

}  // namespace pn
