// Game state shared by all workers: the player registry, who can see whom, world time, the 20 Hz tick loop,
// commands and autosave. Workers (network threads) call in; events go back out through Worker::post_fn.
#pragma once
#include "auth.hpp"
#include "redstone.hpp"
#include "region.hpp"
#include "sim/entity_manager.hpp"
#include "sim/mob.hpp"
#include "sim/player.hpp"
#include <atomic>
#include <condition_variable>
#include <map>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pn {

class Server;

struct GIdentity {
    uint64_t gid = 0;
    bool bedrock = false;
    int worker = 0;
    uint64_t conn = 0;       // JConn::id or Bedrock session id, valid on that worker
    int eid = 0;
    std::string name, xuid;
    uint8_t uuid[16] = {};
    std::vector<AuthProp> props;  // Java skin properties
    std::vector<uint8_t> bedrock_skin;   // Bedrock "Skin" structure for the player list (empty: default)
    std::string addr;
    mutable std::atomic<int> ping_ms{-1};   // round trip to the client, updated by its connection worker
    int64_t joined_ms = 0;
};

struct GPos {
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, head = 0;
    bool ground = true, sneaking = false, sprinting = false;
    int dim = 0;                 // 0 overworld, 1 nether, 2 end
    uint8_t skin_parts = 0x7f;   // displayed skin layers (Java client information)
};

struct GEvent {
    enum Kind { AddTab, RemoveTab, Spawn, Despawn, Move, Teleport, Rotate, Swing, Block, Time, Gamemode, Kick, Message, Pose, Dimension,
                Health, Death, Hurt, ContainerContent, ContainerSlot, SetCursor, SetPlayerSlot, HeldSlot, ContainerClose, OpenScreen, ContainerData, Sound,
                EntityEvent, EntityEquip, EntityAdd, EntityRemove, EntityMove, EntitySync, EntityMotion, EntityData, EntityTake, Experience } kind = Move;
    std::shared_ptr<const GIdentity> id;   // subject
    GPos pos;
    int16_t dx = 0, dy = 0, dz = 0;
    bool moved = false, turned = false;
    int bx = 0, by = 0, bz = 0;
    uint16_t block = 0;
    int64_t t0 = 0, t1 = 0;                // Time: world age, day time
    int value = 0;
    std::string text;
    float health = 0, saturation = 0;       // Health: value is the food level; Experience: health is the bar progress, value the level, slot the total points
    // container events (the player's own worker encodes them)
    int container = 0, state = 0, slot = 0;
    std::vector<sim::ItemStack> items;
    sim::ItemStack stack;
    int entity_id = 0, other_entity_id = 0;
    uint8_t entity_uuid[16] = {};
    std::string entity_kind;
    sim::Vec3 entity_velocity;
    std::vector<sim::Entity::DataItem> entity_data;
};

class Worker;

class Game {
public:
    explicit Game(Server& s) : s_(s), ticker_(s) {}
    ~Game();
    void start();
    void stop();

    // --- called from worker threads ---
    uint64_t add_player(std::shared_ptr<GIdentity> id, const GPos& pos, int gamemode, int view_chunks);
    void remove_player(uint64_t gid);
    void add_entity(int dim, std::unique_ptr<sim::Entity> entity);
    void update_pos(uint64_t gid, const GPos& pos);
    void set_view(uint64_t gid, int view_chunks);
    void block_changed(int dim, int x, int y, int z, uint16_t id);   // tell everyone who can see that block
    void swing(uint64_t gid);
    void chat(uint64_t gid, const std::string& msg);
    void command(uint64_t gid, const std::string& line);   // gid 0 = console
    void teleport(uint64_t gid, const GPos& pos);          // server-initiated, e.g. /tp (pos.dim picks the dimension)
    void set_gamemode(uint64_t gid, int mode);
    std::shared_ptr<const GIdentity> identity(uint64_t gid);
    std::vector<std::shared_ptr<const GIdentity>> identities();
    std::shared_ptr<const GIdentity> find_name(const std::string& name);
    GPos position(uint64_t gid);
    int gamemode_of(uint64_t gid);
    int op_level(uint64_t gid);
    int players();

    // --- inventory (called from worker threads; the game state lives in sim::PlayerSim) ---
    void inv_click(uint64_t gid, int container, int state, int slot, int button, int mode, const std::map<int, sim::HashedLite>& changed, const sim::HashedLite& carried);
    void inv_close(uint64_t gid, int container);
    bool open_container(uint64_t gid, int dim, int x, int y, int z);          // right click on a chest, furnace ...: open its window
    void spill_container(int dim, int x, int y, int z, uint16_t state);        // a container block is broken: its items drop
    int container_signal(int dim, int x, int y, int z);                        // region threads: how full the container at a spot is, 0..15 (comparators); -1 when it holds nothing
    void play_sound(int dim, double x, double y, double z, const std::string& name, float volume, float pitch);   // players nearby hear it
    void hopper_register(int dim, int x, int y, int z);                        // region threads: this hopper should be ticked
    void queue_dispense(int dim, int x, int y, int z, int delay);              // region threads: a dispenser or dropper fires in `delay` ticks
    bool dispense_fluid(int dim, int x, int y, int z, int pour);               // a dispenser pours (1 water, 2 lava) or picks up (0) a fluid at a spot; true when something happened
    void containers_tick();                                                    // game thread: furnaces burn and smelt
    bool open_workstation(uint64_t gid, int dim, int x, int y, int z);        // right click on an anvil or an enchanting table
    void menu_button(uint64_t gid, int container, int button);                // container_button_click (enchanting offers)
    void anvil_rename(uint64_t gid, const std::string& name);                 // rename_item
    bool open_menu(uint64_t gid, const std::string& kind, const std::string& title);   // crafting table ...: open the window of a block; false when the player cannot

    void inv_set_held(uint64_t gid, int slot);
    void inv_creative_slot(uint64_t gid, int slot, const sim::ItemStack& stack);
    sim::ItemStack inv_held(uint64_t gid);                  // a copy of the stack in the selected hotbar slot
    bool inv_consume_held(uint64_t gid, int n);             // remove n from the selected stack (not in creative); false when it holds fewer
    // a survival player broke a block (any thread): the game thread rolls its loot table, damages the tool and drops the items
    void player_broke_block(uint64_t gid, int dim, int x, int y, int z, uint16_t state);
    // --- health, hunger, damage, death (vitals.cpp) ---
    void damage(uint64_t gid, float amount, const std::string& source);   // source: a death message key such as "fall", "starve", "generic"; any thread
    void kill_player(uint64_t gid);
    void respawn(uint64_t gid);                  // the client pressed "Respawn"
    void start_eating(uint64_t gid);             // use_item with food in the hand
    void stop_eating(uint64_t gid);
    float health_of(uint64_t gid);
    void give_xp(uint64_t gid, int points);       // any thread
    // --- mobs (mobs.cpp) ---
    bool summon(const std::string& type, int dim, double x, double y, double z);   // game thread (commands)
    void attack_entity(uint64_t gid, int entity_id);                               // any thread
    void inv_sync(uint64_t gid);                            // send the whole inventory again (join, dimension change)

    // world clock
    int64_t game_time() const { return game_time_; }
    int64_t day_time() const { return day_time_; }
    void set_day_time(int64_t t) { day_time_ = t; broadcast_time(); }
    double tps() const { return tps_; }
    // duration of the last 256 ticks in microseconds (written by the game thread, read for reports)
    std::atomic<uint32_t> tick_us[256] = {};
    std::atomic<uint32_t> tick_idx{0};
    double mspt() const { return mspt_; }
    bool saving_enabled = true;

    void post_task(std::function<void()> fn);
    RegionTicker& ticker() { return ticker_; }
    Redstone& redstone();
    void leaves_update(int dim, int x, int y, int z);                     // region threads: recompute the distance of a leaf block
    void fire_update(int dim, int x, int y, int z);                       // region threads: a fire block's turn
    void fluid_update(int dim, int x, int y, int z);                      // region threads: water or lava (or a waterlogged block) may flow
    void swap_held_bucket(uint64_t gid, const char* to);                  // the held bucket becomes another item (not in creative)
    bool cauldron_use(uint64_t gid, int dim, int x, int y, int z, const std::string& held);   // a bucket used on a cauldron; true when handled
    void bucket_use(uint64_t gid);                                        // game thread: use_item with a bucket, scoop or pour
    bool set_fire(int dim, int x, int y, int z);                          // light a fire where it can live
    void drop_block_loot(int dim, int x, int y, int z, uint16_t state);   // the items a block gives when it breaks by itself
    int random_tick_speed() const { return random_tick_speed_.load(); }
    void set_random_tick_speed(int n) { random_tick_speed_ = n; }
    void save_everything(bool unload);
    void save_player_data(uint64_t gid);

    // portal.cpp
    bool portal_ignite(int dim, int x, int y, int z);   // light a nether portal whose frame contains this empty block; false when there is no frame
    void portal_tick();                                  // game thread: players standing in portals
    void portal_travel(uint64_t gid, const GPos& from, int kind);
    bool put_ender_eye(int dim, int x, int y, int z);   // an eye of ender into a frame; opens the End portal when the ring is full

private:
    struct Vitals {
        float health = 20.0f, saturation = 5.0f, exhaustion = 0.0f, last_hurt = 0.0f, fall = 0.0f, pending_fall = 0.0f, sent_health = 20.0f, sent_saturation_unused = 0.0f, sent_sat = 5.0f;
        int sent_food = 20;
        int food = 20, food_timer = 0, invulnerable = 0, air = 300, drown_timer = 0, eating = -1;   // eating: ticks left, -1 when not
        bool dead = false;
        int xp_level = 0, xp_total = 0;
        float xp_progress = 0.0f;
        int64_t bow_start = -1;   // game time the bow was drawn
    };
    struct PState {
        Vitals v;
        std::shared_ptr<GIdentity> id;
        GPos pos, sent;
        bool dirty = false, sent_valid = false;
        int gamemode = 0, view = 8;
        std::unordered_set<uint64_t> seen;   // players this one currently has spawned
        int64_t last_save_ms = 0;
        std::unique_ptr<sim::PlayerSim> sim;
        std::unique_ptr<sim::ContainerSynchronizer> sync;
        std::shared_ptr<sim::SimpleContainer> ender;   // the ender chest contents (saved as EnderItems)
    };
    struct EntityTrack {
        sim::Entity* entity = nullptr;
        sim::Vec3 pos, delta;
        float yaw = 0, pitch = 0;
        bool on_ground = false;
        std::vector<sim::Entity::DataItem> data;
        uint64_t data_sig = ~0ull;   // data_signature() of the entity when `data` was built
        std::unordered_set<uint64_t> viewers;
    };
    void loop();
    void tick();
    void send_event(const std::shared_ptr<const GIdentity>& to, GEvent ev);
    void queue_event(const std::shared_ptr<const GIdentity>& to, GEvent ev);   // tick only: batched per worker
    void flush_events();
    void broadcast_time();
    void refresh_entity_world();   // tick, players_ not locked: what mobs can see of the players
    void spawn_tick();
    void save_entities();
    void load_entities();             // tick, players_ locked: natural spawning and despawning
    std::unique_ptr<sim::LivingEntity> make_mob(const std::string& type, double x, double y, double z);
    bool gamemode_survival(const PState& p) const { return p.gamemode == 0 || p.gamemode == 2; }
    void vitals_tick();    // tick, players_ locked: regeneration, hunger, starvation, drowning, void, eating
    void hurt_locked(PState& p, float amount, const std::string& source);
    void die_locked(PState& p, const std::string& source);
    void send_health_locked(PState& p);
    void send_xp_locked(PState& p);
    void add_xp_locked(PState& p, int points);
    void give_levels_locked(PState& p, int levels);   // Player.giveExperienceLevels   // Player.giveExperiencePoints
    void track_movement_locked(PState& p, const GPos& now);
    void add_exhaustion_locked(PState& p, float x) { if (p.gamemode != 1 && p.gamemode != 3) p.v.exhaustion = std::min(40.0f, p.v.exhaustion + x); }
    void pickup_items();
    // blocks that change by themselves (blocktick.cpp)
    void random_ticks();
    void block_random_tick(int dim, int x, int y, int z, uint16_t id);
    void plates_tick();
    void tripwire_tick();
    std::atomic<int> random_tick_speed_{3};
    struct BEC;
    struct HopperRef { int dim, x, y, z; int64_t due; };
    std::mutex hop_mu_;
    std::unordered_map<uint64_t, HopperRef> hoppers_;   // hoppers to tick
    std::vector<HopperRef> dispenses_;                   // dispensers and droppers waiting for their delay (due = fire time)
    bool hopper_move_locked(int dim, int x, int y, int z, std::vector<std::function<void()>>& after);
    void dispense_locked(int dim, int x, int y, int z, std::vector<std::function<void()>>& after);
    std::unordered_map<uint64_t, std::weak_ptr<BEC>> bec_registry_;   // block entity containers somebody holds (windows, furnaces); mu_
    std::unordered_map<uint64_t, std::shared_ptr<BEC>> bec_active_;   // furnaces that have work to do
    std::shared_ptr<BEC> bec_get_locked(int dim, int x, int y, int z);
    void bec_save_locked(BEC& b);
    std::mutex fluid_mu_;
    std::unordered_map<uint64_t, int64_t> fluid_next_;   // fluid blocks waiting for their tick delay: position key -> game time
    std::mutex fire_mu_;
    std::unordered_map<uint64_t, int64_t> fire_next_;   // fire blocks and the game time of their next tick   // tick, players_ locked: players collect the item entities they touch
    void spawn_for(PState& viewer, PState& p);
    void despawn_for(PState& viewer, PState& p);
    void run_command(uint64_t gid, const std::string& line);   // commands.cpp
    void announce(const std::string& text);

    Server& s_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::unordered_map<uint64_t, PState> players_;
    std::unique_ptr<sim::EntityManager> entity_managers_[3];
    std::unordered_map<int, EntityTrack> entity_tracks_;
    std::unordered_map<int, std::vector<std::pair<uint64_t, GEvent>>> batch_;   // worker id -> events of the running tick
    uint64_t next_gid_ = 1;
    std::mutex task_mu_;
    std::condition_variable task_cv_;
    std::vector<std::function<void()>> tasks_;
    RegionTicker ticker_;
    std::unique_ptr<Redstone> redstone_;
    std::atomic<int64_t> game_time_{0}, day_time_{0};
    double tps_ = 20, mspt_ = 0;
    int64_t last_autosave_ = 0;
    bool find_portal(int dim, int x, int z, int radius, int& ox, int& oy, int& oz);
    void build_portal(int dim, int x, int y, int z);
    void portal_site(int dim, int x, int z, int& oy, int& ox, int& oz);
    struct PortalState { int ticks = 0, cooldown = 0; };
    std::unordered_map<uint64_t, PortalState> portal_state_;
    int exec_dim_ = -1;   // set while running /execute in <dimension> run ...
    friend struct Commands;
public:
    // commands.cpp
    void exec(uint64_t gid, const std::string& line, const std::function<void(const std::string&)>& reply);
};

}  // namespace pn
