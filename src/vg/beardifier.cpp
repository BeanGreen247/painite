#include "beardifier.hpp"
#include "noise.hpp"
#include <cmath>
#include <cstring>

namespace pn::vg {

namespace {

float g_kernel[24 * 24 * 24];

double fast_inv_sqrt(double x) {
    double xhalf = 0.5 * x;
    int64_t i;
    memcpy(&i, &x, 8);
    i = 6910469410427058090LL - (i >> 1);
    memcpy(&x, &i, 8);
    x *= 1.5 - xhalf * x * x;
    return x;
}

struct KernelInit {
    KernelInit() {
        for (int zi = 0; zi < 24; ++zi)
            for (int xi = 0; xi < 24; ++xi)
                for (int yi = 0; yi < 24; ++yi) {
                    double dx = xi - 12, dy = double(yi - 12) + 0.5, dz = zi - 12;
                    double dist_sqr = dx * dx + dy * dy + dz * dz;
                    g_kernel[zi * 24 * 24 + xi * 24 + yi] = float(std::pow(M_E, -dist_sqr / 16.0));
                }
    }
} kernel_init;

inline bool in_range(int v) { return v >= 0 && v < 24; }

double bury(double dx, double dy, double dz) {
    double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return clamped_map(distance, 0.0, 6.0, 1.0, 0.0);
}

double beard(int dx, int dy, int dz, int y_to_ground) {
    int xi = dx + 12, yi = dy + 12, zi = dz + 12;
    if (!(in_range(xi) && in_range(yi) && in_range(zi))) return 0.0;
    double dy_off = double(y_to_ground) + 0.5;
    double dist_sqr = double(dx) * double(dx) + dy_off * dy_off + double(dz) * double(dz);
    double value = -dy_off * fast_inv_sqrt(dist_sqr / 2.0) / 2.0;
    return value * double(g_kernel[zi * 24 * 24 + xi * 24 + yi]);
}

}  // namespace

Beardifier::Beardifier(const std::vector<std::shared_ptr<const StructureStart>>& starts, int chunk_x, int chunk_z) {
    int bx = chunk_x * 16, bz = chunk_z * 16;
    BoundingBox any = BoundingBox::infinite();
    bool have = false;
    for (auto& start : starts) {
        if (!start->terrain_adaptation) continue;
        for (auto& piece : start->pieces) {
            // StructurePiece.isCloseToChunk(chunkPos, 12)
            if (!piece->bb.intersects(bx - 12, bz - 12, bx + 15 + 12, bz + 15 + 12)) continue;
            if (piece->is_pool) {
                if (piece->rigid) {
                    pieces_.push_back({piece->bb, start->terrain_adaptation, piece->ground_level_delta});
                    any.encapsulate(piece->bb);
                    have = true;
                }
                for (auto& j : piece->junctions) {
                    if (j.source_x <= bx - 12 || j.source_z <= bz - 12 || j.source_x >= bx + 15 + 12 || j.source_z >= bz + 15 + 12) continue;
                    junctions_.push_back(j);
                    any.encapsulate(BlockPos(j.source_x, j.source_ground_y, j.source_z));
                    have = true;
                }
                continue;
            }
            pieces_.push_back({piece->bb, start->terrain_adaptation, 0});
            any.encapsulate(piece->bb);
            have = true;
        }
    }
    if (have) { affected_ = any.inflated_by(24); has_box_ = true; }
}

double Beardifier::compute(int x, int y, int z) const {
    if (!has_box_ || !affected_.inside(x, y, z)) return 0.0;
    double noise = 0.0;
    for (auto& r : pieces_) {
        const BoundingBox& box = r.box;
        int dx = std::max(0, std::max(box.x0 - x, x - box.x1));
        int dz = std::max(0, std::max(box.z0 - z, z - box.z1));
        int ground_y = box.y0 + r.ground_level_delta;
        int dy_to_ground = y - ground_y;
        int dy = 0;
        // TerrainAdjustment: 1 beard_thin, 2 beard_box, 3 bury, 4 encapsulate
        switch (r.adjustment) {
            case 3: case 1: dy = dy_to_ground; break;
            case 2: dy = std::max(0, std::max(ground_y - y, y - box.y1)); break;
            case 4: dy = std::max(0, std::max(box.y0 - y, y - box.y1)); break;
            default: break;
        }
        switch (r.adjustment) {
            case 3: noise += bury(dx, double(dy) / 2.0, dz); break;
            case 1: case 2: noise += beard(dx, dy, dz, dy_to_ground) * 0.8; break;
            case 4: noise += bury(double(dx) / 2.0, double(dy) / 2.0, double(dz) / 2.0) * 0.8; break;
            default: break;
        }
    }
    for (auto& j : junctions_) {
        int dx = x - j.source_x, dy = y - j.source_ground_y, dz = z - j.source_z;
        noise += beard(dx, dy, dz, dy) * 0.4;
    }
    return noise;
}

float Beardifier::compute_f(int x, int y, int z) const {
    if (!has_box_ || !affected_.inside(x, y, z)) return 0.0f;
    auto bury_f = [](float dx, float dy, float dz) {
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= 36.0f) return 0.0f;
        return 1.0f - float(std::sqrt(double(d2))) / 6.0f;
    };
    auto beard_f = [](int dx, int dy, int dz, int y_to_ground) {
        int xi = dx + 12, yi = dy + 12, zi = dz + 12;
        if (!(in_range(xi) && in_range(yi) && in_range(zi))) return 0.0f;
        float dy_off = float(y_to_ground) + 0.5f;
        float d2 = float(dx) * float(dx) + dy_off * dy_off + float(dz) * float(dz);
        float value = -dy_off * float(fast_inv_sqrt(double(d2 / 2.0f))) / 2.0f;
        return value * g_kernel[zi * 24 * 24 + xi * 24 + yi];
    };
    float noise = 0.0f;
    for (auto& r : pieces_) {
        const BoundingBox& box = r.box;
        int dx = std::max(0, std::max(box.x0 - x, x - box.x1));
        int dz = std::max(0, std::max(box.z0 - z, z - box.z1));
        int ground_y = box.y0 + r.ground_level_delta;
        int dy_to_ground = y - ground_y;
        int dy = 0;
        switch (r.adjustment) {
            case 3: case 1: dy = dy_to_ground; break;
            case 2: dy = std::max(0, std::max(ground_y - y, y - box.y1)); break;
            case 4: dy = std::max(0, std::max(box.y0 - y, y - box.y1)); break;
            default: break;
        }
        switch (r.adjustment) {
            case 3: noise += bury_f(float(dx), float(dy) / 2.0f, float(dz)); break;
            case 1: case 2: noise += beard_f(dx, dy, dz, dy_to_ground) * 0.8f; break;
            case 4: noise += bury_f(float(dx) / 2.0f, float(dy) / 2.0f, float(dz) / 2.0f) * 0.8f; break;
            default: break;
        }
    }
    for (auto& j : junctions_) {
        int dx = x - j.source_x, dy = y - j.source_ground_y, dz = z - j.source_z;
        noise += beard_f(dx, dy, dz, dy) * 0.4f;
    }
    return noise;
}

}  // namespace pn::vg
