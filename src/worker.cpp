#include "server.hpp"
#include "log.hpp"
#include "bedrock.hpp"
#include "util.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pn {

static constexpr size_t kBaseConnCost = 16 * 1024;

int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

static int make_socket(const Config& cfg, int type, uint16_t port, std::string& err) {
    bool want_v6 = cfg.server_ip.empty();
    for (int attempt = 0; attempt < 2; attempt++) {
        int fam = (want_v6 && attempt == 0) ? AF_INET6 : AF_INET;
        int fd = socket(fam, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
        int rc;
        if (fam == AF_INET6) {
            int zero = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
            sockaddr_in6 a{};
            a.sin6_family = AF_INET6;
            a.sin6_addr = in6addr_any;
            a.sin6_port = htons(port);
            rc = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        } else {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = cfg.server_ip.empty() ? INADDR_ANY : inet_addr(cfg.server_ip.c_str());
            a.sin_port = htons(port);
            rc = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        }
        if (rc != 0) { err = std::string("bind port ") + std::to_string(port) + ": " + strerror(errno); close(fd); continue; }
        if (type == SOCK_STREAM && listen(fd, 1024) != 0) { err = strerror(errno); close(fd); continue; }
        return fd;
    }
    return -1;
}

Worker::Worker(Server& s, int id, int core, bool with_udp) : s_(s), id_(id), core_(core), with_udp_(with_udp) {
    scratch_.resize(64 * 1024);
}

Worker::~Worker() { join(); }

bool Worker::open_tcp(std::string& err) {
    tcp_ = make_socket(s_.cfg, SOCK_STREAM, static_cast<uint16_t>(s_.cfg.java_port), err);
    if (tcp_ < 0) return false;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = tcp_;
    epoll_ctl(ep_, EPOLL_CTL_ADD, tcp_, &ev);
    return true;
}

bool Worker::open_udp(std::string& err) {
    udp_ = make_socket(s_.cfg, SOCK_DGRAM, static_cast<uint16_t>(s_.cfg.bedrock_port_effective()), err);
    if (udp_ < 0) return false;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = udp_;
    epoll_ctl(ep_, EPOLL_CTL_ADD, udp_, &ev);
    BedrockData bd;
    std::string derr;
    bool ok = load_bedrock_data(s_.cfg.data_dir, s_.exe_dir, s_.cfg.compression_level, bd, derr);
    if (!ok) warn("Bedrock: " + derr + " (only the server-list ping is answered)");
    else s_.cfg.bedrock_protocol = bd.protocol;
    bedrock_ = std::make_unique<Bedrock>(s_, udp_, std::move(bd), ok);
    return true;
}

bool Worker::start(std::string& err) {
    ep_ = epoll_create1(EPOLL_CLOEXEC);
    evfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ep_ < 0 || evfd_ < 0) { err = "epoll/eventfd failed"; return false; }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = evfd_;
    epoll_ctl(ep_, EPOLL_CTL_ADD, evfd_, &ev);
    if (!open_tcp(err)) return false;
    if (with_udp_ && s_.cfg.bedrock && !open_udp(err)) return false;
    idle_since_ms = now_ms();
    th_ = std::thread([this] { run(); });
    return true;
}

void Worker::stop() {
    stop_ = true;
    if (evfd_ >= 0) { uint64_t one = 1; (void)!write(evfd_, &one, 8); }
}

void Worker::drain() {
    if (draining_.exchange(true)) return;
    if (tcp_ >= 0) { epoll_ctl(ep_, EPOLL_CTL_DEL, tcp_, nullptr); close(tcp_); tcp_ = -1; }
    uint64_t one = 1;
    (void)!write(evfd_, &one, 8);
}

void Worker::join() {
    if (th_.joinable()) th_.join();
    for (int* fd : {&tcp_, &udp_, &evfd_, &ep_}) if (*fd >= 0) { close(*fd); *fd = -1; }
}

void Worker::post_chat(const std::string& text) {
    {
        std::lock_guard<std::mutex> l(inbox_mu_);
        inbox_.push_back(text);
    }
    uint64_t one = 1;
    (void)!write(evfd_, &one, 8);
}

void Worker::post_fn(std::function<void(Worker&)> fn) {
    {
        std::lock_guard<std::mutex> l(inbox_mu_);
        fn_inbox_.push_back(std::move(fn));
    }
    uint64_t one = 1;
    (void)!write(evfd_, &one, 8);
}

JConn* Worker::conn_by_id(uint64_t id) {
    auto it = id_fd_.find(id);
    if (it == id_fd_.end()) return nullptr;
    auto c = conns_.find(it->second);
    return c == conns_.end() ? nullptr : c->second.get();
}

void Worker::send_chat_to(uint64_t conn, const std::string& text) {
    if (JConn* c = conn_by_id(conn)) { if (c->st == St::Play && !c->dead) send_chat(*c, text); }
    else if (bedrock_) bedrock_->chat_to(conn, text);
}

void Worker::kick_conn(uint64_t conn, const std::string& reason) {
    if (JConn* c = conn_by_id(conn)) kick(*c, reason);
    else if (bedrock_) bedrock_->kick(conn, reason);
}

void Worker::on_game_event(uint64_t conn, const GEvent& ev) {
    if (JConn* c = conn_by_id(conn)) { if (c->st == St::Play && !c->dead) java_event(*c, ev); }
    else if (bedrock_) bedrock_->on_game_event(conn, ev);
}

void Worker::post_auth(uint64_t conn_id, AuthResult&& r) {
    {
        std::lock_guard<std::mutex> l(inbox_mu_);
        auth_inbox_.emplace_back(conn_id, std::move(r));
    }
    uint64_t one = 1;
    (void)!write(evfd_, &one, 8);
}

void Worker::drain_inbox() {
    std::vector<std::string> msgs;
    std::vector<std::pair<uint64_t, AuthResult>> auths;
    std::vector<std::function<void(Worker&)>> fns;
    {
        std::lock_guard<std::mutex> l(inbox_mu_);
        msgs.swap(inbox_);
        auths.swap(auth_inbox_);
        fns.swap(fn_inbox_);
    }
    for (auto& f : fns) f(*this);
    for (auto& kv : conns_) if (!kv.second->out.empty() && !kv.second->dead) flush(*kv.second);
    for (auto& a : auths) on_auth_result(a.first, a.second);
    if (bedrock_) for (auto& m : msgs) bedrock_->broadcast_chat(m);
    for (auto& m : msgs)
        for (auto& kv : conns_)
            if (kv.second->st == St::Play && !kv.second->dead) send_chat(*kv.second, m);
}

void Worker::account(JConn& c) {
    size_t now = kBaseConnCost + c.in.capacity() + c.out.capacity() + c.sent.size() * 24 + c.pending.capacity() * 8;
    if (now > c.acct) s_.mem_used += static_cast<int64_t>(now - c.acct);
    else s_.mem_used -= static_cast<int64_t>(c.acct - now);
    c.acct = now;
}

void Worker::accept_ready() {
    for (int i = 0; i < 64; i++) {
        sockaddr_storage ss{};
        socklen_t sl = sizeof ss;
        int fd = accept4(tcp_, reinterpret_cast<sockaddr*>(&ss), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        if (!s_.mem_add(kBaseConnCost)) { close(fd); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        auto c = std::make_unique<JConn>();
        c->id = s_.next_conn_id++;
        c->fd = fd;
        c->accepted_ms = now_ms();
        c->acct = kBaseConnCost;
        char ip[64] = "?";
        int port = 0;
        if (ss.ss_family == AF_INET6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
            inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
            port = ntohs(a->sin6_port);
            if (strncmp(ip, "::ffff:", 7) == 0) memmove(ip, ip + 7, strlen(ip) - 6);
        } else if (ss.ss_family == AF_INET) {
            auto* a = reinterpret_cast<sockaddr_in*>(&ss);
            inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
            port = ntohs(a->sin_port);
        }
        c->addr = std::string(ip) + ":" + std::to_string(port);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = fd;
        epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
        id_fd_[c->id] = fd;
        conns_[fd] = std::move(c);
        nconn++;
    }
}

void Worker::tcp_readable(JConn& c) {
    size_t total = 0;
    while (total < 256 * 1024) {
        ssize_t n = recv(c.fd, scratch_.data(), scratch_.size(), 0);
        if (n > 0) {
            c.in.insert(c.in.end(), scratch_.data(), scratch_.data() + n);
            total += static_cast<size_t>(n);
            if (c.in.size() - c.inpos > 4 * 1024 * 1024) { c.dead = true; return; }
        } else if (n == 0) {
            c.dead = true;
            c.out.clear();
            c.outpos = 0;
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) { c.dead = true; c.out.clear(); c.outpos = 0; }
            break;
        }
    }
    if (!c.in.empty() && !(c.dead && c.out.empty())) parse_frames(c);
}

void Worker::tcp_writable(JConn& c) { flush(c); }

void Worker::udp_readable() {
    uint8_t buf[2048];
    for (int i = 0; i < 256; i++) {
        sockaddr_storage from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(udp_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) return;
        if (bedrock_) bedrock_->on_datagram(from, fl, buf, static_cast<size_t>(n), now_ms());
    }
}

void Worker::flush(JConn& c) {
    while (c.outpos < c.out.size()) {
        ssize_t n = ::send(c.fd, c.out.data() + c.outpos, c.out.size() - c.outpos, MSG_NOSIGNAL);
        if (n > 0) c.outpos += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { c.out.clear(); c.outpos = 0; c.dead = true; return; }
    }
    if (c.outpos == c.out.size()) {
        c.out.clear();
        c.outpos = 0;
        if (c.out.capacity() > 256 * 1024) std::vector<uint8_t>().swap(c.out);
    } else if (c.outpos > 128 * 1024) {
        c.out.erase(c.out.begin(), c.out.begin() + static_cast<long>(c.outpos));
        c.outpos = 0;
    }
    bool need = c.outpos < c.out.size();
    if (need != c.want_out) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP | (need ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.fd = c.fd;
        epoll_ctl(ep_, EPOLL_CTL_MOD, c.fd, &ev);
        c.want_out = need;
    }
}

void Worker::close_conn(JConn& c) {
    if (c.reserved) s_.release_name(c.name);
    if (c.gid) { s_.game.remove_player(c.gid); c.gid = 0; }
    if (c.counted) {
        s_.unregister_player(c.name);
        s_.online--;
        info(c.name + " lost connection: Disconnected");
        info(c.name + " left the game");
        if (!s_.stopping) s_.broadcast(c.name + " left the game");
    }
    epoll_ctl(ep_, EPOLL_CTL_DEL, c.fd, nullptr);
    close(c.fd);
    s_.mem_sub(static_cast<int64_t>(c.acct));
    int fd = c.fd;
    id_fd_.erase(c.id);
    conns_.erase(fd);
    nconn--;
    if (conns_.empty()) idle_since_ms = now_ms();
}

void Worker::tick(int64_t now) {
    for (auto& kv : conns_) {
        JConn& c = *kv.second;
        if (c.dead) continue;
        if (c.st != St::Play && c.st != St::Status && now - c.accepted_ms > 30000) { kick(c, "Took too long to log in"); continue; }
        if (c.st == St::Status && now - c.accepted_ms > 15000) { c.dead = true; continue; }
        if (c.st == St::Play) {
            if (now - c.last_ka_sent >= 10000) {
                if (now - c.last_ka_ack > 30000) { kick(c, "Timed out"); continue; }
                c.ka_id = now;
                Wr w;
                w.varint(c.vd->id.pl_keep_alive);
                w.be64(static_cast<uint64_t>(c.ka_id));
                send(c, w.b);
                c.last_ka_sent = now;
            }
            pump_chunks(c);
        }
    }
}

void Worker::run() {
    {
        std::string nm = "pn-worker" + std::to_string(id_);
        pthread_setname_np(pthread_self(), nm.c_str());
    }
    if (s_.cfg.pin && core_ >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core_, &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    }
    epoll_event evs[128];
    last_tick_ = now_ms();
    uint64_t wall0 = now_ns();
    std::vector<int> touched;
    while (!stop_) {
        if (draining_ && conns_.empty()) break;
        int n = epoll_wait(ep_, evs, 128, (bedrock_ && bedrock_->sessions()) ? 20 : 50);
        uint64_t t0 = now_ns();
        touched.clear();
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == tcp_) { accept_ready(); continue; }
            if (fd == udp_) {
                try { udp_readable(); } catch (const std::exception& e) { error(std::string("Bedrock datagram handling failed: ") + e.what()); }
                continue;
            }
            if (fd == evfd_) {
                uint64_t v;
                (void)!read(evfd_, &v, 8);
                try { drain_inbox(); } catch (const std::exception& e) { error(std::string("worker event failed: ") + e.what()); }
                continue;
            }
            auto it = conns_.find(fd);
            if (it == conns_.end()) continue;
            JConn& c = *it->second;
            try {
                if (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) tcp_readable(c);
                if (evs[i].events & EPOLLOUT) tcp_writable(c);
            } catch (const std::exception& e) {   // a bug triggered by one client must not take the server down
                error("connection " + c.addr + " dropped after an internal error: " + e.what());
                c.dead = true;
                c.out.clear();
                c.outpos = 0;
            }
            touched.push_back(fd);
        }
        int64_t now = now_ms();
        if (bedrock_) bedrock_->tick(now);
        if (now - last_tick_ >= 50) { last_tick_ = now; tick(now); for (auto& kv : conns_) touched.push_back(kv.first); }
        for (int fd : touched) {
            auto it = conns_.find(fd);
            if (it == conns_.end()) continue;
            JConn& c = *it->second;
            if (!c.out.empty()) flush(c);
            account(c);
            if (c.dead && c.out.empty()) close_conn(c);
        }
        busy_ns += now_ns() - t0;
        wall_ns = now_ns() - wall0;
    }
    if (bedrock_) bedrock_->shutdown();
    std::vector<int> fds;
    for (auto& kv : conns_) fds.push_back(kv.first);
    for (int fd : fds) { auto it = conns_.find(fd); if (it != conns_.end()) { it->second->out.clear(); close_conn(*it->second); } }
    finished = true;
}

}  // namespace pn
