#include "lossyproxy.hpp"
#include "util.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <queue>
#include <random>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace pt {

LossyProxy::LossyProxy(int listen_port, int target_port, double loss, int delay_ms) : target_(target_port), loss_(loss), delay_ms_(delay_ms) {
    auto bind_udp = [](int p) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(p));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) throw std::runtime_error("proxy cannot bind");
        return fd;
    };
    front_ = bind_udp(listen_port);
    back_ = bind_udp(0);
    sockaddr_in a{};
    socklen_t sl = sizeof a;
    getsockname(front_, reinterpret_cast<sockaddr*>(&a), &sl);
    port = ntohs(a.sin_port);
    th_ = std::thread([this] { run(); });
}

LossyProxy::~LossyProxy() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    ::close(front_);
    ::close(back_);
}

void LossyProxy::run() {
    struct Item { int64_t due; uint64_t n; int sock; sockaddr_in dst; Bytes data; };
    auto later = [](const Item& a, const Item& b) { return a.due != b.due ? a.due > b.due : a.n > b.n; };
    std::priority_queue<Item, std::vector<Item>, decltype(later)> q(later);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> u(0, 1);
    sockaddr_in client{};
    bool have_client = false;
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(uint16_t(target_));
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    uint64_t n = 0;
    while (!stop_) {
        int timeout = 100;
        if (!q.empty()) timeout = int(std::max<int64_t>(0, std::min<int64_t>(100, q.top().due - now_ms())));
        pollfd pf[2] = {{front_, POLLIN, 0}, {back_, POLLIN, 0}};
        if (::poll(pf, 2, timeout) > 0) {
            for (int i = 0; i < 2; i++) {
                if (!(pf[i].revents & POLLIN)) continue;
                uint8_t buf[4096];
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                ssize_t len = ::recvfrom(pf[i].fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
                if (len <= 0) continue;
                if (u(rng) * 100 < loss_) { dropped_++; continue; }
                int64_t due = now_ms() + int64_t(delay_ms_ + u(rng) * delay_ms_ * 0.3);
                n++;
                if (i == 0) { client = from; have_client = true; q.push({due, n, back_, target, Bytes(buf, buf + len)}); }
                else if (have_client) q.push({due, n, front_, client, Bytes(buf, buf + len)});
            }
        }
        int64_t now = now_ms();
        while (!q.empty() && q.top().due <= now) {
            Item it = q.top();
            q.pop();
            ::sendto(it.sock, it.data.data(), it.data.size(), 0, reinterpret_cast<sockaddr*>(&it.dst), sizeof it.dst);
            forwarded_++;
        }
    }
}

}  // namespace pt
