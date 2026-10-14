#include "carver.hpp"
#include "mathutil.hpp"
#include <climits>
#include <cmath>
#include <map>
#include <set>

namespace pn::vg {

namespace {

constexpr float kPi = kPiF;

// (a - b) * c with the random calls in Java's order
inline float rot_noise(Rng& r) { float a = r.next_float(); float b = r.next_float(); float c = r.next_float(); return (a - b) * c; }
inline float msin(double v) { return mth_sin(v); }
inline float mcos(double v) { return mth_cos(v); }

}  // namespace

struct Carvers::Impl {
    const Registry& reg;
    const WorldState& state;
    const SurfaceSystem* surface;
    CarveBlocks cb;
    GenRange range;
    std::vector<std::vector<std::string>> biome_carvers;
    struct Carver {
        std::string type;   // cave, nether_cave, canyon
        const Json* config;
        float probability;
        std::vector<uint8_t> replaceable;   // per block id
    };
    std::map<std::string, Carver> carvers;

    // per-chunk carving state
    struct Job {
        ChunkBlocks& blocks;
        int chunk_x, chunk_z;
        ChunkFill& fill;
        std::vector<uint8_t> mask;
        const std::function<int(int, int, int)>* lookup = nullptr;
        Job(ChunkBlocks& b, int cx, int cz, ChunkFill& f) : blocks(b), chunk_x(cx), chunk_z(cz), fill(f), mask(b.b.size(), 0) {}
    };
    using Skip = std::function<bool(double, double, double, int)>;

    const Carver& carver(const std::string& id, size_t nblocks, const std::function<std::string(uint16_t)>& name_of) {
        auto it = carvers.find(id);
        if (it != carvers.end()) return it->second;
        Carver c;
        const Json& j = reg.get("configured_carver", id);
        c.type = Registry::strip(j.at("type").str);
        c.config = &j.at("config");
        c.probability = c.config->at("probability").float_value();
        std::set<std::string> members;
        const Json& rep = c.config->at("replaceable");
        auto add = [&](const std::string& e) {
            if (!e.empty() && e[0] == '#') { auto& m = reg.tag_members("block", e); members.insert(m.begin(), m.end()); }
            else members.insert(e);
        };
        if (rep.is_arr()) for (auto& e : rep.arr) add(e.str); else add(rep.str);
        c.replaceable.assign(nblocks, 0);
        for (size_t i = 0; i < nblocks; i++) c.replaceable[i] = members.count(name_of(uint16_t(i))) ? 1 : 0;
        return carvers.emplace(id, std::move(c)).first->second;
    }

    Impl(const Registry& r, const WorldState& s) : reg(r), state(s) {}

    // WorldCarver.carveBlock / NetherWorldCarver.carveBlock
    bool carve_block(Job& job, const Carver& car, int x, int y, int z, bool& has_grass) {
        const bool nether = car.type == "nether_cave";
        uint16_t old = job.blocks.get(x, y, z);
        if (nether) {
            if (!car.replaceable[old]) return false;
            job.blocks.set(x, y, z, y <= range.min_y + 31 ? cb.lava : cb.cave_air, false);
            return true;
        }
        if (old == cb.grass_block || old == cb.mycelium) has_grass = true;
        if (!car.replaceable[old]) return false;
        uint16_t st;
        if (y <= resolve_anchor(car.config->at("lava_level"), range)) st = cb.lava;
        else if (!job.fill.aquifer_substance(x, y, z, 0.0, st)) return false;
        job.blocks.set(x, y, z, st, job.fill.aquifer_should_schedule());
        if (has_grass) {
            if (job.blocks.get(x, y - 1, z) == cb.dirt) {
                uint16_t top;
                if (surface->top_material(job.blocks, job.chunk_x, job.chunk_z, job.fill, x, y - 1, z, job.blocks.fluid(st), top, job.lookup)) job.blocks.set(x, y - 1, z, top);
            }
        }
        return true;
    }

    bool carve_ellipsoid(Job& job, const Carver& car, double x, double y, double z, double hr, double vr, const Skip& skip) {
        double center_x = job.chunk_x * 16 + 8, center_z = job.chunk_z * 16 + 8;
        double max_delta = 16.0 + hr * 2.0;
        if (std::fabs(x - center_x) > max_delta || std::fabs(z - center_z) > max_delta) return false;
        int cmx = job.chunk_x * 16, cmz = job.chunk_z * 16;
        int min_xi = std::max(mth_floor(x - hr) - cmx - 1, 0), max_xi = std::min(mth_floor(x + hr) - cmx, 15);
        int min_y = std::max(mth_floor(y - vr) - 1, range.min_y + 1);
        int max_y = std::min(mth_floor(y + vr) + 1, range.min_y + range.height - 1 - 7);
        int min_zi = std::max(mth_floor(z - hr) - cmz - 1, 0), max_zi = std::min(mth_floor(z + hr) - cmz, 15);
        bool carved = false;
        for (int xi = min_xi; xi <= max_xi; xi++) {
            int wx = cmx + xi;
            double xd = (wx + 0.5 - x) / hr;
            for (int zi = min_zi; zi <= max_zi; zi++) {
                int wz = cmz + zi;
                double zd = (wz + 0.5 - z) / hr;
                if (xd * xd + zd * zd >= 1.0) continue;
                bool has_grass = false;
                for (int wy = max_y; wy > min_y; wy--) {
                    double yd = (wy - 0.5 - y) / vr;
                    if (skip(xd, yd, zd, wy)) continue;
                    size_t mi = size_t(wy - job.blocks.min_y) * 256 + size_t(zi) * 16 + size_t(xi);
                    if (job.mask[mi]) continue;
                    job.mask[mi] = 1;
                    carved |= carve_block(job, car, wx, wy, wz, has_grass);
                }
            }
        }
        return carved;
    }

    static bool can_reach(int chunk_x, int chunk_z, double x, double z, int step, int total, float thickness) {
        double xm = chunk_x * 16 + 8, zm = chunk_z * 16 + 8;
        double xd = x - xm, zd = z - zm;
        double remaining = total - step;
        double rr = double(thickness + 2.0f + 16.0f);
        return xd * xd + zd * zd - remaining * remaining <= rr * rr;
    }

    // ---- caves
    void create_room(Job& job, const Carver& car, double x, double y, double z, float thickness, double y_scale, const Skip& skip) {
        double hr = 1.5 + double(msin(double(kPi / 2.0f)) * thickness);
        double vr = hr * y_scale;
        carve_ellipsoid(job, car, x + 1.0, y, z, hr, vr, skip);
    }

    void create_tunnel(Job& job, const Carver& car, int64_t tunnel_seed, double x, double y, double z, double hr_mult, double vr_mult, float thickness, float h_rot, float v_rot, int step,
                       int dist, double y_scale, const Skip& skip) {
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
                    create_tunnel(job, car, sub_seed, x, y, z, hr_mult, vr_mult, sub_thickness, side < 0 ? h_rot - kPi / 2.0f : h_rot + kPi / 2.0f, v_rot / 3.0f, cur, dist, 1.0, skip);
                }
                return;
            }
            if (random.next_int(4) != 0) {
                if (!can_reach(job.chunk_x, job.chunk_z, x, z, cur, dist, thickness)) return;
                carve_ellipsoid(job, car, x, y, z, hr * hr_mult, vr * vr_mult, skip);
            }
        }
    }

    void carve_cave(Job& job, const Carver& car, Rng& random, int src_x, int src_z) {
        const bool nether = car.type == "nether_cave";
        const Json& cfg = *car.config;
        int max_distance = (4 * 2 - 1) * 16;
        int bound = nether ? 10 : 15;
        int cave_count = random.next_int(random.next_int(random.next_int(bound) + 1) + 1);
        for (int cave = 0; cave < cave_count; cave++) {
            double x = double(src_x * 16 + random.next_int(16));
            double y = double(sample_height(cfg.at("y"), random, range));
            double z = double(src_z * 16 + random.next_int(16));
            double h_mult = double(sample_float(cfg.at("horizontal_radius_multiplier"), random));
            double v_mult = double(sample_float(cfg.at("vertical_radius_multiplier"), random));
            double floor_level = double(sample_float(cfg.at("floor_level"), random));
            Skip skip = [floor_level](double xd, double yd, double zd, int) { return yd <= floor_level ? true : xd * xd + yd * yd + zd * zd >= 1.0; };
            int tunnels = 1;
            if (random.next_int(4) == 0) {
                double y_scale = double(sample_float(cfg.at("yScale"), random));
                float thickness = 1.0f + random.next_float() * 6.0f;
                create_room(job, car, x, y, z, thickness, y_scale, skip);
                tunnels += random.next_int(4);
            }
            for (int i = 0; i < tunnels; i++) {
                float h_rot = random.next_float() * (kPi * 2.0f);
                float v_rot = (random.next_float() - 0.5f) / 4.0f;
                float thickness;
                {
                    float a = random.next_float();
                    float b = random.next_float();
                    if (nether) thickness = (a * 2.0f + b) * 2.0f;
                    else {
                        thickness = a * 2.0f + b;
                        if (random.next_int(10) == 0) {
                            float c = random.next_float();
                            float d = random.next_float();
                            thickness *= c * d * 3.0f + 1.0f;
                        }
                    }
                }
                int distance = max_distance - random.next_int(max_distance / 4);
                create_tunnel(job, car, random.next_long(), x, y, z, h_mult, v_mult, thickness, h_rot, v_rot, 0, distance, nether ? 5.0 : 1.0, skip);
            }
        }
    }

    // ---- canyons
    void carve_canyon(Job& job, const Carver& car, Rng& random, int src_x, int src_z) {
        const Json& cfg = *car.config;
        const Json& shape = cfg.at("shape");
        int max_distance = (4 * 2 - 1) * 16;
        double x = double(src_x * 16 + random.next_int(16));
        int y = sample_height(cfg.at("y"), random, range);
        double z = double(src_z * 16 + random.next_int(16));
        float h_rot = random.next_float() * (kPi * 2.0f);
        float v_rot = sample_float(cfg.at("vertical_rotation"), random);
        double y_scale = double(sample_float(cfg.at("yScale"), random));
        float thickness = sample_float(shape.at("thickness"), random);
        int distance = int(double(max_distance) * double(sample_float(shape.at("distance_factor"), random)));
        canyon_do_carve(job, car, random.next_long(), x, double(y), z, thickness, h_rot, v_rot, distance, y_scale, shape);
    }

    void canyon_do_carve(Job& job, const Carver& car, int64_t tunnel_seed, double x, double y, double z, float thickness, float h_rot, float v_rot, int distance, double y_scale, const Json& shape) {
        LegacyRandom r(tunnel_seed);
        const int depth = range.height;
        std::vector<float> widths(size_t(depth), 0.0f);
        float wf = 1.0f;
        int smooth = int(shape.at("width_smoothness").int_value());
        for (int i = 0; i < depth; i++) {
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
                if (!can_reach(job.chunk_x, job.chunk_z, x, z, cur, distance, thickness)) return;
                carve_ellipsoid(job, car, x, y, z, hr, vr, skip);
            }
        }
    }

    const std::vector<std::string>& carvers_for(int biome) const { return biome_carvers[size_t(biome)]; }
};

Carvers::~Carvers() = default;

Carvers::Carvers(const Registry& reg, const WorldState& state, const SurfaceSystem* surface, const CarveBlocks& cb, size_t blocks_count, std::function<std::string(uint16_t)> name_of, GenRange range,
                 std::vector<std::vector<std::string>> carvers_of_biome)
    : p_(std::make_unique<Impl>(reg, state)) {
    Impl& I = *p_;
    I.surface = surface;
    I.cb = cb;
    I.range = range;
    I.biome_carvers = std::move(carvers_of_biome);
    for (auto& list : I.biome_carvers)
        for (auto& id : list) I.carver(id, blocks_count, name_of);
}

void Carvers::carve(ChunkBlocks& blocks, int chunk_x, int chunk_z, ChunkFill& fill, const std::function<int(int, int)>& source_biome, const std::function<int(int, int, int)>* lookup) const {
    Impl& I = *p_;
    Impl::Job job(blocks, chunk_x, chunk_z, fill);
    job.lookup = lookup;
    const int64_t seed = I.state.seed();
    LegacyRandom random(0);
    for (int dx = -8; dx <= 8; dx++)
        for (int dz = -8; dz <= 8; dz++) {
            int sx = chunk_x + dx, sz = chunk_z + dz;
            const auto& list = I.carvers_for(source_biome(sx * 4, sz * 4));
            int index = 0;
            for (const std::string& id : list) {
                const Impl::Carver& car = I.carvers.at(id);
                // WorldgenRandom.setLargeFeatureSeed
                int64_t base = seed + index;
                random.set_seed(base);
                int64_t xs = random.next_long(), zs = random.next_long();
                random.set_seed(int64_t(uint64_t(int64_t(sx)) * uint64_t(xs)) ^ int64_t(uint64_t(int64_t(sz)) * uint64_t(zs)) ^ base);
                if (random.next_float() <= car.probability) {
                    if (car.type == "canyon") I.carve_canyon(job, car, random, sx, sz);
                    else I.carve_cave(job, car, random, sx, sz);
                }
                index++;
            }
        }
}

}  // namespace pn::vg
