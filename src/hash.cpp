#include "hash.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

namespace pn {

void md5(const std::string& in, uint8_t out[16]) {
    static uint32_t K[64];
    static bool init = false;
    if (!init) { for (int i = 0; i < 64; i++) K[i] = static_cast<uint32_t>(std::floor(std::fabs(std::sin(i + 1.0)) * 4294967296.0)); init = true; }
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::vector<uint8_t> m(in.begin(), in.end());
    uint64_t bits = static_cast<uint64_t>(in.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 0; i < 8; i++) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) M[i] = m[off + i * 4] | (m[off + i * 4 + 1] << 8) | (m[off + i * 4 + 2] << 16) | (static_cast<uint32_t>(m[off + i * 4 + 3]) << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + ((F << R[i]) | (F >> (32 - R[i])));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    uint32_t r[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<uint8_t>(r[i] >> (8 * j));
}

void offline_uuid(const std::string& name, uint8_t out[16]) {
    md5("OfflinePlayer:" + name, out);
    out[6] = (out[6] & 0x0f) | 0x30;
    out[8] = (out[8] & 0x3f) | 0x80;
}

std::string mc_hex_digest(const uint8_t sha1[20]) {
    uint8_t d[20];
    for (int i = 0; i < 20; i++) d[i] = sha1[i];
    bool neg = d[0] & 0x80;
    if (neg) {
        int carry = 1;
        for (int i = 19; i >= 0; i--) {
            int v = (~d[i] & 0xff) + carry;
            d[i] = static_cast<uint8_t>(v);
            carry = v >> 8;
        }
    }
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    size_t lead = 0;
    while (lead < 39 && hex[lead] == '0') lead++;
    return (neg ? "-" : "") + std::string(hex + lead);
}

}  // namespace pn
