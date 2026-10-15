#include "world.hpp"
#include "vg/gen.hpp"
#include "perf.hpp"
#include "dimension.hpp"
#include "worldgen.hpp"
#include "anvil.hpp"
#include "log.hpp"
#include "server.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <dirent.h>
#include <fstream>
#include "json.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <set>
#include <sys/stat.h>

namespace pn {

static int64_t ckey(int x, int z) { return (static_cast<int64_t>(x) << 32) | static_cast<uint32_t>(z); }

void Chunk::set(int lx, int y, int lz, uint16_t id) {
    int s = (y - kMinY) >> 4;
    if (s < 0 || s >= kSections) return;
    if (!sec[s]) {
        if (id == 0) return;
        sec[s] = std::make_unique<Section>();
        memcpy(sec[s]->biome, air_biomes[s], 64);
    }
    sec[s]->b[((y & 15) << 8) | (lz << 4) | lx] = id;
    version++;
    dirty = true;
}


// Mojang's generator (src/vg) needs the extracted worldgen data; null when it is missing or the dimension is not supported
static std::unique_ptr<vg::VanillaGen> make_vanilla_gen(int dim, uint64_t seed, const std::string& level_type, const std::vector<std::string>& biome_names) {
    std::string exe_dir;
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) { buf[n] = 0; exe_dir = buf; exe_dir = exe_dir.substr(0, exe_dir.rfind('/')); }
    }
    for (std::string d : {std::string("data"), std::string("../data"), exe_dir + "/data", exe_dir + "/../data"}) {
        std::string root = d + "/worldgen/26.2";
        if (access((root + "/.done").c_str(), R_OK) != 0 || access((root + "/blockflags.txt").c_str(), R_OK) != 0) continue;
        try {
            vg::GenOptions o;
            o.version = "26.2";
            o.dim = dim;
            o.seed = int64_t(seed);
            o.level_type = level_type;
            return std::make_unique<vg::VanillaGen>(d, o, biome_names);
        } catch (const std::exception& e) {
            warn(std::string("vanilla world generator unavailable: ") + e.what());
            return nullptr;
        }
    }
    return nullptr;
}

World::World() {}
World::~World() { if (lock_fd_ >= 0) ::close(lock_fd_); }

static bool is_dir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

uint8_t World::biome_index(const std::string& name) const {
    for (size_t i = 0; i < biome_names.size(); i++) if (biome_names[i] == name) return static_cast<uint8_t>(i);
    for (size_t i = 0; i < biome_names.size(); i++) if (biome_names[i] == "minecraft:plains") return static_cast<uint8_t>(i);
    return 0;
}

bool World::open(const std::string& d, const std::string& generator, const std::string& level_type, const std::string& seed_str, const VanillaData& newest, std::string& err) {
    dir = d;
    biome_names = newest.biomes;
    mkdir(dir.c_str(), 0755);
    // the same lock vanilla and Paper take (an fcntl write lock on session.lock), so two servers never write one world at the same time
    lock_fd_ = ::open((dir + "/session.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd_ >= 0) {
        struct flock fl{};
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        if (fcntl(lock_fd_, F_SETLK, &fl) != 0) {
            err = "the world \"" + dir + "\" is locked by another server process (is Paper or vanilla running on it?)";
            return false;
        }
        (void)!ftruncate(lock_fd_, 0);
        (void)!write(lock_fd_, "\xE2\x98\x83", 3);
    }
    data_version = newest.data_version;
    mkdir(dir.c_str(), 0755);
    // vanilla layouts: world/region (<= 1.21) or world/dimensions/minecraft/overworld/region (26.x)
    std::string a = dir + "/dimensions/minecraft/overworld/region", b = dir + "/region";
    if (region_dir_has_chunks(a)) region_dir = a;
    else if (region_dir_has_chunks(b)) region_dir = b;
    else region_dir = (is_dir(dir + "/dimensions") || newest.data_version >= 4900) ? a : b;
    if (region_dir == a) {
        mkdir((dir + "/dimensions").c_str(), 0755);
        mkdir((dir + "/dimensions/minecraft").c_str(), 0755);
        mkdir((dir + "/dimensions/minecraft/overworld").c_str(), 0755);
    }
    mkdir(region_dir.c_str(), 0755);
    bool existing = region_dir_has_chunks(region_dir);
    // what fills chunks that are not on disk: remembered per world (painite.json) so a changed setting cannot put terrain next to a flat area
    std::string kind = generator;
    std::string stored_type, stored_seed;
    {
        std::ifstream pj(dir + "/painite.json");
        std::string txt((std::istreambuf_iterator<char>(pj)), {});
        JP jp{txt.data(), txt.data() + txt.size()};
        JV v = jp.val();
        if (!jp.bad && v.t == JV::Obj) {
            if (auto* t = v.get("type")) stored_type = t->s;
            if (auto* s2 = v.get("seed")) stored_seed = s2->s;
        }
    }
    std::string lt = level_type;
    if (lt == "superflat") lt = "flat";
    if (!stored_type.empty()) { lt = stored_type; kind = "terrain"; }
    else if (kind == "auto") kind = existing ? "void" : (lt == "void" ? "void" : lt == "flat" ? "flat" : "terrain");
    GenKind gk = kind == "void" ? GenKind::Void : kind == "flat" ? GenKind::Flat : lt == "large_biomes" ? GenKind::LargeBiomes : lt == "amplified" ? GenKind::Amplified : GenKind::Normal;
    if (kind == "terrain" && (lt == "flat" || lt == "void")) gk = lt == "flat" ? GenKind::Flat : GenKind::Void;
    seed = stored_seed.empty() ? seed_from_string(seed_str) : static_cast<uint64_t>(strtoll(stored_seed.c_str(), nullptr, 10));
    gen = std::make_unique<WorldGen>(gk, seed, biome_names);
    if (generator == "vanilla" && (gk == GenKind::Normal || gk == GenKind::LargeBiomes || gk == GenKind::Amplified))
        vgen = make_vanilla_gen(0, seed, lt, biome_names);
    gen_name = gk == GenKind::Void ? "void" : gk == GenKind::Flat ? "flat" : gk == GenKind::LargeBiomes ? "large_biomes" : gk == GenKind::Amplified ? "amplified" : "normal";
    if (!existing && gk != GenKind::Void) {
        std::ofstream pj(dir + "/painite.json");
        pj << "{\"type\": \"" << gen_name << "\", \"seed\": " << static_cast<long long>(seed) << "}\n";
    }
    have_level_ = read_nbt_file(dir + "/level.dat", level_);
    if (have_level_) {
        const Nbt* data = level_.get("Data");
        if (data) {
            if (const Nbt* sp = data->get("spawn")) {  // 26.x
                if (const Nbt* pos = sp->get("pos")) if (pos->type == Nbt::IntArray && pos->ia.size() == 3) { spawn_x = pos->ia[0]; spawn_y = pos->ia[1]; spawn_z = pos->ia[2]; }
            } else if (data->get("SpawnX")) {
                spawn_x = static_cast<int>(data->geti("SpawnX"));
                spawn_y = static_cast<int>(data->geti("SpawnY", -60));
                spawn_z = static_cast<int>(data->geti("SpawnZ"));
            }
            game_time = data->geti("Time");
            day_time = data->geti("DayTime", game_time);
            if (const Nbt* ds = data->get("difficulty_settings")) {
                std::string dn = ds->gets("difficulty");
                difficulty = dn == "peaceful" ? 0 : dn == "easy" ? 1 : dn == "hard" ? 3 : 2;
            } else if (data->get("Difficulty")) difficulty = static_cast<int>(data->geti("Difficulty", 2));
            // 26.x keeps the time of day in data/minecraft/world_clocks.dat
            Nbt clocks;
            if (read_nbt_file(dir + "/data/minecraft/world_clocks.dat", clocks))
                if (const Nbt* cd = clocks.get("data")) if (const Nbt* ov = cd->get("minecraft:overworld")) if (ov->get("total_ticks")) day_time = ov->geti("total_ticks");
            if (int dv = static_cast<int>(data->geti("DataVersion"))) data_version = dv;
        }
    }
    if (!have_level_ && (gk == GenKind::Normal || gk == GenKind::LargeBiomes || gk == GenKind::Amplified)) {
        int sx, sz;
        gen->find_spawn(sx, sz);
        spawn_x = sx;
        spawn_z = sz;
        spawn_y = gen->surface_height(sx, sz) + 1;
        if (vgen) {   // the first dry column around the origin in Mojang's terrain
            bool found = false;
            for (int r = 0; r < 12 && !found; r++)
                for (int i = -r; i <= r && !found; i++)
                    for (int j = -r; j <= r && !found; j++) {
                        if (std::max(std::abs(i), std::abs(j)) != r) continue;
                        Chunk tmp;
                        tmp.x = i; tmp.z = j;
                        vgen->generate(tmp);
                        for (int y = 319; y >= 63; y--) {
                            uint16_t b = tmp.get(8, y, 8);
                            if (b == 0) continue;
                            const BlockInfo& bi = Blocks::get().info(b);
                            if (bi.fluid || bi.name == "minecraft:ice" || bi.name.find("leaves") != std::string::npos) break;
                            spawn_x = i * 16 + 8; spawn_z = j * 16 + 8; spawn_y = y + 1;
                            found = true;
                            break;
                        }
                    }
        }
    }
    info(std::string("World \"") + dir + "\": " + (existing ? "existing region files in " + region_dir : std::string("no chunks yet")) + (", missing chunks: " + gen_name + (gk == GenKind::Normal || gk == GenKind::LargeBiomes || gk == GenKind::Amplified ? " terrain, seed " + std::to_string(static_cast<long long>(seed)) : std::string())));
    (void)err;
    return true;
}

bool World::open_dimension(const World& ow, int d, std::string& err) {
    dim = d;
    dir = ow.dir;
    biome_names = ow.biome_names;
    data_version = ow.data_version;
    seed = ow.seed;
    bool layout26 = ow.region_dir == ow.dir + "/dimensions/minecraft/overworld/region";
    if (layout26) {
        mkdir((dir + "/dimensions").c_str(), 0755);
        mkdir((dir + "/dimensions/minecraft").c_str(), 0755);
        std::string base = dir + "/dimensions/minecraft/" + dim_info(d).dir_name;
        mkdir(base.c_str(), 0755);
        region_dir = base + "/region";
    } else {
        std::string base = dir + (d == 1 ? "/DIM-1" : "/DIM1");
        mkdir(base.c_str(), 0755);
        region_dir = base + "/region";
    }
    mkdir(region_dir.c_str(), 0755);
    gen = std::make_unique<WorldGen>(d == 1 ? GenKind::Nether : GenKind::End, seed, biome_names);
    if (ow.vgen) vgen = make_vanilla_gen(d, seed, "normal", biome_names);
    gen_name = d == 1 ? "nether" : "end";
    (void)err;
    return true;
}

static int ceil_log2(int v) { int b = 0; while ((1 << b) < v) b++; return b; }

bool World::from_nbt(Chunk& c, const Nbt& root) {
    const Blocks& B = Blocks::get();
    const Nbt* secs = root.get("sections");
    if (!secs || secs->type != Nbt::List) return false;
    c.extra = Nbt::compound();
    for (auto& kv : root.map) if (kv.first != "sections" && kv.first != "Heightmaps" && kv.first != "isLightOn" && kv.first != "Lights") c.extra.map.push_back(kv);
    for (auto& s : secs->list) {
        int y = static_cast<int>(s.geti("Y"));
        int idx = y - (kMinY >> 4);
        if (idx < 0 || idx >= kSections) continue;
        auto sec = std::make_unique<Section>();
        const Nbt* bs = s.get("block_states");
        if (bs) {
            const Nbt* pal = bs->get("palette");
            std::vector<uint16_t> map;
            if (pal) for (auto& e : pal->list) {
                std::string props;
                std::string name = e.type == Nbt::String ? e.s : e.gets("id");
                if (name.empty()) name = e.gets("Name");
                if (name.empty()) for (auto& kv : e.map) if (kv.second.type == Nbt::String) { name = kv.second.s; break; }  // 26.x writes plain blocks as {"": name}
                const Nbt* pr = e.get("properties");
                if (!pr) pr = e.get("Properties");
                if (pr) {
                    std::vector<std::pair<std::string, std::string>> kv;
                    for (auto& p : pr->map) kv.emplace_back(p.first, p.second.type == Nbt::String ? p.second.s : std::to_string(p.second.i));
                    std::sort(kv.begin(), kv.end());
                    for (auto& p : kv) props += (props.empty() ? "" : ",") + p.first + "=" + p.second;
                }
                if (!B.known(name)) {
                    static std::mutex um;
                    static std::set<std::string> seen;
                    std::lock_guard<std::mutex> ul(um);
                    if (seen.size() < 40 && seen.insert(name).second) warn("world contains the unknown block \"" + name + "\" (shown as air; probably a pre-upgrade chunk)");
                }
                map.push_back(B.find(name, props));
            }
            const Nbt* data = bs->get("data");
            if (map.size() == 1 || !data || data->la.empty()) {
                if (!map.empty()) std::fill(sec->b, sec->b + 4096, map[0]);
            } else {
                int bits = std::max(4, ceil_log2(static_cast<int>(map.size())));
                int per = 64 / bits;
                uint64_t mask = (1ull << bits) - 1;
                for (int i = 0; i < 4096; i++) {
                    size_t li = static_cast<size_t>(i / per);
                    if (li >= data->la.size()) break;
                    uint32_t v = static_cast<uint32_t>((static_cast<uint64_t>(data->la[li]) >> ((i % per) * bits)) & mask);
                    sec->b[i] = v < map.size() ? map[v] : 0;
                }
            }
        }
        if (const Nbt* bm = s.get("biomes")) {
            const Nbt* pal = bm->get("palette");
            std::vector<uint8_t> map;
            if (pal) for (auto& e : pal->list) map.push_back(biome_index(e.type == Nbt::String ? e.s : ""));
            const Nbt* data = bm->get("data");
            if (map.size() == 1 || !data || data->la.empty()) std::fill(sec->biome, sec->biome + 64, map.empty() ? biome_index("minecraft:plains") : map[0]);
            else {
                int bits = std::max(1, ceil_log2(static_cast<int>(map.size())));
                int per = 64 / bits;
                uint64_t mask = (1ull << bits) - 1;
                for (int i = 0; i < 64; i++) {
                    size_t li = static_cast<size_t>(i / per);
                    if (li >= data->la.size()) break;
                    uint32_t v = static_cast<uint32_t>((static_cast<uint64_t>(data->la[li]) >> ((i % per) * bits)) & mask);
                    sec->biome[i] = v < map.size() ? map[v] : 0;
                }
            }
        }
        bool empty = true;
        for (int i = 0; i < 4096 && empty; i++) if (sec->b[i]) empty = false;
        if (empty) memcpy(c.air_biomes[idx], sec->biome, 64);     // air only: no Section object, the biomes are kept for saving
        else c.sec[idx] = std::move(sec);
    }
    return true;
}

static Nbt packed(const std::vector<uint16_t>& idx, int bits, size_t n) {
    int per = 64 / bits;
    std::vector<int64_t> la((n + static_cast<size_t>(per) - 1) / static_cast<size_t>(per), 0);
    for (size_t i = 0; i < n; i++) la[i / static_cast<size_t>(per)] |= static_cast<int64_t>(static_cast<uint64_t>(idx[i]) << ((i % static_cast<size_t>(per)) * static_cast<size_t>(bits)));
    return Nbt::longs(std::move(la));
}

void World::to_nbt(Chunk& c, Nbt& root) {
    const Blocks& B = Blocks::get();
    root = c.extra;
    if (root.type != Nbt::Compound) root = Nbt::compound();
    root.put("DataVersion", Nbt::make(Nbt::Int, data_version));
    root.put("xPos", Nbt::make(Nbt::Int, c.x));
    root.put("zPos", Nbt::make(Nbt::Int, c.z));
    root.put("yPos", Nbt::make(Nbt::Int, kMinY >> 4));
    if (!root.get("Status")) root.put("Status", Nbt::str("minecraft:full"));
    Nbt secs = Nbt::list_of(Nbt::Compound);
    for (int s = 0; s < kSections; s++) {
        Nbt sn = Nbt::compound();
        sn.put("Y", Nbt::make(Nbt::Byte, s + (kMinY >> 4)));
        Section tmp;   // air only sections carry just their biomes
        const Section* sp = c.sec[s].get();
        if (!sp) { memcpy(tmp.biome, c.air_biomes[s], 64); sp = &tmp; }
        std::vector<uint16_t> pal, idx(4096);
        std::unordered_map<uint16_t, uint16_t> pm;
        for (int i = 0; i < 4096; i++) {
            uint16_t b = sp->b[i];
            auto it = pm.find(b);
            if (it == pm.end()) { it = pm.emplace(b, static_cast<uint16_t>(pal.size())).first; pal.push_back(b); }
            idx[static_cast<size_t>(i)] = it->second;
        }
        Nbt bs = Nbt::compound(), plist = Nbt::list_of(Nbt::Compound);
        for (uint16_t b : pal) {
            const BlockInfo& bi = B.info(b);
            Nbt e = Nbt::compound();
            if (bi.props.empty()) e.map.emplace_back("", Nbt::str(bi.name));  // same shape vanilla 26.x writes
            else e.put("id", Nbt::str(bi.name));
            if (!bi.props.empty()) {
                Nbt pr = Nbt::compound();
                size_t i = 0;
                while (i < bi.props.size()) {
                    size_t e1 = bi.props.find(',', i);
                    std::string kv = bi.props.substr(i, e1 == std::string::npos ? std::string::npos : e1 - i);
                    size_t eq = kv.find('=');
                    pr.put(kv.substr(0, eq), Nbt::str(kv.substr(eq + 1)));
                    if (e1 == std::string::npos) break;
                    i = e1 + 1;
                }
                e.put("properties", std::move(pr));
            }
            plist.list.push_back(std::move(e));
        }
        bool plain = true;
        for (uint16_t b : pal) if (!B.info(b).props.empty()) plain = false;
        if (plain) {  // vanilla 26.x writes a list of strings when no entry has properties
            plist = Nbt::list_of(Nbt::String);
            for (uint16_t b : pal) plist.list.push_back(Nbt::str(B.info(b).name));
        }
        bs.put("palette", std::move(plist));
        if (pal.size() > 1) bs.put("data", packed(idx, std::max(4, ceil_log2(static_cast<int>(pal.size()))), 4096));
        sn.put("block_states", std::move(bs));
        Nbt bm = Nbt::compound(), bpal = Nbt::list_of(Nbt::String);
        std::vector<uint16_t> bp, bidx(64);
        std::unordered_map<uint16_t, uint16_t> bmap;
        for (int i = 0; i < 64; i++) {
            uint16_t b = sp->biome[i];
            auto it = bmap.find(b);
            if (it == bmap.end()) { it = bmap.emplace(b, static_cast<uint16_t>(bp.size())).first; bp.push_back(b); }
            bidx[static_cast<size_t>(i)] = it->second;
        }
        for (uint16_t b : bp) bpal.list.push_back(Nbt::str(b < biome_names.size() ? biome_names[b] : "minecraft:plains"));
        bm.put("palette", std::move(bpal));
        if (bp.size() > 1) bm.put("data", packed(bidx, std::max(1, ceil_log2(static_cast<int>(bp.size()))), 64));
        sn.put("biomes", std::move(bm));
        secs.list.push_back(std::move(sn));
    }
    root.put("sections", std::move(secs));
}

std::shared_ptr<Chunk> World::load_chunk(int cx, int cz) {
    PN_PERF("chunk.load");
    auto c = std::make_shared<Chunk>();
    c->x = cx;
    c->z = cz;
    memset(c->air_biomes, biome_index("minecraft:plains"), sizeof c->air_biomes);
    Nbt root;
    if (anvil_read(region_dir, cx, cz, root) && from_nbt(*c, root)) {
        // loaded
    } else if (vgen) {
        vgen->generate(*c);
        c->dirty = true;
    } else if (gen) {
        gen->generate(*c);
        c->dirty = gen->kind() != GenKind::Void;
    } else {
        c->extra = Nbt::compound();
    }
    return c;
}

std::shared_ptr<Chunk> World::chunk(int cx, int cz) {
    int64_t key = ckey(cx, cz);
    std::unique_lock<std::mutex> l(mu_);
    for (;;) {
        auto it = chunks_.find(key);
        if (it != chunks_.end()) { it->second->last_used_ms = now_ms(); return it->second; }
        if (loading_.count(key)) { loaded_cv_.wait(l); continue; }
        loading_.insert(key);
        l.unlock();
        std::shared_ptr<Chunk> c;
        try { c = load_chunk(cx, cz); } catch (...) { l.lock(); loading_.erase(key); loaded_cv_.notify_all(); throw; }
        l.lock();
        c->last_used_ms = now_ms();
        chunks_[key] = c;
        loading_.erase(key);
        loaded_cv_.notify_all();
        return c;
    }
}

std::shared_ptr<Chunk> World::chunk_if_loaded(int cx, int cz) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = chunks_.find(ckey(cx, cz));
    if (it == chunks_.end()) return nullptr;
    it->second->last_used_ms = now_ms();
    return it->second;
}

int World::loaded() { std::lock_guard<std::mutex> l(mu_); return static_cast<int>(chunks_.size()); }

uint16_t World::get_block(int x, int y, int z) {
    auto c = chunk(x >> 4, z >> 4);
    std::lock_guard<std::mutex> l(c->mu);
    return c->get(x & 15, y, z & 15);
}

bool World::set_block(int x, int y, int z, uint16_t id) {
    if (y < kMinY || y >= kMinY + kHeight) return false;
    auto c = chunk(x >> 4, z >> 4);
    std::lock_guard<std::mutex> l(c->mu);
    if (c->get(x & 15, y, z & 15) == id) return false;
    c->set(x & 15, y, z & 15, id);
    // block entities at that spot no longer match the block
    if (Nbt* be = c->extra.get("block_entities")) {
        auto& v = be->list;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Nbt& e) { return e.geti("x") == x && e.geti("y") == y && e.geti("z") == z; }), v.end());
    }
    blocks_changed++;
    return true;
}

int World::top_y(int x, int z) {
    auto c = chunk(x >> 4, z >> 4);
    std::lock_guard<std::mutex> l(c->mu);
    const Blocks& B = Blocks::get();
    for (int y = kMinY + kHeight - 1; y >= kMinY; y--) {
        uint16_t b = c->get(x & 15, y, z & 15);
        if (b && B.info(b).solid) return y;
    }
    return kMinY - 1;
}

void World::save(bool unload_idle, int64_t now) {
    std::vector<std::shared_ptr<Chunk>> list;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& kv : chunks_) list.push_back(kv.second);
    }
    int saved = 0;
    for (auto& c : list) {
        Nbt root;
        {
            std::lock_guard<std::mutex> l(c->mu);
            if (!c->dirty) continue;
            to_nbt(*c, root);
            c->dirty = false;
        }
        if (anvil_write(region_dir, c->x, c->z, root)) saved++;
        else warn("could not write chunk " + std::to_string(c->x) + "," + std::to_string(c->z));
    }
    if (unload_idle) {
        std::lock_guard<std::mutex> l(mu_);
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            auto& c = it->second;
            if (c.use_count() == 1 && now - c->last_used_ms > 60000 && !c->dirty) it = chunks_.erase(it);
            else ++it;
        }
    }
    (void)saved;
}

void World::save_level() {
    Nbt clocks;
    if (read_nbt_file(dir + "/data/minecraft/world_clocks.dat", clocks))
        if (Nbt* cd = clocks.get("data")) if (Nbt* ov = cd->get("minecraft:overworld")) {
            ov->put("total_ticks", Nbt::make(Nbt::Long, day_time));
            write_nbt_file(dir + "/data/minecraft/world_clocks.dat", clocks);
        }
    if (!have_level_) return;
    Nbt* d = level_.get("Data");
    if (!d) return;
    d->put("Time", Nbt::make(Nbt::Long, game_time));
    d->put("DayTime", Nbt::make(Nbt::Long, day_time));
    d->put("LastPlayed", Nbt::make(Nbt::Long, static_cast<int64_t>(time(nullptr)) * 1000));
    write_nbt_file(dir + "/level.dat", level_);
}

World::Spawn World::pick_spawn(const std::string& uuid, int default_gamemode) {
    Spawn s{spawn_x + 0.5, static_cast<double>(spawn_y), spawn_z + 0.5, 0, 0, default_gamemode, false};
    Nbt pd;
    if (load_player(uuid, pd)) {
        std::string dim = pd.gets("Dimension");
        const Nbt* pos = pd.get("Pos");
        s.dim = dim_from_name(dim);
        if (pos && pos->type == Nbt::List && pos->list.size() == 3) {
            double x = pos->list[0].f, y = pos->list[1].f, z = pos->list[2].f;
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::fabs(x) < 3e7 && std::fabs(z) < 3e7) {
                s.x = x; s.y = y; s.z = z;
                if (const Nbt* rot = pd.get("Rotation")) if (rot->list.size() == 2) { s.yaw = static_cast<float>(rot->list[0].f); s.pitch = static_cast<float>(rot->list[1].f); }
                s.gamemode = static_cast<int>(pd.geti("playerGameType", default_gamemode));
                s.saved = true;
                if (s.dim) return s;
                return s;
            }
        }
    }
    int top = top_y(spawn_x, spawn_z);
    if (top >= kMinY) s.y = top + 1;
    return s;
}

// 26.x keeps player files in players/data, older worlds in playerdata
std::string World::player_dir() const { return is_dir(dir + "/players/data") || (data_version >= 4900 && !is_dir(dir + "/playerdata")) ? dir + "/players/data" : dir + "/playerdata"; }

bool World::load_player(const std::string& uuid, Nbt& out) { return read_nbt_file(player_dir() + "/" + uuid + ".dat", out); }

bool World::save_player(const std::string& uuid, const Nbt& out) {
    std::string d = player_dir();
    if (d.size() > 12 && d.substr(d.size() - 12) == "players/data") mkdir((dir + "/players").c_str(), 0755);
    mkdir(d.c_str(), 0755);
    return write_nbt_file(d + "/" + uuid + ".dat", out);
}

// ---- lighting -------------------------------------------------------------------------------------------

static void compute_light_overworld(const Chunk& c, Light& L) {
    PN_PERF("chunk.light");
    const Blocks& B = Blocks::get();
    const int S = kSections + 2;
    L.height_ws.assign(256, kMinY);
    L.height_mb.assign(256, kMinY);
    // everything above the highest section that holds a block is open sky: only the part below needs work
    int top = -1;
    for (int s = kSections - 1; s >= 0; s--) if (c.sec[s]) { top = s; break; }
    L.sky.assign(static_cast<size_t>(S) * 2048, 0xff);
    L.blk.assign(static_cast<size_t>(S) * 2048, 0);
    if (top < 0) return;
    const int H = std::min(kHeight, (top + 1) * 16);   // rows computed; row H and above are air with full skylight
    const size_t N = static_cast<size_t>(256) * static_cast<size_t>(H);
    std::vector<uint8_t> op(N, 0);
    auto at = [&](int x, int y, int z) -> size_t { return (static_cast<size_t>(y) * 16 + static_cast<size_t>(z)) * 16 + static_cast<size_t>(x); };
    std::vector<uint32_t> bq;
    std::vector<uint8_t> bl;
    bool any_emit = false;
    for (int s = 0; s <= top; s++) {
        if (!c.sec[s]) continue;
        for (int i = 0; i < 4096; i++) {
            uint16_t b = c.sec[s]->b[i];
            if (!b) continue;
            int x = i & 15, z = (i >> 4) & 15, y = s * 16 + (i >> 8);
            const BlockInfo& bi = B.info(b);
            size_t ix = at(x, y, z);
            op[ix] = bi.opacity;
            if (!bi.air) {
                auto& h = L.height_ws[static_cast<size_t>(z * 16 + x)];
                h = static_cast<int16_t>(std::max<int>(h, y + kMinY + 1));
            }
            if ((bi.solid || bi.fluid) && !bi.air) {
                auto& h = L.height_mb[static_cast<size_t>(z * 16 + x)];
                h = static_cast<int16_t>(std::max<int>(h, y + kMinY + 1));
            }
            if (bi.emit) {
                if (!any_emit) { any_emit = true; bl.assign(N, 0); }
                bl[ix] = bi.emit;
                bq.push_back(static_cast<uint32_t>(ix));
            }
        }
    }
    std::vector<uint8_t> light(N, 0);
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int l = 15;
            for (int y = H - 1; y >= 0 && l > 0; y--) {
                l = std::max(0, l - op[at(x, y, z)]);
                light[at(x, y, z)] = static_cast<uint8_t>(l);
            }
        }
    static const int dx[6] = {1, -1, 0, 0, 0, 0}, dy[6] = {0, 0, 1, -1, 0, 0}, dz[6] = {0, 0, 0, 0, 1, -1};
    // spill sideways under overhangs and into caves: only cells with a darker neighbour can brighten anything
    std::vector<uint32_t> q;
    for (int y = 0; y < H; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                size_t ix = at(x, y, z);
                int l = light[ix];
                if (l <= 1) continue;
                bool edge = false;
                for (int d = 0; d < 6 && !edge; d++) {
                    int nx = x + dx[d], ny = y + dy[d], nz = z + dz[d];
                    if (nx < 0 || nx > 15 || nz < 0 || nz > 15 || ny < 0 || ny >= H) continue;
                    size_t ni = at(nx, ny, nz);
                    if (op[ni] < 15 && light[ni] + 1 < l) edge = true;
                }
                if (edge) q.push_back(static_cast<uint32_t>(ix));
            }
    auto flood = [&](std::vector<uint32_t>& queue, std::vector<uint8_t>& lv) {
        for (size_t h = 0; h < queue.size(); h++) {
            uint32_t cur = queue[h];
            int x = cur & 15, z = (cur >> 4) & 15, y = static_cast<int>(cur >> 8);
            int l = lv[cur];
            if (l <= 1) continue;
            for (int d = 0; d < 6; d++) {
                int nx = x + dx[d], ny = y + dy[d], nz = z + dz[d];
                if (nx < 0 || nx > 15 || nz < 0 || nz > 15 || ny < 0 || ny >= H) continue;
                size_t ni = at(nx, ny, nz);
                if (op[ni] >= 15) continue;
                int nl = l - std::max<int>(1, op[ni]);
                if (nl > lv[ni]) { lv[ni] = static_cast<uint8_t>(nl); queue.push_back(static_cast<uint32_t>(ni)); }
            }
        }
    };
    flood(q, light);
    if (any_emit) flood(bq, bl);
    for (int y = 0; y < H; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                int sidx = (y >> 4) + 1;
                size_t cell = static_cast<size_t>(((y & 15) << 8) | (z << 4) | x);
                size_t bi = static_cast<size_t>(sidx) * 2048 + (cell >> 1);
                size_t li = at(x, y, z);
                uint8_t sv = light[li], bv = any_emit ? bl[li] : 0;
                if (cell & 1) { L.sky[bi] = static_cast<uint8_t>((L.sky[bi] & 0x0f) | (sv << 4)); L.blk[bi] = static_cast<uint8_t>(L.blk[bi] | (bv << 4)); }
                else { L.sky[bi] = static_cast<uint8_t>((L.sky[bi] & 0xf0) | sv); L.blk[bi] = static_cast<uint8_t>(L.blk[bi] | bv); }
            }
    // section index 0 is the one below the world: dark
    std::fill(L.sky.begin(), L.sky.begin() + 2048, 0);
}

void compute_light(const Chunk& c, Light& L, bool sky) {
    compute_light_overworld(c, L);
    if (!sky) std::fill(L.sky.begin(), L.sky.end(), 0);
}

}  // namespace pn
