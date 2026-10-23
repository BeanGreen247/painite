// Checks of the gameplay core against what Mojang's code does (hand-worked cases), run without a server: painite_simtest
#include "../../src/sim/inventory.hpp"
#include "../../src/sim/entity_manager.hpp"
#include "../../src/blocks.hpp"
#include "../../src/sim/path.hpp"
#include "../../src/sim/mob.hpp"
#include <cstdio>
#include <functional>
#include <cstdlib>

using namespace pn::sim;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct TestPlayer : MenuPlayer {
    PlayerInventory inv{this};
    bool creative = false;
    std::vector<ItemStack> dropped;
    void drop(const ItemStack& s, bool) override { dropped.push_back(s); }
    bool has_infinite_materials() const override { return creative; }
    PlayerInventory& inventory() override { return inv; }
};


// a small column world for entity tests: stone at y <= floor_y, everything else air
struct ColumnWorld : EntityWorld {
    int floor_y = 0;
    std::vector<std::pair<int, uint16_t>> placed;   // set_block_state calls (y, state)
    pn::vg::LegacyRandom rnd{1};
    uint16_t stone = pn::Blocks::get().default_state("minecraft:stone");
    int64_t game_time() const override { return 0; }
    pn::vg::Rng& random() override { return rnd; }
    uint16_t block_state(int x, int y, int z) const override { return y <= floor_y ? stone : 0; }
    bool set_block_state(int x, int y, int z, uint16_t st) override { placed.emplace_back(y, st); return true; }
    int min_build_y() const override { return -64; }
    void block_collisions(const AABB& a, std::vector<AABB>& out) const override {
        for (int y = int(std::floor(a.min_y)); y <= int(std::ceil(a.max_y)) - 1; y++)
            if (y <= floor_y) out.push_back(AABB(a.min_x - 1, y, a.min_z - 1, a.max_x + 1, y + 1, a.max_z + 1));
    }
    void entities_in_box(const AABB&, const Entity*, std::vector<Entity*>&) override {}
    Entity* add_entity(std::unique_ptr<Entity> e) override { spawned.push_back(std::move(e)); return spawned.back().get(); }
    std::vector<std::unique_ptr<Entity>> spawned;
};

// a world from a rule: solid(x, y, z)
struct GridWorld : EntityWorld {
    std::function<bool(int, int, int)> solid;
    pn::vg::LegacyRandom rnd{1};
    int64_t game_time() const override { return 0; }
    pn::vg::Rng& random() override { return rnd; }
    void block_collisions(const AABB& a, std::vector<AABB>& out) const override {
        for (int x = int(std::floor(a.min_x)); x <= int(std::ceil(a.max_x)) - 1; x++)
            for (int y = int(std::floor(a.min_y)); y <= int(std::ceil(a.max_y)) - 1; y++)
                for (int z = int(std::floor(a.min_z)); z <= int(std::ceil(a.max_z)) - 1; z++)
                    if (solid(x, y, z)) out.push_back(AABB(x, y, z, x + 1, y + 1, z + 1));
    }
    void entities_in_box(const AABB&, const Entity*, std::vector<Entity*>&) override {}
    Entity* add_entity(std::unique_ptr<Entity>) override { return nullptr; }
};

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "data/game/26.2";
    std::string err;
    if (!Items::load(dir, err)) { printf("cannot load items: %s\n", err.c_str()); return 2; }
    size_t cut = dir.rfind('/');
    std::string data_root = cut == std::string::npos ? "data" : dir.substr(0, cut);
    cut = data_root.rfind('/');
    data_root = cut == std::string::npos ? "data" : data_root.substr(0, cut);
    if (!pn::Blocks::get().load({{777, data_root + "/vanilla-26.3.blocks"}, {776, data_root + "/vanilla-26.2.blocks"}}, err)) {
        printf("cannot load block states: %s\n", err.c_str());
        return 2;
    }
    if (!BlockCollisionShapes::get().load(dir + "/blockshapes_states.txt", dir + "/blockshapes.txt", err)) {
        printf("cannot load block collision shapes: %s\n", err.c_str());
        return 2;
    }
    uint16_t stone_state = pn::Blocks::get().default_state("minecraft:stone");
    uint16_t slab_state = pn::Blocks::get().find("minecraft:oak_slab", "type=top,waterlogged=false");
    CHECK(BlockCollisionShapes::get().is_full_block(stone_state));
    CHECK(!BlockCollisionShapes::get().is_full_block(slab_state));
    const auto& slab_boxes = BlockCollisionShapes::get().boxes(slab_state);
    CHECK(slab_boxes.size() == 1 && slab_boxes[0].min_y == 0.5 && slab_boxes[0].max_y == 1.0);
    const Items& I = Items::get();
    const ItemType* stone = I.by_name("minecraft:stone");
    const ItemType* dirt = I.by_name("minecraft:dirt");
    const ItemType* pick = I.by_name("minecraft:diamond_pickaxe");
    CHECK(stone && dirt && pick);
    CHECK(pick->max_stack_size == 1 && pick->max_damage == 1561 && pick->has_tool);

    TestPlayer p;
    InventoryMenu menu(&p.inv, &p);
    CHECK(menu.slots.size() == 46);
    // pick up 10 stone from hotbar slot 0 (menu slot 36), put 4 into slot 37 with a right click... and drop outside
    p.inv.items[0] = ItemStack(stone, 10);
    menu.clicked(36, 0, ContainerInput::PICKUP, p);
    CHECK(menu.carried().count() == 10 && p.inv.items[0].is_empty());
    menu.clicked(37, 1, ContainerInput::PICKUP, p);    // right click: place one
    CHECK(menu.carried().count() == 9 && p.inv.items[1].count() == 1);
    menu.clicked(37, 0, ContainerInput::PICKUP, p);    // left click on the same item: merge everything
    CHECK(menu.carried().is_empty() && p.inv.items[1].count() == 10);
    // shift click moves the hotbar stack to the main inventory (slots 9..35)
    menu.clicked(37, 0, ContainerInput::QUICK_MOVE, p);
    CHECK(p.inv.items[1].is_empty() && p.inv.items[9].count() == 10);
    // swap with hotbar key 3
    menu.clicked(9, 3, ContainerInput::SWAP, p);
    CHECK(p.inv.items[9].is_empty() && p.inv.items[3].count() == 10);
    // drag evenly over 3 slots: 10 stone -> 3,3,3 and 1 left
    menu.clicked(39, 0, ContainerInput::PICKUP, p);
    CHECK(menu.carried().count() == 10);
    menu.clicked(-999, 0 | (0 << 2), ContainerInput::QUICK_CRAFT, p);   // start, charitable
    menu.clicked(10, 1, ContainerInput::QUICK_CRAFT, p);
    menu.clicked(11, 1, ContainerInput::QUICK_CRAFT, p);
    menu.clicked(12, 1, ContainerInput::QUICK_CRAFT, p);
    menu.clicked(-999, 2, ContainerInput::QUICK_CRAFT, p);               // end
    CHECK(p.inv.items[10].count() == 3 && p.inv.items[11].count() == 3 && p.inv.items[12].count() == 3 && menu.carried().count() == 1);
    // armor slot refuses stone, accepts nothing but a helmet
    const ItemType* helm = I.by_name("minecraft:diamond_helmet");
    menu.set_carried(ItemStack(stone, 1));
    menu.clicked(5, 0, ContainerInput::PICKUP, p);
    CHECK(p.inv.equipment_at(EquipmentSlot::HEAD).is_empty());
    menu.set_carried(ItemStack(helm, 1));
    menu.clicked(5, 0, ContainerInput::PICKUP, p);
    CHECK(p.inv.equipment_at(EquipmentSlot::HEAD).is(helm));
    // closing returns the cursor to the inventory
    menu.set_carried(ItemStack(dirt, 5));
    menu.removed(p);
    CHECK(menu.carried().is_empty());
    // tool damage is kept in the component patch
    ItemStack t(pick, 1);
    t.set_damage_value(100);
    CHECK(t.damage_value() == 100 && t.is_damaged());
    std::vector<uint8_t> w;
    t.write(w);
    CHECK(!w.empty());
    {   // a falling sand block drops from y 10 onto stone at y 0 and is placed on y 1 after about 12 ticks (0.04 gravity, 0.98 drag)
        ColumnWorld cw;
        uint16_t sand = pn::Blocks::get().default_state("minecraft:sand");
        FallingBlockEntity fb(&cw, 0.5, 10, 0.5, sand);
        int ticks = 0;
        while (!fb.removed && ticks < 200) { fb.tick(); ticks++; }
        CHECK(fb.removed);
        CHECK(cw.placed.size() == 1 && cw.placed[0].first == 1 && cw.placed[0].second == sand);
        CHECK(ticks > 10 && ticks < 30);
        // above water-free air with nothing below it never lands before the build limit rule: it is dropped as an item after 600 ticks at the latest
        ColumnWorld deep;
        deep.floor_y = -1000;
        FallingBlockEntity fall(&deep, 0.5, 10, 0.5, sand);
        for (int i = 0; i < 700 && !fall.removed; i++) fall.tick();
        CHECK(fall.removed && deep.placed.empty());
    }
    {   // pathfinding: a wall at x = 3 for z in -3..3 with the way round at z = 4; a one block step up is taken, a two block wall is not
        GridWorld gw;
        gw.solid = [](int x, int y, int z) { return y <= 0 || (x == 3 && z >= -3 && z <= 3 && y <= 3); };
        auto path = find_path(gw, Vec3(0.5, 1, 0.5), Vec3(6.5, 1, 0.5), 0.6, 1.8);
        CHECK(!path.empty());
        bool through = false, reached = false;
        for (auto& p : path) { if (int(std::floor(p.x)) == 3 && p.z < 4.0) through = true; if (std::fabs(p.x - 6.5) < 0.6 && std::fabs(p.z - 0.5) < 0.6) reached = true; }
        CHECK(!through);
        CHECK(reached);
        GridWorld steps;
        steps.solid = [](int x, int y, int z) { return y <= 0 || (x >= 2 && y <= 1) || (x >= 4 && y <= 2); };
        auto up = find_path(steps, Vec3(0.5, 1, 0.5), Vec3(5.5, 3, 0.5), 0.6, 1.8);
        CHECK(!up.empty() && std::fabs(up.back().x - 5.5) < 0.6 && up.back().y == 3);
        GridWorld cliff;
        cliff.solid = [](int x, int y, int z) { return y <= 0 && x < 4; };   // the floor ends: the target is across a pit
        auto none = find_path(cliff, Vec3(0.5, 1, 0.5), Vec3(8.5, 1, 0.5), 0.6, 1.8);
        CHECK(none.empty() || none.back().x < 4.0);   // it walks to the edge, not into the void
    }
    {   // a cow with its head under water runs out of air after 300 ticks and then takes damage; a zombie-like mob on land keeps its air
        struct Pool : ColumnWorld {
            double fluid_height(const AABB& box, bool lava) const override { return lava ? 0.0 : std::max(0.0, 5.0 - box.min_y); }
        };
        Pool pool;
        pool.floor_y = 0;
        Mob cow("minecraft:cow", &pool);
        cow.pos = Vec3(0.5, 1.0, 0.5);
        for (int i = 0; i < 250; i++) cow.tick();
        CHECK(cow.air_supply < 100 && cow.health == 20.0f);
        for (int i = 0; i < 100; i++) cow.tick();
        CHECK(cow.health < 20.0f);
        Mob fish("minecraft:cod", &pool);
        fish.pos = Vec3(0.5, 1.0, 0.5);
        for (int i = 0; i < 400; i++) fish.tick();
        CHECK(fish.air_supply == 300 && fish.health == 20.0f);
    }
    {   // flowing water pushes along its slope: a row of water getting shallower towards +x pushes +x; still water pushes nothing
        auto amount = [](int x, int y, int z) { return y == 1 && z == 0 && x >= 0 && x <= 6 ? 8 - x : 0; };
        auto none = [](int, int, int) { return false; };
        auto floor = [](int x, int y, int z) { return y <= 0 || z != 0; };
        Vec3 f = compute_fluid_flow(AABB(2.1, 1.0, 0.1, 2.7, 1.5, 0.7), amount, none, floor);
        CHECK(f.x > 0.9 && std::fabs(f.z) < 0.1);
        auto flat = [](int x, int y, int z) { return y == 1 && z == 0 && x >= 0 && x <= 6 ? 8 : 0; };
        Vec3 g = compute_fluid_flow(AABB(2.1, 1.0, 0.1, 2.7, 1.5, 0.7), flat, none, floor);
        CHECK(g.length_sqr() == 0.0);
    }
    printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
