// Redstone: power sources, dust, torches, repeaters and the things power operates (lamps, doors, trapdoors, gates, pistons, TNT).
// Dust networks are solved all at once when something next to them changes; torches, repeaters, lamps and buttons keep the delays of the game.
// Everything is driven by RegionTicker::process_block (a neighbour changed or a scheduled update is due), so it runs on the region threads.
#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pn {

class Server;
struct BlockInfo;

class Redstone {
public:
    explicit Redstone(Server& s) : s_(s) {}
    void update(int dim, int x, int y, int z);                        // a neighbour changed, or a scheduled update of this block is due
    bool use(int dim, int x, int y, int z);                           // right click: levers, buttons, doors, trapdoors, gates; true when the click was used
    struct PlateHit { int dim, x, y, z; bool living; };
    void plates(const std::vector<PlateHit>& hits, int64_t tick);     // game thread: the plates entities stand on right now
    bool ignite_tnt(int dim, int x, int y, int z);                    // flint and steel on TNT
    static bool is_interactive(uint16_t block);
    void target_hit(int dim, int x, int y, int z, double rx, double ry, double rz, int ticks);   // an arrow hit a target block; (rx, ry, rz) is the hit inside the block

private:
    struct Pos { int x, y, z; };
    struct Ctx;
    void update_wire(Ctx& c, int x, int y, int z);
    void update_torch(Ctx& c, int x, int y, int z, uint16_t id);
    void update_repeater(Ctx& c, int x, int y, int z, uint16_t id);
    void update_lamp(Ctx& c, int x, int y, int z, uint16_t id);
    void update_openable(Ctx& c, int x, int y, int z, uint16_t id);
    void update_piston(Ctx& c, int x, int y, int z, uint16_t id);
    void update_button(Ctx& c, int x, int y, int z, uint16_t id);
    void update_observer(Ctx& c, int x, int y, int z, uint16_t id);
    void update_comparator(Ctx& c, int x, int y, int z, uint16_t id);
    void update_daylight(Ctx& c, int x, int y, int z, uint16_t id);
    void update_target(Ctx& c, int x, int y, int z, uint16_t id);
    void update_note(Ctx& c, int x, int y, int z, uint16_t id);
    void play_note(Ctx& c, int x, int y, int z, uint16_t id);
    void schedule_daylight(Ctx& c, int x, int y, int z);
    int analog_signal(Ctx& c, int x, int y, int z);
    int comparator_signal(Ctx& c, int x, int y, int z, uint16_t id);
    bool push_line(Ctx& c, int px, int py, int pz, int sx, int sy, int sz, int move, std::vector<Pos>& moved, std::vector<Pos>& destroyed);
    void extend_piston(Ctx& c, int x, int y, int z, uint16_t id);
    void retract_piston(Ctx& c, int x, int y, int z, uint16_t id);
    Server& s_;
    std::mutex mu_;
    std::unordered_map<uint64_t, uint16_t> observed_;   // observers: the block they saw in front last time
    std::unordered_map<uint64_t, int> comp_out_;
    std::unordered_map<uint64_t, std::vector<int64_t>> burnout_;   // redstone torches: when they turned off lately        // comparators: their output signal
    std::unordered_map<uint64_t, int64_t> pending_;   // blocks waiting for their delay: key of the position -> game time it is due
    struct PlateState { int dim, x, y, z; int64_t until; };
    std::unordered_map<uint64_t, PlateState> plates_;  // pressed plates and the game time until which they stay pressed
};

}  // namespace pn
