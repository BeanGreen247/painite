// Small helpers shared by the terrain generators (worldgen.cpp, worldgen_dims.cpp).
#pragma once
#include <cstdint>

namespace pn {

inline uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

inline uint64_t hash2(uint64_t seed, int64_t a, int64_t b) {
    uint64_t x = seed ^ (static_cast<uint64_t>(a) * 0x9e3779b97f4a7c15ull) ^ (static_cast<uint64_t>(b) * 0xc2b2ae3d27d4eb4full);
    return splitmix(x);
}

inline double rnd01(uint64_t& rs) { return static_cast<double>(splitmix(rs) >> 11) / 9007199254740992.0; }
inline int rndi(uint64_t& rs, int n) { return static_cast<int>(rnd01(rs) * n); }

inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
inline double smooth(double e0, double e1, double x) { double t = clamp((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }

}  // namespace pn
