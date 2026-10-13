#include "rak.hpp"
#include "util.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>

namespace pn {

static const uint8_t kMagic[16] = {0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};
static constexpr int kRakProtocol = 11;
static constexpr size_t kUdpOverhead = 48;       // IPv6 + UDP headers; also safe for IPv4
static constexpr size_t kMaxQueued = 64u << 20;
static constexpr uint32_t kMaxSplit = 4096;

static bool magic_at(const uint8_t* p, size_t n, size_t off) { return n >= off + 16 && memcmp(p + off, kMagic, 16) == 0; }

static void put_addr(Wr& w, const sockaddr_storage& a) {
    if (a.ss_family == AF_INET6) {
        auto* s = reinterpret_cast<const sockaddr_in6*>(&a);
        if (IN6_IS_ADDR_V4MAPPED(&s->sin6_addr)) {
            w.u8(4);
            for (int i = 12; i < 16; i++) w.u8(static_cast<uint8_t>(~s->sin6_addr.s6_addr[i]));
            w.be16(ntohs(s->sin6_port));
        } else {
            w.u8(6);
            w.le16(23);
            w.be16(ntohs(s->sin6_port));
            w.be32(0);
            w.raw(s->sin6_addr.s6_addr, 16);
            w.be32(0);
        }
    } else {
        auto* s = reinterpret_cast<const sockaddr_in*>(&a);
        w.u8(4);
        const uint8_t* ip = reinterpret_cast<const uint8_t*>(&s->sin_addr);
        for (int i = 0; i < 4; i++) w.u8(static_cast<uint8_t>(~ip[i]));
        w.be16(ntohs(s->sin_port));
    }
}

size_t RakSession::queued_bytes() const {
    size_t n = 0;
    for (auto& f : out_q) n += f.payload.size();
    return n;
}

std::string Rak::key_of(const sockaddr_storage& a) {
    std::string k;
    if (a.ss_family == AF_INET6) {
        auto* s = reinterpret_cast<const sockaddr_in6*>(&a);
        k.assign(reinterpret_cast<const char*>(s->sin6_addr.s6_addr), 16);
        k.append(reinterpret_cast<const char*>(&s->sin6_port), 2);
    } else {
        auto* s = reinterpret_cast<const sockaddr_in*>(&a);
        k.assign(reinterpret_cast<const char*>(&s->sin_addr), 4);
        k.append(reinterpret_cast<const char*>(&s->sin_port), 2);
    }
    return k;
}

void Rak::send_raw(RakSession& s, const uint8_t* p, size_t n) { sendto(fd_, p, n, 0, reinterpret_cast<const sockaddr*>(&s.addr), s.alen); }

void Rak::on_datagram(const sockaddr_storage& from, socklen_t fl, const uint8_t* p, size_t n, int64_t now) {
    if (n == 0) return;
    uint8_t id = p[0];
    auto reply = [&](const Wr& w) { sendto(fd_, w.b.data(), w.b.size(), 0, reinterpret_cast<const sockaddr*>(&from), fl); };
    if (id == 0x01 || id == 0x02) {
        if (!magic_at(p, n, 9) || n < 25) return;
        Rd r(p + 1, n - 1);
        uint64_t t = r.be64();
        Wr w;
        w.u8(0x1c);
        w.be64(t);
        w.be64(guid_);
        w.raw(kMagic, 16);
        w.be16(static_cast<uint16_t>(pong_.size()));
        w.raw(pong_.data(), pong_.size());
        reply(w);
        return;
    }
    if (id == 0x05) {
        if (!magic_at(p, n, 1) || n < 18) return;
        Wr w;
        if (p[17] != kRakProtocol) {
            w.u8(0x19);
            w.u8(kRakProtocol);
            w.raw(kMagic, 16);
            w.be64(guid_);
        } else {
            int mtu = std::min<int>(static_cast<int>(n) + 28, 1492);
            w.u8(0x06);
            w.raw(kMagic, 16);
            w.be64(guid_);
            w.u8(0);
            w.be16(static_cast<uint16_t>(mtu));
        }
        reply(w);
        return;
    }
    if (id == 0x07) {
        if (!magic_at(p, n, 1)) return;
        Rd r(p + 17, n - 17);
        uint8_t ver = r.u8();
        r.skip(ver == 4 ? 6 : 26);
        uint16_t mtu = r.be16();
        uint64_t cguid = r.be64();
        if (r.bad || mtu < 400) return;
        if (sessions_.size() >= 2048) return;
        std::string key = key_of(from);
        auto it = sessions_.find(key);
        if (it != sessions_.end()) close(*it->second, false);
        auto s = std::make_unique<RakSession>();
        s->addr = from;
        s->alen = fl;
        s->key = key;
        s->mtu = std::min<uint16_t>(mtu, 1492);
        s->guid = cguid;
        s->last_rx_ms = s->last_ping_ms = now;
        Wr w;
        w.u8(0x08);
        w.raw(kMagic, 16);
        w.be64(guid_);
        put_addr(w, from);
        w.be16(s->mtu);
        w.u8(0);
        reply(w);
        sessions_[key] = std::move(s);
        return;
    }
    auto it = sessions_.find(key_of(from));
    if (it == sessions_.end()) return;
    RakSession& s = *it->second;
    if (s.closed) return;
    s.last_rx_ms = now;
    if (id == 0xC0 || id == 0xA0) { handle_ack(s, p + 1, n - 1, id == 0xA0, now); return; }
    if (!(id & 0x80) || n < 4) return;
    uint32_t seq = p[1] | (p[2] << 8) | (p[3] << 16);
    if (static_cast<int64_t>(seq) > s.highest_seq + 1)
        for (int64_t q = s.highest_seq + 1; q < static_cast<int64_t>(seq) && s.nack_q.size() < 256; q++) s.nack_q.push_back(static_cast<uint32_t>(q));
    if (static_cast<int64_t>(seq) > s.highest_seq) s.highest_seq = seq;
    s.ack_q.push_back(seq);
    size_t i = 4;
    while (i + 3 <= n && !s.closed) {
        uint8_t fl2 = p[i];
        uint8_t rel = fl2 >> 5;
        bool split = fl2 & 0x10;
        size_t len = ((static_cast<size_t>(p[i + 1]) << 8 | p[i + 2]) + 7) / 8;
        i += 3;
        uint32_t rel_idx = 0, ord_idx = 0;
        bool has_rel = rel == 2 || rel == 3 || rel == 4 || rel == 6 || rel == 7;
        bool has_ord = rel == 1 || rel == 3 || rel == 4 || rel == 7;
        if (has_rel) { if (i + 3 > n) break; rel_idx = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16); i += 3; }
        if (rel == 1 || rel == 4) { i += 3; }
        if (has_ord) { if (i + 4 > n) break; ord_idx = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16); i += 4; }
        uint32_t sc = 0, si = 0;
        uint16_t sid = 0;
        if (split) { if (i + 10 > n) break; sc = (p[i] << 24) | (p[i + 1] << 16) | (p[i + 2] << 8) | p[i + 3]; sid = static_cast<uint16_t>(p[i + 4] << 8 | p[i + 5]); si = (p[i + 6] << 24) | (p[i + 7] << 16) | (p[i + 8] << 8) | p[i + 9]; i += 10; }
        if (i + len > n) break;
        const uint8_t* body = p + i;
        i += len;
        if (has_rel) {
            if (rel_idx < s.rel_floor || s.rel_seen.count(rel_idx)) continue;
            s.rel_seen.insert(rel_idx);
            while (s.rel_seen.count(s.rel_floor)) { s.rel_seen.erase(s.rel_floor); s.rel_floor++; }
            if (s.rel_seen.size() > 65536) { close(s, false); break; }
        }
        std::vector<uint8_t> whole;
        const uint8_t* wp = body;
        size_t wn = len;
        if (split) {
            if (sc == 0 || sc > kMaxSplit || si >= sc) continue;
            auto& sp = s.splits[sid];
            sp.n = sc;
            sp.parts[si].assign(body, body + len);
            if (sp.parts.size() < sc) { if (s.splits.size() > 64) { close(s, false); break; } continue; }
            for (auto& kv : sp.parts) whole.insert(whole.end(), kv.second.begin(), kv.second.end());
            s.splits.erase(sid);
            wp = whole.data();
            wn = whole.size();
        }
        if (has_ord) {
            if (ord_idx < s.next_ord) continue;
            s.ord_buf[ord_idx].assign(wp, wp + wn);
            if (s.ord_buf.size() > 4096) { close(s, false); break; }
            while (!s.ord_buf.empty() && s.ord_buf.begin()->first == s.next_ord && !s.closed) {
                auto v = std::move(s.ord_buf.begin()->second);
                s.ord_buf.erase(s.ord_buf.begin());
                s.next_ord++;
                handle_frame_payload(s, v.data(), v.size(), now);
            }
        } else {
            handle_frame_payload(s, wp, wn, now);
        }
    }
    if (!s.closed) { send_acks(s); flush(s, now); }
}

void Rak::handle_frame_payload(RakSession& s, const uint8_t* p, size_t n, int64_t now) {
    if (n == 0) return;
    if (p[0] == 0xFE) { if (s.connected && on_payload_) on_payload_(s, p + 1, n - 1); return; }
    handle_connected(s, p, n, now);
}

void Rak::handle_connected(RakSession& s, const uint8_t* p, size_t n, int64_t now) {
    uint8_t id = p[0];
    if (id == 0x00 && n >= 9) {
        Wr w;
        w.u8(0x03);
        w.raw(p + 1, 8);
        w.be64(static_cast<uint64_t>(now));
        send(s, w.b.data(), w.b.size(), false);
    } else if (id == 0x03 && n >= 9) {
        Rd r(p + 1, 8);
        int64_t sent = static_cast<int64_t>(r.be64());
        int64_t rtt = now - sent;
        if (rtt >= 0 && rtt < 10000) s.srtt_ms = (s.srtt_ms * 7 + rtt) / 8;
    } else if (id == 0x09 && n >= 17) {
        Rd r(p + 9, n - 9);
        uint64_t t = r.be64();
        Wr w;
        w.u8(0x10);
        put_addr(w, s.addr);
        w.be16(0);
        for (int i = 0; i < 10; i++) { w.u8(4); w.u8(0x80); w.u8(0xff); w.u8(0xff); w.u8(0xfe); w.be16(0); }
        w.be64(t);
        w.be64(static_cast<uint64_t>(now));
        send(s, w.b.data(), w.b.size(), true);
    } else if (id == 0x13) {
        s.connected = true;
    } else if (id == 0x15) {
        close(s, false);
    }
}

void Rak::send(RakSession& s, const uint8_t* p, size_t n, bool reliable) {
    if (s.closed) return;
    if (s.queued_bytes() > kMaxQueued) { close(s, false); return; }
    size_t maxp = s.mtu - kUdpOverhead - 4 - 24;
    size_t parts = n == 0 ? 1 : (n + maxp - 1) / maxp;
    uint16_t sid = s.split_id;
    if (parts > 1) s.split_id++;
    uint32_t ord = s.ord_idx;
    for (size_t k = 0; k < parts; k++) {
        RakFrame f;
        f.rel = reliable ? 3 : 0;
        size_t off = k * maxp, len = std::min(maxp, n - off);
        f.payload.assign(p + off, p + off + len);
        if (reliable) { f.rel_idx = s.rel_idx++; f.ord_idx = ord; }
        if (parts > 1) { f.split = true; f.split_count = static_cast<uint32_t>(parts); f.split_index = static_cast<uint32_t>(k); f.split_id = sid; }
        s.out_q.push_back(std::move(f));
    }
    if (reliable) s.ord_idx++;
}

static size_t frame_size(const RakFrame& f) {
    size_t h = 3;
    if (f.rel == 2 || f.rel == 3 || f.rel == 4 || f.rel == 6 || f.rel == 7) h += 3;
    if (f.rel == 1 || f.rel == 3 || f.rel == 4 || f.rel == 7) h += 4;
    if (f.split) h += 10;
    return h + f.payload.size();
}

static void encode_frame(std::vector<uint8_t>& o, const RakFrame& f) {
    Wr w;
    w.u8(static_cast<uint8_t>((f.rel << 5) | (f.split ? 0x10 : 0)));
    w.be16(static_cast<uint16_t>(f.payload.size() * 8));
    if (f.rel == 2 || f.rel == 3 || f.rel == 4 || f.rel == 6 || f.rel == 7) { w.u8(f.rel_idx); w.u8(f.rel_idx >> 8); w.u8(f.rel_idx >> 16); }
    if (f.rel == 1 || f.rel == 3 || f.rel == 4 || f.rel == 7) { w.u8(f.ord_idx); w.u8(f.ord_idx >> 8); w.u8(f.ord_idx >> 16); w.u8(0); }
    if (f.split) { w.be32(f.split_count); w.be16(f.split_id); w.be32(f.split_index); }
    o.insert(o.end(), w.b.begin(), w.b.end());
    o.insert(o.end(), f.payload.begin(), f.payload.end());
}

void Rak::flush(RakSession& s, int64_t now) {
    size_t budget = s.mtu - kUdpOverhead;
    while (!s.out_q.empty() && s.inflight.size() < static_cast<size_t>(s.cwnd)) {
        std::vector<uint8_t> d;
        d.reserve(s.mtu);
        uint32_t seq = s.send_seq++;
        d.push_back(0x84);
        d.push_back(static_cast<uint8_t>(seq));
        d.push_back(static_cast<uint8_t>(seq >> 8));
        d.push_back(static_cast<uint8_t>(seq >> 16));
        RakSession::Inflight inf{{}, now, 0};
        bool reliable = false;
        while (!s.out_q.empty() && (inf.frames.empty() || d.size() + frame_size(s.out_q.front()) <= budget)) {
            RakFrame f = std::move(s.out_q.front());
            s.out_q.pop_front();
            encode_frame(d, f);
            if (f.rel != 0) reliable = true;
            inf.frames.push_back(std::move(f));
        }
        send_raw(s, d.data(), d.size());
        if (reliable) s.inflight.emplace(seq, std::move(inf));
    }
}

void Rak::send_acks(RakSession& s) {
    auto emit = [&](std::vector<uint32_t>& q, uint8_t id) {
        if (q.empty()) return;
        std::sort(q.begin(), q.end());
        q.erase(std::unique(q.begin(), q.end()), q.end());
        Wr body;
        uint16_t count = 0;
        size_t i = 0;
        while (i < q.size()) {
            size_t j = i;
            while (j + 1 < q.size() && q[j + 1] == q[j] + 1) j++;
            if (i == j) { body.u8(1); body.u8(q[i]); body.u8(q[i] >> 8); body.u8(q[i] >> 16); }
            else { body.u8(0); body.u8(q[i]); body.u8(q[i] >> 8); body.u8(q[i] >> 16); body.u8(q[j]); body.u8(q[j] >> 8); body.u8(q[j] >> 16); }
            count++;
            i = j + 1;
        }
        Wr w;
        w.u8(id);
        w.be16(count);
        w.raw(body.b.data(), body.b.size());
        send_raw(s, w.b.data(), w.b.size());
        q.clear();
    };
    emit(s.ack_q, 0xC0);
    emit(s.nack_q, 0xA0);
}

void Rak::requeue(RakSession& s, uint32_t seq, bool shrink) {
    auto it = s.inflight.find(seq);
    if (it == s.inflight.end()) return;
    auto& fr = it->second.frames;
    for (auto r = fr.rbegin(); r != fr.rend(); ++r) s.out_q.push_front(std::move(*r));
    s.inflight.erase(it);
    if (shrink) { s.ssthresh = std::max(s.cwnd / 2, 4.0); s.cwnd = s.ssthresh; }
}

void Rak::handle_ack(RakSession& s, const uint8_t* p, size_t n, bool nack, int64_t now) {
    if (n < 2) return;
    uint16_t count = static_cast<uint16_t>(p[0] << 8 | p[1]);
    size_t i = 2;
    for (uint16_t c = 0; c < count && i < n; c++) {
        bool single = p[i++];
        if (i + 3 > n) return;
        uint32_t lo = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16);
        uint32_t hi = lo;
        i += 3;
        if (!single) { if (i + 3 > n) return; hi = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16); i += 3; }
        if (hi - lo > 8192) return;
        for (uint32_t q = lo; q <= hi; q++) {
            auto it = s.inflight.find(q);
            if (it == s.inflight.end()) continue;
            if (nack) { requeue(s, q, true); continue; }
            int64_t rtt = now - it->second.sent_ms;
            if (it->second.resends == 0 && rtt >= 0) s.srtt_ms = (s.srtt_ms * 7 + rtt) / 8;
            s.inflight.erase(it);
            if (s.cwnd < s.ssthresh) s.cwnd += 1; else s.cwnd += 1.0 / s.cwnd;
            if (s.cwnd > 1024) s.cwnd = 1024;
        }
    }
    flush(s, now);
}

void Rak::close(RakSession& s, bool notify_peer) {
    if (s.closed) return;
    if (notify_peer) {
        uint8_t b = 0x15;
        send(s, &b, 1, true);
        flush(s, s.last_rx_ms);
    }
    s.closed = true;
    dead_.push_back(s.key);
}

void Rak::tick(int64_t now) {
    for (auto& kv : sessions_) {
        RakSession& s = *kv.second;
        if (s.closed) continue;
        int64_t idle = now - s.last_rx_ms;
        if (idle > (s.connected ? 30000 : 10000)) { close(s, false); continue; }
        // retransmit anything the peer has not acknowledged within a smoothed RTT-based timeout
        int64_t rto = std::max<int64_t>(250, s.srtt_ms * 3);
        bool shrunk = false;
        std::vector<uint32_t> late;
        for (auto& inf : s.inflight) if (now - inf.second.sent_ms > rto) late.push_back(inf.first);
        for (uint32_t q : late) {
            auto it = s.inflight.find(q);
            if (it != s.inflight.end() && ++it->second.resends > 20) { close(s, false); break; }
            requeue(s, q, !shrunk);
            shrunk = true;
        }
        if (s.closed) continue;
        if (s.connected && now - s.last_ping_ms > 5000) {
            s.last_ping_ms = now;
            Wr w;
            w.u8(0x00);
            w.be64(static_cast<uint64_t>(now));
            send(s, w.b.data(), w.b.size(), false);
        }
        send_acks(s);
        flush(s, now);
    }
    for (auto& k : dead_) {
        auto it = sessions_.find(k);
        if (it == sessions_.end()) continue;
        if (on_close_) on_close_(*it->second);
        sessions_.erase(it);
    }
    dead_.clear();
}

}  // namespace pn
