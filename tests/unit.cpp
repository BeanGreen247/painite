// Unit tests for pure helpers. Build: cmake --build build --target painite_unit && build/painite_unit
#include "config.hpp"
#include "hash.hpp"
#include "util.hpp"
#include <cstdio>
#include <openssl/evp.h>
#include <cstring>

using namespace pn;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static std::string digest_of(const char* text) {
    uint8_t d[20];
    unsigned n = 0;
    EVP_Digest(text, strlen(text), d, &n, EVP_sha1(), nullptr);
    return mc_hex_digest(d);
}

int main() {
    // Vectors from the Minecraft authentication documentation (sha1 of the player name).
    CHECK(digest_of("Notch") == "4ed1f46bbe04bc756bcb17c0c7ce3e4632f06a48");
    CHECK(digest_of("jeb_") == "-7c9d5b0044c130109a5d7b5fb5c317c02b4e28c1");
    CHECK(digest_of("simon") == "88e16a1019277b15d58faf0541e11910eb756f6");

    uint8_t u[16];
    offline_uuid("Notch", u);
    char b[40];
    snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    CHECK(std::string(b) == "b50ad385-829d-3141-a216-7e7d7539ba7f");

    uint8_t m[16];
    md5("", m);
    CHECK(m[0] == 0xd4 && m[1] == 0x1d && m[15] == 0x7e);

    Wr w;
    w.varint(-1);
    w.varint(300);
    Rd r(w.b.data(), w.b.size());
    CHECK(r.varint() == -1);
    CHECK(r.varint() == 300);
    CHECK(!r.bad);

    std::vector<int> c;
    CHECK(parse_cores("0-3", c) && c.size() == 4 && c[3] == 3);
    CHECK(parse_cores("[0, 2,4]", c) && c.size() == 3 && c[1] == 2);
    CHECK(parse_cores("all", c) && c.empty());
    CHECK(!parse_cores("x", c));

    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
