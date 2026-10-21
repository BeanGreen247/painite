// Ground pathfinding for mobs: A* over block cells where the mob can stand, with one block steps up and short drops.
#pragma once
#include "entity.hpp"
#include <vector>

namespace pn::sim {

// Waypoints (cell centres, feet height) from `from` towards `to`. When the target cannot be reached the path leads to the reachable cell closest to it.
// Empty when there is nowhere to go.
std::vector<Vec3> find_path(EntityWorld& world, const Vec3& from, const Vec3& to, double width, double height, int max_nodes = 300, int max_fall = 3);

}  // namespace pn::sim
