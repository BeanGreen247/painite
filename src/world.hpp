// The shared overworld: chunk storage, anvil load/save, flat fallback generator, block changes and skylight.
#pragma once
#include "blocks.hpp"
#include "nbt.hpp"
#include "data.hpp"
#include <condition_variable>
#include <memory>
#include <unordered_set>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn {
namespace vg { class VanillaGen; }

constexpr int kMinY = -64, kHeight = 384, kSections = 24;

struct Section {
    uint16_t b[4096];   // (y & 15) << 8 | (z & 15) << 4 | (x & 15)
    uint8_t biome[64];  // 4x4x4 cells, index into World::biome_names
    Section() { memset(b, 0, sizeof b); memset(biome, 0, sizeof biome); }
};

struct Chunk {
    int x = 0, z = 0;
    std::mutex mu;
    std::unique_ptr<Section> sec[kSections];
    Nbt extra;                       // chunk NBT as loaded, minus sections (entities, block entities, ...)
    std::vector<Nbt> gen_entities;   // entities the generator made (structures: witches, guardians, ...), taken over by the entity system
    std::atomic<uint32_t> version{1};
    bool dirty = false;
    int64_t last_used_ms = 0;
    uint8_t air_biomes[kSections][64] = {};   // biome cells of sections that hold only air (those have no Section object)

    uint16_t get(int lx, int y, int lz) const {
        int s = (y - kMinY) >> 4;
        if (s < 0 || s >= kSections || !sec[s]) return 0;
        return sec[s]->b[((y & 15) << 8) | (lz << 4) | lx];
    }
    void set(int lx, int y, int lz, uint16_t id);  // caller holds mu
};

class World {
public:
    World();
    ~World();
    // dir = world directory; the region directory is found (or created) from the vanilla layouts
    // generator: auto | flat | void | terrain; level_type picks the terrain flavour (normal, large_biomes, amplified, flat, void); seed as in level-seed
    // another dimension of the same world folder: shares seed and biome list, own chunks, region folder and generator (no lock, no level.dat)
    bool open_dimension(const World& overworld, int dim_id, std::string& err);
    int dim = 0;
    bool open(const std::string& dir, const std::string& generator, const std::string& level_type, const std::string& seed, const VanillaData& newest, std::string& err);
    std::shared_ptr<Chunk> chunk(int cx, int cz);            // loads or generates
    std::shared_ptr<Chunk> chunk_if_loaded(int cx, int cz);
    uint16_t get_block(int x, int y, int z);
    bool set_block(int x, int y, int z, uint16_t id);        // marks dirty, bumps the chunk version
    int top_y(int x, int z);                                 // y of the highest solid block, or kMinY - 1
    void save(bool unload_idle, int64_t now_ms);
    int loaded() ;
    void memory_use(int64_t& chunks, int64_t& bytes);   // loaded chunks and the bytes of their sections
    std::vector<std::string> biome_names;
    int spawn_x = 0, spawn_y = -60, spawn_z = 0;
    int64_t game_time = 0, day_time = 0;
    int difficulty = 2;                  // 0 peaceful .. 3 hard, from level.dat
    int data_version = 0;
    std::string dir, region_dir;
    std::string gen_version = "26.2";     // data/worldgen/<version> the vanilla generator uses
    std::unique_ptr<vg::VanillaGen> vgen;  // Mojang's generator ported (generator=vanilla); wins over gen when set
    std::unique_ptr<class WorldGen> gen;   // fills chunks that are not on disk; null = leave them empty
    uint64_t seed = 0;
    std::string gen_name;
    std::atomic<int64_t> blocks_changed{0};
    // player data (playerdata/<uuid>.dat)
    bool load_player(const std::string& uuid, Nbt& out);
    bool save_player(const std::string& uuid, const Nbt& out);
    void save_level();
    uint8_t biome_index(const std::string& name) const;
    std::string player_dir() const;
    // last known position from players/data (when there is one), else the world spawn above the surface
    struct Spawn { double x, y, z; float yaw, pitch; int gamemode; bool saved; int dim = 0; };
    Spawn pick_spawn(const std::string& uuid, int default_gamemode);

private:
    std::shared_ptr<Chunk> load_chunk(int cx, int cz);
    bool from_nbt(Chunk& c, const Nbt& root);
    void to_nbt(Chunk& c, Nbt& root);
    std::mutex mu_;
    std::condition_variable loaded_cv_;
    std::unordered_set<int64_t> loading_;
    int lock_fd_ = -1;
    std::unordered_map<int64_t, std::shared_ptr<Chunk>> chunks_;
    Nbt level_;
    bool have_level_ = false;
};

// Light and heightmaps of a chunk, computed for one encode and then dropped (caller holds c.mu).
struct Light {
    std::vector<uint8_t> sky, blk;                 // (kSections + 2) * 2048 nibbles, section index + 1
    std::vector<int16_t> height_ws, height_mb;     // 256 each: highest y+1 of world surface / motion blocking, kMinY if none
};
void compute_light(const Chunk& c, Light& out, bool sky = true);   // sky = false: Nether and End, no skylight

}  // namespace pn
