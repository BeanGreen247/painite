#include "carver3.hpp"
#include "mathutil.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

namespace pn::vg::v3 {

namespace {
constexpr float kPi = kPiF;
inline float rot_noise(Rng& r) { float a = r.next_float(); float b = r.next_float(); float c = r.next_float(); return (a - b) * c; }
inline int jfloor(double v) { return int(__builtin_floor(v)); }
inline float msin(double v) { return pn::vg::mth_sin(v); }
inline float mcos(double v) { return pn::vg::mth_cos(v); }

float opt_float(const Json& j, const char* key, float def, Rng& r) {
    const Json* p = j.find(key);
    return p ? sample_float(*p, r) : def;
}
}  // namespace

struct Carvers3::Impl {
    const Registry& reg;
    const BlockData& bd;
    int64_t seed;
    GenRange range;
    std::vector<std::vector<std::string>> biome_carvers;
    struct Carver { bool canyon; const Json* cfg; float probability; };
    std::map<std::string, Carver> carvers;
    std::vector<uint8_t> uncarvable;

    // CarvingMask
    struct Mask {
        int min_y, max_y, height;
        std::vector<uint8_t> bits;
        Mask(int lo, int hi) : min_y(lo), max_y(hi), height(hi - lo + 1), bits(size_t(256) * size_t(hi - lo + 1), 0) {}
        void carve(int x, int y, int z) { bits[size_t(y - min_y) + size_t(z + (x << 4)) * size_t(height)] = 1; }
    };
    struct Job { int cx, cz; Mask& mask; };
    using Skip = std::function<bool(double, double, double, int)>;

    Impl(const Registry& r, const BlockData& b) : reg(r), bd(b) {}

    static bool can_reach(int cx, int cz, double x, double z, int step, int total, float thickness) {
        double xd = x - double(cx * 16 + 8), zd = z - double(cz * 16 + 8);
        double remaining = double(total - step);
        double rr = double(thickness + 2.0f + 16.0f);
        return xd * xd + zd * zd - remaining * remaining <= rr * rr;
    }

    void carve_ellipsoid(Job& job, double x, double y, double z, double hr, double vr, const Skip& skip) {
        double center_x = job.cx * 16 + 8, center_z = job.cz * 16 + 8;
        double max_delta = 16.0 + hr * 2.0;
        if (std::fabs(x - center_x) > max_delta || std::fabs(z - center_z) > max_delta) return;
        int cmx = job.cx * 16, cmz = job.cz * 16;
        int min_xi = std::max(jfloor(x - hr) - cmx - 1, 0), max_xi = std::min(jfloor(x + hr) - cmx, 15);
        int min_y = std::max(jfloor(y - vr) - 1, job.mask.min_y), max_y = std::min(jfloor(y + vr) + 1, job.mask.max_y);
        int min_zi = std::max(jfloor(z - hr) - cmz - 1, 0), max_zi = std::min(jfloor(z + hr) - cmz, 15);
        for (int xi = min_xi; xi <= max_xi; xi++) {
            double xd = (double(cmx + xi) + 0.5 - x) / hr;
            for (int zi = min_zi; zi <= max_zi; zi++) {
                double zd = (double(cmz + zi) + 0.5 - z) / hr;
                if (xd * xd + zd * zd >= 1.0) continue;
                for (int wy = max_y; wy > min_y; wy--) {
                    double yd = (double(wy) - 0.5 - y) / vr;
                    if (skip(xd, yd, zd, wy)) continue;
                    job.mask.carve(xi, wy, zi);
                }
            }
        }
    }

    void create_room(Job& job, double x, double y, double z, float thickness, double y_scale, const Skip& skip) {
        double hr = 1.5 + double(msin(double(kPi / 2.0f)) * thickness);
        carve_ellipsoid(job, x + 1.0, y, z, hr, hr * y_scale, skip);
    }

    void create_tunnel(Job& job, int64_t tunnel_seed, double x, double y, double z, double hr_mult, double vr_mult, float thickness, float h_rot, float v_rot, int step, int dist, double y_scale, const Skip& skip) {
        LegacyRandom random(tunnel_seed);
        int split = random.next_int(dist / 2) + dist / 4;
        bool steep = random.next_int(6) == 0;
        float y_rota = 0.0f, x_rota = 0.0f;
        for (int cur = step; cur < dist; cur++) {
            double hr = 1.5 + double(msin(double(kPi * float(cur) / float(dist))) * thickness);
            double vr = hr * y_scale;
            float cos_x = mcos(v_rot);
            x += double(mcos(h_rot) * cos_x);
            y += double(msin(v_rot));
            z += double(msin(h_rot) * cos_x);
            v_rot *= steep ? 0.92f : 0.7f;
            v_rot += x_rota * 0.1f;
            h_rot += y_rota * 0.1f;
            x_rota *= 0.9f;
            y_rota *= 0.75f;
            x_rota += rot_noise(random) * 2.0f;
            y_rota += rot_noise(random) * 4.0f;
            if (cur == split && thickness > 1.0f) {
                for (int side = -1; side <= 1; side += 2) {
                    int64_t sub_seed = random.next_long();
                    float sub_thickness = random.next_float() * 0.5f + 0.5f;
                    create_tunnel(job, sub_seed, x, y, z, hr_mult, vr_mult, sub_thickness, side < 0 ? h_rot - 1.5707964f : h_rot + 1.5707964f, v_rot / 3.0f, cur, dist, 1.0, skip);
                }
                return;
            }
            if (random.next_int(4) == 0) continue;
            if (!can_reach(job.cx, job.cz, x, z, cur, dist, thickness)) return;
            carve_ellipsoid(job, x, y, z, hr * hr_mult, vr * vr_mult, skip);
        }
    }

    void carve_cave(Job& job, const Carver& car, Rng& random, int src_x, int src_z) {
        const Json& cfg = *car.cfg;
        int max_distance = (4 * 2 - 1) * 16;
        int count = sample_int(cfg.at("count"), random);
        for (int cave = 0; cave < count; cave++) {
            double x = double(src_x * 16 + random.next_int(16));
            double y = double(sample_height(cfg.at("y"), random, range));
            double z = double(src_z * 16 + random.next_int(16));
            double h_mult = double(sample_float(cfg.at("horizontal_radius_multiplier"), random));
            double v_mult = double(sample_float(cfg.at("vertical_radius_multiplier"), random));
            double start_v = double(opt_float(cfg, "start_vertical_radius_multiplier", 1.0f, random));
            double floor_level = double(sample_float(cfg.at("floor_level"), random));
            Skip skip = [floor_level](double xd, double yd, double zd, int) { return yd <= floor_level ? true : xd * xd + yd * yd + zd * zd >= 1.0; };
            int tunnels = 1;
            if (random.next_int(4) == 0) {
                double y_scale = double(sample_float(cfg.at("room_vertical_radius_multiplier"), random));
                float thickness = 1.0f + random.next_float() * 6.0f;
                create_room(job, x, y, z, thickness, y_scale, skip);
                tunnels += random.next_int(4);
            }
            bool weird = cfg.get_bool("weird_thickness_bias", true);
            for (int i = 0; i < tunnels; i++) {
                float h_rot = random.next_float() * (kPi * 2.0f);
                float v_rot = (random.next_float() - 0.5f) / 4.0f;
                float thickness = sample_float(cfg.at("thickness"), random);
                if (weird && random.next_int(10) == 0) thickness *= random.next_float() * random.next_float() * 3.0f + 1.0f;
                int distance = max_distance - random.next_int(max_distance / 4);
                create_tunnel(job, random.next_long(), x, y, z, h_mult, v_mult, thickness, h_rot, v_rot, 0, distance, start_v, skip);
            }
        }
    }

    void carve_canyon(Job& job, const Carver& car, Rng& random, int src_x, int src_z) {
        const Json& cfg = *car.cfg;
        const Json& shape = cfg.at("shape");
        int max_distance = (4 * 2 - 1) * 16;
        double x = double(src_x * 16 + random.next_int(16));
        int y = sample_height(cfg.at("y"), random, range);
        double z = double(src_z * 16 + random.next_int(16));
        float h_rot = random.next_float() * (kPi * 2.0f);
        float v_rot = sample_float(cfg.at("vertical_rotation"), random);
        double y_scale = double(sample_float(shape.at("y_scale"), random));
        float thickness = sample_float(shape.at("thickness"), random);
        int distance = int(double(float(max_distance) * sample_float(shape.at("distance_factor"), random)));
        canyon_do_carve(job, random.next_long(), x, double(y), z, thickness, h_rot, v_rot, distance, y_scale, shape);
    }

    void canyon_do_carve(Job& job, int64_t tunnel_seed, double x, double y, double z, float thickness, float h_rot, float v_rot, int distance, double y_scale, const Json& shape) {
        LegacyRandom r(tunnel_seed);
        std::vector<float> widths(size_t(range.height), 0.0f);
        float wf = 1.0f;
        int smooth = int(shape.at("width_smoothness").int_value());
        for (int i = 0; i < range.height; i++) {
            if (i == 0 || r.next_int(smooth) == 0) { float a = r.next_float(); float b = r.next_float(); wf = 1.0f + a * b; }
            widths[size_t(i)] = wf * wf;
        }
        const float vdef = shape.at("vertical_radius_default_factor").float_value(), vcen = shape.at("vertical_radius_center_factor").float_value();
        const int min_gen_y = range.min_y;
        Skip skip = [&widths, min_gen_y](double xd, double yd, double zd, int wy) { return (xd * xd + zd * zd) * double(widths[size_t(wy - min_gen_y - 1)]) + yd * yd / 6.0 >= 1.0; };
        float y_rota = 0.0f, x_rota = 0.0f;
        for (int cur = 0; cur < distance; cur++) {
            double hr = 1.5 + double(msin(double(float(cur) * kPi / float(distance))) * thickness);
            double vr = hr * y_scale;
            hr *= double(sample_float(shape.at("horizontal_radius_factor"), r));
            float vmul = 1.0f - std::fabs(0.5f - float(cur) / float(distance)) * 2.0f;
            float factor = vdef + vcen * vmul;
            float between = r.next_float() * (1.0f - 0.75f) + 0.75f;
            vr = double(factor) * vr * double(between);
            float xc = mcos(v_rot), xs = msin(v_rot);
            x += double(mcos(h_rot) * xc);
            y += double(xs);
            z += double(msin(h_rot) * xc);
            v_rot *= 0.7f;
            v_rot += x_rota * 0.05f;
            h_rot += y_rota * 0.05f;
            x_rota *= 0.8f;
            y_rota *= 0.5f;
            x_rota += rot_noise(r) * 2.0f;
            y_rota += rot_noise(r) * 4.0f;
            if (r.next_int(4) != 0) {
                if (!can_reach(job.cx, job.cz, x, z, cur, distance, thickness)) return;
                carve_ellipsoid(job, x, y, z, hr, vr, skip);
            }
        }
    }
};

Carvers3::~Carvers3() = default;

Carvers3::Carvers3(const Registry& reg, const BlockData& bd, int64_t seed, GenRange range, std::vector<std::vector<std::string>> carvers_of_biome) : p_(std::make_unique<Impl>(reg, bd)) {
    Impl& I = *p_;
    I.seed = seed;
    I.range = range;
    I.biome_carvers = std::move(carvers_of_biome);
    for (auto& list : I.biome_carvers)
        for (auto& id : list) {
            if (I.carvers.count(id)) continue;
            const Json& j = reg.get("carver", id);
            Impl::Carver c;
            c.canyon = Registry::strip(j.at("type").str) == "canyon";
            c.cfg = &j;
            c.probability = j.at("probability").float_value();
            I.carvers.emplace(id, c);
        }
}

void Carvers3::carve(const Terrain3& t, NoiseChunk3& nc, ChunkBlocks& blocks, int chunk_x, int chunk_z, const std::function<int(int, int)>& source_biome, const std::function<int(int, int, int)>& lookup) const {
    Impl& I = *p_;
    const int protected_on_top = 7;
    Impl::Mask mask(I.range.min_y + 1, I.range.min_y + I.range.height - 1 - protected_on_top);
    Impl::Job job{chunk_x, chunk_z, mask};
    LegacyRandom random(0);
    for (int dx = -8; dx <= 8; dx++)
        for (int dz = -8; dz <= 8; dz++) {
            int sx = chunk_x + dx, sz = chunk_z + dz;
            const auto& list = I.biome_carvers[size_t(source_biome(sx * 4, sz * 4))];
            int index = 0;
            for (const std::string& id : list) {
                const Impl::Carver& car = I.carvers.at(id);
                int64_t base = I.seed + index;
                random.set_seed(base);
                int64_t xs = random.next_long(), zs = random.next_long();
                random.set_seed(int64_t(uint64_t(int64_t(sx)) * uint64_t(xs)) ^ int64_t(uint64_t(int64_t(sz)) * uint64_t(zs)) ^ base);
                if (random.next_float() <= car.probability) {
                    if (car.canyon) I.carve_canyon(job, car, random, sx, sz);
                    else I.carve_cave(job, car, random, sx, sz);
                }
                index++;
            }
        }
    // applyCarvingMask: column by column, each run of carved blocks from the top down
    if (I.uncarvable.empty()) {}
    const uint8_t* flags = I.bd.chunk_flags();
    const BlockData& bd = I.bd;
    const int h = mask.height;
    for (int xi = 0; xi < 16; xi++)
        for (int zi = 0; zi < 16; zi++) {
            const uint8_t* col = &mask.bits[size_t(zi + (xi << 4)) * size_t(h)];
            int wx = chunk_x * 16 + xi, wz = chunk_z * 16 + zi;
            int y = 0;
            while (y < h) {
                if (!col[y]) { y++; continue; }
                int start = y;
                while (y < h && col[y]) y++;
                int bottom = start + mask.min_y, top = y - 1 + mask.min_y;
                bool has_grass = false;
                for (int wy = top; wy >= bottom; wy--) {
                    uint16_t old = blocks.get(wx, wy, wz);
                    if (bd.in_tag(old, "minecraft:uncarvable")) continue;
                    const std::string& nm = bd.name(old);
                    if (nm == "minecraft:grass_block" || nm == "minecraft:mycelium") has_grass = true;
                    int sub = t.aquifer_substance(nc, wx, wy, wz, 0.0);
                    if (sub < 0) continue;
                    bool fluid = (flags[sub] & kFlagFluid) != 0;
                    blocks.set(wx, wy, wz, uint16_t(sub), t.aquifer_should_schedule(nc) && fluid);
                    if (!has_grass) continue;
                    if (bd.name(blocks.get(wx, wy - 1, wz)) != "minecraft:dirt") continue;
                    uint16_t top_state;
                    if (t.top_material(nc, blocks, lookup, wx, wy - 1, wz, fluid, top_state)) blocks.set(wx, wy - 1, wz, top_state, (flags[top_state] & kFlagFluid) != 0);
                }
            }
        }
}

}  // namespace pn::vg::v3
