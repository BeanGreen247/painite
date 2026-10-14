// Mth.sin and Mth.cos: Mojang's 65536-entry lookup tables (what world generation uses instead of the library functions).
#pragma once
#include <cmath>
#include <cstdint>

namespace pn::vg {

constexpr float kPiF = 3.14159274f;   // (float) Math.PI

struct SinTable {
    float t[65536];
    SinTable() { for (int i = 0; i < 65536; i++) t[i] = float(std::sin(double(i) / 10430.378350470453)); }
};
inline const SinTable& sin_table() { static const SinTable s; return s; }
inline float mth_sin(double v) { return sin_table().t[int(int64_t(v * 10430.378350470453) & 65535)]; }
inline float mth_cos(double v) { return sin_table().t[int(int64_t(v * 10430.378350470453 + 16384.0) & 65535)]; }

}  // namespace pn::vg
