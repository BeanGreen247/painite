// Bedrock side, stage 1: RakNet unconnected ping/pong and the open-connection handshake.
// The reliable-frame session layer and the Bedrock login are not implemented yet.
#include "server.hpp"
#include "log.hpp"
#include "util.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace pn {

static const uint8_t kMagic[16] = {0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};
static constexpr int kRakNetProtocol = 11;

static bool magic_at(const uint8_t* p, size_t n, size_t off) { return n >= off + 16 && memcmp(p + off, kMagic, 16) == 0; }

void Worker::udp_readable() {
    uint8_t buf[2048];
    for (int i = 0; i < 128; i++) {
        sockaddr_storage from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(udp_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) return;
        Rd r(buf, static_cast<size_t>(n));
        uint8_t id = r.u8();
        Wr w;
        if (id == 0x01 || id == 0x02) {
            if (!magic_at(buf, static_cast<size_t>(n), 9)) continue;
            uint64_t t = r.be64();
            std::string motd = s_.cfg.motd;
            for (auto& ch : motd) if (ch == ';') ch = ',';
            std::string level = s_.cfg.level_name;
            for (auto& ch : level) if (ch == ';') ch = ',';
            static const char* gm[] = {"Survival", "Creative", "Adventure", "Spectator"};
            std::string info = "MCPE;" + motd + ";" + std::to_string(s_.cfg.bedrock_protocol) + ";" + s_.cfg.bedrock_version + ";" +
                               std::to_string(s_.online.load()) + ";" + std::to_string(s_.cfg.max_players) + ";" + std::to_string(s_.guid) + ";" +
                               level + ";" + gm[s_.cfg.gamemode & 3] + ";1;" + std::to_string(s_.cfg.bedrock_port_effective()) + ";" +
                               std::to_string(s_.cfg.bedrock_port_effective()) + ";";
            w.u8(0x1c);
            w.be64(t);
            w.be64(s_.guid);
            w.raw(kMagic, 16);
            w.be16(static_cast<uint16_t>(info.size()));
            w.raw(info.data(), info.size());
        } else if (id == 0x05) {
            if (!magic_at(buf, static_cast<size_t>(n), 1)) continue;
            uint8_t ver = buf[17];
            if (ver != kRakNetProtocol) {
                w.u8(0x19);
                w.u8(kRakNetProtocol);
                w.raw(kMagic, 16);
                w.be64(s_.guid);
            } else {
                int mtu = std::min<int>(static_cast<int>(n) + 28, 1492);
                w.u8(0x06);
                w.raw(kMagic, 16);
                w.be64(s_.guid);
                w.u8(0);
                w.be16(static_cast<uint16_t>(mtu));
            }
        } else if (id == 0x07) {
            if (!magic_at(buf, static_cast<size_t>(n), 1)) continue;
            r.skip(16);
            uint8_t ver = r.u8();
            r.skip(ver == 4 ? 6 : 26);
            uint16_t mtu = r.be16();
            if (r.bad) continue;
            w.u8(0x08);
            w.raw(kMagic, 16);
            w.be64(s_.guid);
            uint16_t port = 0;
            uint8_t ip4[4] = {0, 0, 0, 0};
            if (from.ss_family == AF_INET6) {
                auto* a = reinterpret_cast<sockaddr_in6*>(&from);
                port = ntohs(a->sin6_port);
                memcpy(ip4, a->sin6_addr.s6_addr + 12, 4);
            } else {
                auto* a = reinterpret_cast<sockaddr_in*>(&from);
                port = ntohs(a->sin_port);
                memcpy(ip4, &a->sin_addr, 4);
            }
            w.u8(4);
            for (int k = 0; k < 4; k++) w.u8(static_cast<uint8_t>(~ip4[k]));
            w.be16(port);
            w.be16(std::min<uint16_t>(mtu, 1492));
            w.u8(0);
            static int64_t last_log = 0;
            int64_t now = now_ms();
            if (now - last_log > 5000) {
                last_log = now;
                warn("Bedrock client reached the RakNet handshake; the Bedrock session layer is not implemented yet");
            }
        } else {
            continue;  // connected-session datagrams: not implemented yet
        }
        sendto(udp_, w.b.data(), w.b.size(), 0, reinterpret_cast<sockaddr*>(&from), fl);
    }
}

}  // namespace pn
