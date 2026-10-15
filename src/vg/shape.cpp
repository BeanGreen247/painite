#include "shape.hpp"
#include <algorithm>
#include <cstring>

namespace pn::vg {

namespace {

bool has_class(const BlockData& bd, uint16_t id, const char* cls) {
    const auto& c = bd.classes(id);
    return std::find(c.begin(), c.end(), cls) != c.end();
}

// Block.isExceptionForConnection
bool exception_for_connection(const BlockData& bd, uint16_t s) {
    const std::string& n = bd.name(s);
    return has_class(bd, s, "LeavesBlock") || n == "minecraft:barrier" || n == "minecraft:carved_pumpkin" || n == "minecraft:jack_o_lantern" || n == "minecraft:melon" || n == "minecraft:pumpkin" ||
           bd.in_tag(s, "#minecraft:shulker_boxes");
}

const char* const kHorizontalProp[6] = {nullptr, nullptr, "north", "south", "west", "east"};
int dir_of_name(const std::string& n) {
    static const char* names[6] = {"down", "up", "north", "south", "west", "east"};
    for (int i = 0; i < 6; i++) if (n == names[i]) return i;
    return NORTH;
}
int clockwise(int d) { switch (d) { case NORTH: return EAST; case EAST: return SOUTH; case SOUTH: return WEST; case WEST: return NORTH; default: return d; } }
char axis_of(int d) { return d < 2 ? 'y' : d < 4 ? 'z' : 'x'; }

uint16_t update_shape(const BlockData& bd, uint16_t state, int dir, uint16_t neighbour) {
    const std::string& name = bd.name(state);
    const int opposite = dir ^ 1;
    // fences
    if (name.size() > 6 && name.compare(name.size() - 6, 6, "_fence") == 0) {
        if (dir < 2) return state;
        bool face_sturdy = bd.sturdy(neighbour, opposite);
        bool same_fence = bd.in_tag(neighbour, "#minecraft:fences") && bd.in_tag(neighbour, "#minecraft:wooden_fences") == bd.in_tag(state, "#minecraft:wooden_fences");
        const std::string& nn = bd.name(neighbour);
        bool gate = nn.size() > 11 && nn.compare(nn.size() - 11, 11, "_fence_gate") == 0 && axis_of(dir_of_name(bd.prop(neighbour, "facing"))) == axis_of(clockwise(opposite));
        bool connects = (!exception_for_connection(bd, neighbour) && face_sturdy) || same_fence || gate;
        return bd.with(state, kHorizontalProp[dir], connects ? "true" : "false");
    }
    if (name == "minecraft:iron_bars") {
        if (dir < 2) return state;
        bool face_sturdy = bd.sturdy(neighbour, opposite);
        bool attaches = (!exception_for_connection(bd, neighbour) && face_sturdy) || has_class(bd, neighbour, "IronBarsBlock") || bd.in_tag(neighbour, "#minecraft:walls");
        return bd.with(state, kHorizontalProp[dir], attaches ? "true" : "false");
    }
    if (name == "minecraft:torch") {
        // BaseTorchBlock.updateShape: below is the only neighbour that matters (the support must hold the centre)
        if (dir == DOWN && !bd.sturdy(neighbour, UP)) return 0;
        return state;
    }
    if (name == "minecraft:wall_torch" || name == "minecraft:ladder") {
        int facing = dir_of_name(bd.prop(state, "facing"));
        if (opposite == facing) {
            // canSurvive: the block behind it has a sturdy face towards the torch / ladder (the neighbour in direction `dir` is that block)
            if (!bd.sturdy(neighbour, facing)) return 0;
        }
        return state;
    }
    return state;
}

}  // namespace

uint16_t update_from_neighbour_shapes(const BlockData& bd, const Level& level, const BlockPos& pos, uint16_t state) {
    static const int kOrder[6] = {WEST, EAST, NORTH, SOUTH, DOWN, UP};
    for (int d : kOrder) {
        uint16_t neighbour = level.get(pos.x + kDirX[d], pos.y + kDirY[d], pos.z + kDirZ[d]);
        state = update_shape(bd, state, d, neighbour);
    }
    return state;
}

}  // namespace pn::vg
