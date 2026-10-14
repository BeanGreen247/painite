// A scripted Java player: joins, answers keep alives / chunk batches / teleports, and queues everything else by packet name.
#pragma once
#include "jclient.hpp"
#include <functional>

namespace pt {

class JBot {
public:
    JBot(int port, int protocol, std::shared_ptr<Ids> ids, const std::string& name);
    JClient c;
    std::shared_ptr<Ids> ids;
    std::string name;
    int protocol;
    Bytes uuid;
    bool has_pos = false;
    double pos[3] = {0, 0, 0};
    int chunks = 0;
    Bytes last_chunk;
    std::deque<std::pair<std::string, Bytes>> log;
    std::function<void(const Bytes&)> on_chunk;   // called for every level chunk packet

    int S(const char* n) const { return (*ids)("play", "s", n); }
    void send(const char* name, const Bytes& d = {}) { c.send(S(name), d); }
    void command(const std::string& cmd) { send("chat_command", Wr().str(cmd).b); }
    // reads for a fixed time
    void pump(int ms);
    // reads until `pred` holds (checked after every packet) or the timeout; returns the predicate's last value
    bool until(const std::function<bool()>& pred, int timeout_ms);
    // reads until nothing arrives for `quiet_ms` (at most `max_ms`)
    void settle(int quiet_ms = 120, int max_ms = 3000);
    size_t count(const std::string& n) const;
    bool wait_for(const std::string& n, size_t count, int timeout_ms) { return until([&] { return this->count(n) >= count; }, timeout_ms); }
    std::vector<Bytes> take(const std::string& n);
    void drop(const std::string& n) { take(n); }
    bool step(int timeout_ms);   // handles one packet
};

}  // namespace pt
