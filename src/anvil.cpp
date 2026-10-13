#include "anvil.hpp"
#include <cstdio>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace pn {

bool inflate_any(const uint8_t* p, size_t n, std::vector<uint8_t>& out, size_t cap) {
    z_stream z{};
    if (inflateInit2(&z, 15 + 32) != Z_OK) return false;  // zlib or gzip header
    z.next_in = const_cast<uint8_t*>(p);
    z.avail_in = static_cast<uInt>(n);
    out.clear();
    uint8_t buf[65536];
    int r;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) { inflateEnd(&z); return false; }
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
        if (out.size() > cap) { inflateEnd(&z); return false; }
    } while (r != Z_STREAM_END);
    inflateEnd(&z);
    return true;
}

bool gzip_compress(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    z_stream z{};
    if (deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    out.resize(deflateBound(&z, static_cast<uLong>(n)) + 64);
    z.next_in = const_cast<uint8_t*>(p);
    z.avail_in = static_cast<uInt>(n);
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    int r = deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return r == Z_STREAM_END;
}

static bool zlib_compress(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    uLongf cap = compressBound(static_cast<uLong>(n));
    out.resize(cap);
    if (compress2(out.data(), &cap, p, static_cast<uLong>(n), 4) != Z_OK) return false;
    out.resize(cap);
    return true;
}

// LZ4 block decompression (enough for Minecraft's LZ4BlockOutputStream framing).
static bool lz4_block(const uint8_t* in, size_t n, uint8_t* out, size_t olen) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t tok = in[i++];
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= n) return false; b = in[i++]; lit += b; } while (b == 255); }
        if (i + lit > n || o + lit > olen) return false;
        memcpy(out + o, in + i, lit);
        i += lit;
        o += lit;
        if (i >= n) break;
        if (i + 2 > n) return false;
        size_t off = in[i] | (static_cast<size_t>(in[i + 1]) << 8);
        i += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (i >= n) return false; b = in[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (off == 0 || off > o || o + ml > olen) return false;
        for (size_t k = 0; k < ml; k++) { out[o] = out[o - off]; o++; }
    }
    return o == olen;
}

static bool lz4_stream(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    size_t i = 0;
    while (i + 21 <= n) {
        if (memcmp(p + i, "LZ4Block", 8) != 0) return false;
        uint8_t token = p[i + 8];
        uint32_t clen, dlen;
        memcpy(&clen, p + i + 9, 4);
        memcpy(&dlen, p + i + 13, 4);
        i += 21;
        if (i + clen > n || dlen > (64u << 20)) return false;
        size_t base = out.size();
        out.resize(base + dlen);
        if ((token & 0xf0) == 0x10) { if (clen != dlen) return false; memcpy(out.data() + base, p + i, dlen); }
        else if (!lz4_block(p + i, clen, out.data() + base, dlen)) return false;
        i += clen;
        if (dlen == 0) break;
    }
    return true;
}

static std::string region_path(const std::string& dir, int cx, int cz) {
    return dir + "/r." + std::to_string(cx >> 5) + "." + std::to_string(cz >> 5) + ".mca";
}

static bool read_all(int fd, uint8_t* p, size_t n, off_t off) {
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r; n -= static_cast<size_t>(r); off += r;
    }
    return true;
}
static bool write_all(int fd, const uint8_t* p, size_t n, off_t off) {
    while (n) {
        ssize_t r = pwrite(fd, p, n, off);
        if (r <= 0) return false;
        p += r; n -= static_cast<size_t>(r); off += r;
    }
    return true;
}

bool anvil_read(const std::string& dir, int cx, int cz, Nbt& root) {
    int fd = open(region_path(dir, cx, cz).c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = false;
    do {
        uint8_t ent[4];
        int idx = (cx & 31) + (cz & 31) * 32;
        if (!read_all(fd, ent, 4, idx * 4)) break;
        uint32_t off = (static_cast<uint32_t>(ent[0]) << 16 | ent[1] << 8 | ent[2]);
        if (off == 0 || ent[3] == 0) break;
        uint8_t hdr[5];
        if (!read_all(fd, hdr, 5, static_cast<off_t>(off) * 4096)) break;
        uint32_t len = static_cast<uint32_t>(hdr[0]) << 24 | hdr[1] << 16 | hdr[2] << 8 | hdr[3];
        if (len < 1 || len > (ent[3] * 4096u)) {
            if (!(hdr[4] & 128)) break;
        }
        uint8_t type = hdr[4];
        std::vector<uint8_t> raw;
        if (type & 128) {  // payload in c.<x>.<z>.mcc
            std::string ext = dir + "/c." + std::to_string(cx) + "." + std::to_string(cz) + ".mcc";
            int ef = open(ext.c_str(), O_RDONLY);
            if (ef < 0) break;
            struct stat st;
            fstat(ef, &st);
            raw.resize(static_cast<size_t>(st.st_size));
            bool r = read_all(ef, raw.data(), raw.size(), 0);
            close(ef);
            if (!r) break;
            type &= 127;
        } else {
            raw.resize(len - 1);
            if (!read_all(fd, raw.data(), raw.size(), static_cast<off_t>(off) * 4096 + 5)) break;
        }
        std::vector<uint8_t> data;
        if (type == 1 || type == 2) { if (!inflate_any(raw.data(), raw.size(), data)) break; }
        else if (type == 3) data = std::move(raw);
        else if (type == 4) { if (!lz4_stream(raw.data(), raw.size(), data)) break; }
        else break;
        ok = nbt_read(data.data(), data.size(), root, true);
    } while (false);
    close(fd);
    return ok;
}

bool anvil_write(const std::string& dir, int cx, int cz, const Nbt& root) {
    mkdir(dir.c_str(), 0755);
    std::vector<uint8_t> nbt, z;
    nbt_write(nbt, root, true, "");
    if (!zlib_compress(nbt.data(), nbt.size(), z)) return false;
    int fd = open(region_path(dir, cx, cz).c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return false;
    bool ok = false;
    do {
        struct stat st;
        fstat(fd, &st);
        std::vector<uint8_t> hdr(8192, 0);
        if (st.st_size >= 8192) { if (!read_all(fd, hdr.data(), 8192, 0)) break; }
        else if (!write_all(fd, hdr.data(), 8192, 0)) break;
        size_t fsec = std::max<size_t>(2, (static_cast<size_t>(st.st_size) + 4095) / 4096);
        std::vector<bool> used(fsec, false);
        used[0] = used[1] = true;
        int idx = (cx & 31) + (cz & 31) * 32;
        for (int i = 0; i < 1024; i++) {
            if (i == idx) continue;
            uint32_t off = static_cast<uint32_t>(hdr[i * 4]) << 16 | hdr[i * 4 + 1] << 8 | hdr[i * 4 + 2];
            for (uint32_t k = 0; k < hdr[i * 4 + 3] && off + k < fsec; k++) used[off + k] = true;
        }
        size_t need = (z.size() + 5 + 4095) / 4096;
        if (need > 255) break;  // would need an external .mcc file; not produced by editing flat/normal chunks
        size_t start = 0, run = 0;
        for (size_t s = 2; s <= fsec; s++) {
            bool free_ = s >= fsec || !used[s];
            if (free_) { if (run == 0) start = s; run++; if (run == need) break; }
            else run = 0;
        }
        if (run < need) { start = fsec; }  // append (a trailing partial run is extended by writing past the end)
        std::vector<uint8_t> blob(need * 4096, 0);
        uint32_t len = static_cast<uint32_t>(z.size() + 1);
        blob[0] = static_cast<uint8_t>(len >> 24); blob[1] = static_cast<uint8_t>(len >> 16); blob[2] = static_cast<uint8_t>(len >> 8); blob[3] = static_cast<uint8_t>(len);
        blob[4] = 2;
        memcpy(blob.data() + 5, z.data(), z.size());
        if (!write_all(fd, blob.data(), blob.size(), static_cast<off_t>(start) * 4096)) break;
        hdr[idx * 4] = static_cast<uint8_t>(start >> 16); hdr[idx * 4 + 1] = static_cast<uint8_t>(start >> 8); hdr[idx * 4 + 2] = static_cast<uint8_t>(start);
        hdr[idx * 4 + 3] = static_cast<uint8_t>(need);
        uint32_t ts = static_cast<uint32_t>(time(nullptr));
        for (int k = 0; k < 4; k++) hdr[4096 + idx * 4 + k] = static_cast<uint8_t>(ts >> (24 - 8 * k));
        ok = write_all(fd, hdr.data(), 8192, 0);
    } while (false);
    close(fd);
    return ok;
}

bool read_nbt_file(const std::string& path, Nbt& root) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    fstat(fd, &st);
    std::vector<uint8_t> raw(static_cast<size_t>(st.st_size)), data;
    bool ok = read_all(fd, raw.data(), raw.size(), 0);
    close(fd);
    return ok && inflate_any(raw.data(), raw.size(), data) && nbt_read(data.data(), data.size(), root, true);
}

bool write_nbt_file(const std::string& path, const Nbt& root) {
    std::vector<uint8_t> nbt, z;
    nbt_write(nbt, root, true, "");
    if (!gzip_compress(nbt.data(), nbt.size(), z)) return false;
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write_all(fd, z.data(), z.size(), 0);
    close(fd);
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

bool region_dir_has_chunks(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool any = false;
    while (auto* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 6 && n.rfind("r.", 0) == 0 && n.substr(n.size() - 4) == ".mca") {
            struct stat st;
            if (stat((dir + "/" + n).c_str(), &st) == 0 && st.st_size > 8192) { any = true; break; }
        }
    }
    closedir(d);
    return any;
}

}  // namespace pn
