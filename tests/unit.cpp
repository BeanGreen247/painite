// Unit tests for pure helpers. Build: cmake --build build --target painite_unit && build/painite_unit
#include "anvil.hpp"
#include "config.hpp"
#include "lists.hpp"
#include "nbt.hpp"
#include "skin.hpp"
#include <unistd.h>
#include <zlib.h>
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

    // NBT round trip, including 26.x style lists of strings and compounds
    {
        Nbt root = Nbt::compound();
        root.put("DataVersion", Nbt::make(Nbt::Int, 5023));
        Nbt pal = Nbt::list_of(Nbt::String);
        pal.list.push_back(Nbt::str("minecraft:stone"));
        root.put("palette", pal);
        root.put("longs", Nbt::longs({1, -2, 3}));
        Nbt d; d.type = Nbt::Double; d.f = 1.5;
        root.put("d", d);
        std::vector<uint8_t> raw;
        nbt_write(raw, root, true, "");
        Nbt back;
        CHECK(nbt_read(raw.data(), raw.size(), back, true));
        CHECK(back.geti("DataVersion") == 5023);
        CHECK(back.get("palette") && back.get("palette")->list.size() == 1 && back.get("palette")->list[0].s == "minecraft:stone");
        CHECK(back.get("longs") && back.get("longs")->la.size() == 3 && back.get("longs")->la[1] == -2);
        CHECK(back.get("d") && back.get("d")->f == 1.5);
        CHECK(!nbt_read(raw.data(), raw.size() / 2, back, true));   // truncated input is refused
    }
    // region files: write two chunks into one file, read them back, replace one
    {
        char dirb[] = "/tmp/painite-unit-XXXXXX";
        std::string dir = mkdtemp(dirb);
        Nbt a = Nbt::compound(), b2 = Nbt::compound();
        a.put("xPos", Nbt::make(Nbt::Int, 3));
        b2.put("xPos", Nbt::make(Nbt::Int, -29));
        CHECK(anvil_write(dir, 3, 4, a));
        CHECK(anvil_write(dir, -29, 4, b2));
        Nbt r1, r2;
        CHECK(anvil_read(dir, 3, 4, r1) && r1.geti("xPos") == 3);
        CHECK(anvil_read(dir, -29, 4, r2) && r2.geti("xPos") == -29);
        a.put("xPos", Nbt::make(Nbt::Int, 99));
        CHECK(anvil_write(dir, 3, 4, a));
        CHECK(anvil_read(dir, 3, 4, r1) && r1.geti("xPos") == 99);
        CHECK(!anvil_read(dir, 5, 5, r1));
        CHECK(region_dir_has_chunks(dir));
        std::string cmd = "rm -rf " + dir;
        (void)!system(cmd.c_str());
    }
    // ban lists keep the vanilla JSON shape
    {
        char dirb[] = "/tmp/painite-unit-XXXXXX";
        std::string dir = mkdtemp(dirb);
        char* old = getcwd(nullptr, 0);
        (void)!chdir(dir.c_str());
        Lists l;
        l.load_all();
        CHECK(l.ban_add("Griefer", "11111111-2222-3333-4444-555555555555", "console", "x\"y"));
        CHECK(!l.ban_add("griefer", "", "console", ""));
        std::string why;
        CHECK(l.banned("GRIEFER", "", &why) && why == "x\"y");
        CHECK(l.banned("other", "11111111-2222-3333-4444-555555555555"));
        CHECK(l.op_add("Boss", "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", 4) && l.op_level("boss", "") == 4 && l.op_level("nobody", "") == 0);
        Lists l2;
        l2.load_all();
        CHECK(l2.banned("Griefer", "") && l2.op_level("Boss", "") == 4);
        CHECK(l2.pardon("griefer") && !l2.banned("Griefer", ""));
        (void)!chdir(old);
        free(old);
        std::string cmd = "rm -rf " + dir;
        (void)!system(cmd.c_str());
    }
    // PNG decoding (8 bit RGBA, filter 0 and 1)
    {
        uint8_t px[2][1 + 8] = {{0, 255, 0, 0, 255, 0, 255, 0, 128}, {1, 10, 20, 30, 255, 5, 5, 5, 0}};
        uLongf zl = compressBound(sizeof px);
        std::vector<uint8_t> z(zl);
        compress(z.data(), &zl, &px[0][0], sizeof px);
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        auto chunk = [&](const char* t, const std::vector<uint8_t>& dta) {
            uint32_t n = static_cast<uint32_t>(dta.size());
            for (int s = 24; s >= 0; s -= 8) png.push_back(static_cast<uint8_t>(n >> s));
            std::vector<uint8_t> body(t, t + 4);
            body.insert(body.end(), dta.begin(), dta.end());
            png.insert(png.end(), body.begin(), body.end());
            uint32_t c = static_cast<uint32_t>(crc32(0, body.data(), static_cast<uInt>(body.size())));
            for (int s = 24; s >= 0; s -= 8) png.push_back(static_cast<uint8_t>(c >> s));
        };
        chunk("IHDR", {0, 0, 0, 2, 0, 0, 0, 2, 8, 6, 0, 0, 0});
        chunk("IDAT", std::vector<uint8_t>(z.begin(), z.begin() + static_cast<long>(zl)));
        chunk("IEND", {});
        int w2 = 0, h2 = 0;
        std::vector<uint8_t> rgba;
        CHECK(decode_png(png.data(), png.size(), w2, h2, rgba) && w2 == 2 && h2 == 2 && rgba.size() == 16);
        CHECK(rgba[0] == 255 && rgba[3] == 255 && rgba[7] == 128 && rgba[11] == 255);
        CHECK(rgba[8] == 10 && rgba[9] == 20 && rgba[12] == 15);   // second row: Sub filter adds the pixel to the left
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
