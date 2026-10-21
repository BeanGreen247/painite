// BiomeManager's fuzzy quart-cell choice: the seed hash and the fiddled distance to a cell corner.
#pragma once
#include <cstdint>
#include <openssl/evp.h>

namespace pn::vg {

inline int64_t lcg_next(int64_t rval, int64_t c) {
    uint64_t r = uint64_t(rval);
    r *= r * 6364136223846793005ULL + 1442695040888963407ULL;
    return int64_t(r + uint64_t(c));
}

// Guava Hashing.sha256().hashLong(seed).asLong(): first eight digest bytes, little endian
inline int64_t obfuscate_seed(int64_t seed) {
    unsigned char in[8], out[32];
    for (int i = 0; i < 8; i++) in[i] = uint8_t(uint64_t(seed) >> (8 * i));
    unsigned int len = 0;
    EVP_Digest(in, 8, out, &len, EVP_sha256(), nullptr);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | out[i];
    return int64_t(v);
}

inline double fiddle(int64_t rval) {
    int64_t m = (rval >> 24) % 1024;
    if (m < 0) m += 1024;
    return (double(m) / 1024.0 - 0.5) * 0.9;
}

inline double fiddled_distance(int64_t seed, int x, int y, int z, double dx, double dy, double dz) {
    int64_t r = lcg_next(seed, x);
    r = lcg_next(r, y);
    r = lcg_next(r, z);
    r = lcg_next(r, x);
    r = lcg_next(r, y);
    r = lcg_next(r, z);
    double fx = fiddle(r);
    r = lcg_next(r, seed);
    double fy = fiddle(r);
    r = lcg_next(r, seed);
    double fz = fiddle(r);
    double a = dz + fz, b = dy + fy, c = dx + fx;
    return a * a + b * b + c * c;
}


}  // namespace pn::vg
