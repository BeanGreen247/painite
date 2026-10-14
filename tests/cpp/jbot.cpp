#include "jbot.hpp"
#include <cstdio>
#include <cstdlib>

namespace pt {

JBot::JBot(int port, int proto, std::shared_ptr<Ids> i, const std::string& n) : ids(std::move(i)), name(n), protocol(proto) {
    uuid = offline_uuid(n);
    java_login(c, port, proto, n, uuid);
    java_configure(c, *ids);
}

bool JBot::step(int timeout_ms) {
    Packet p;
    if (!c.recv(p, timeout_ms)) return false;
    std::string nm = ids->name("play", "c", p.id);
    static const bool trace = getenv("PT_TRACE") != nullptr;
    if (trace && nm != "level_chunk_with_light") { fprintf(stderr, "[%s] %s (%zu bytes)\n", name.c_str(), nm.c_str(), p.d.size()); if (nm == "disconnect") { std::string t; for (uint8_t ch : p.d) t.push_back(ch >= 32 && ch < 127 ? char(ch) : '.'); fprintf(stderr, "   %s\n", t.c_str()); } }
    if (nm == "keep_alive") c.send(S("keep_alive"), p.d);
    else if (nm == "chunk_batch_finished") c.send(S("chunk_batch_received"), Wr().f32(25.0f).b);
    else if (nm == "player_position") {
        Rd r(p.d);
        uint32_t tid = r.varint();
        pos[0] = r.f64(); pos[1] = r.f64(); pos[2] = r.f64();
        r.f64(); r.f64(); r.f64();
        float yaw = r.f32(), pitch = r.f32();
        has_pos = true;
        Wr ack;
        ack.varint(tid);
        if (protocol >= 777) ack.f64(pos[0]).f64(pos[1]).f64(pos[2]).f32(yaw).f32(pitch);   // 26.3 confirms with the position it was given
        c.send(S("accept_teleportation"), ack.b);
        log.emplace_back(nm, p.d);
    } else if (nm == "level_chunk_with_light") { chunks++; last_chunk = p.d; if (on_chunk) on_chunk(p.d); }
    else if (nm == "respawn") { chunks = 0; last_chunk.clear(); log.emplace_back(nm, p.d); }   // a new world: the old chunks are gone
    else log.emplace_back(nm, p.d);
    return true;
}

void JBot::pump(int ms) {
    int64_t end = now_ms() + ms;
    for (;;) {
        int left = int(end - now_ms());
        if (left <= 0) return;
        step(left);
    }
}

bool JBot::until(const std::function<bool()>& pred, int timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    if (pred()) return true;
    for (;;) {
        int left = int(end - now_ms());
        if (left <= 0) return pred();
        if (step(left) && pred()) return true;
    }
}

void JBot::settle(int quiet_ms, int max_ms) {
    int64_t end = now_ms() + max_ms;
    while (now_ms() < end) if (!step(quiet_ms)) return;
}

size_t JBot::count(const std::string& n) const {
    size_t k = 0;
    for (auto& e : log) if (e.first == n) k++;
    return k;
}

std::vector<Bytes> JBot::take(const std::string& n) {
    std::vector<Bytes> out;
    std::deque<std::pair<std::string, Bytes>> keep;
    for (auto& e : log) { if (e.first == n) out.push_back(std::move(e.second)); else keep.push_back(std::move(e)); }
    log.swap(keep);
    return out;
}

}  // namespace pt
