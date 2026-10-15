#include "phys.hpp"

namespace pn::sim {

// one box against the moving box: VoxelShape.collideX / collideY / collideZ with the 1.0E-7 slack Mojang uses on the other two axes
static double box_collide(int axis, const AABB& moving, const AABB& b, double movement) {
    int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    if (!(b.min(a1) < moving.max(a1) - 1.0E-7 && b.max(a1) > moving.min(a1) + 1.0E-7 && b.min(a2) < moving.max(a2) - 1.0E-7 && b.max(a2) > moving.min(a2) + 1.0E-7)) return movement;
    if (movement > 0.0) {
        double distance = b.min(axis) - moving.max(axis);
        if (distance >= -1.0E-7) movement = std::min(movement, distance);
    } else if (movement < 0.0) {
        double distance = b.max(axis) - moving.min(axis);
        if (distance <= 1.0E-7) movement = std::max(movement, distance);
    }
    return movement;
}

double shapes_collide(int axis, const AABB& box, const std::vector<AABB>& shapes, double movement) {
    for (const AABB& s : shapes) {
        if (std::fabs(movement) < 1.0E-7) return 0.0;
        movement = box_collide(axis, box, s, movement);
    }
    return movement;
}

// Direction.axisStepOrder
static void axis_step_order(const Vec3& m, int out[3]) {
    out[0] = 1;
    if (std::fabs(m.x) < std::fabs(m.z)) { out[1] = 2; out[2] = 0; } else { out[1] = 0; out[2] = 2; }
}

Vec3 collide_with_shapes(const Vec3& movement, const AABB& box, const std::vector<AABB>& shapes) {
    if (shapes.empty()) return movement;
    Vec3 resolved;
    int order[3];
    axis_step_order(movement, order);
    for (int k = 0; k < 3; k++) {
        int axis = order[k];
        double axis_movement = movement.get(axis);
        if (axis_movement == 0.0) continue;
        double c = shapes_collide(axis, box.move(resolved), shapes, axis_movement);
        resolved = resolved.with(axis, c);
    }
    return resolved;
}

static void collect(const CollisionSource& w, const AABB& area, std::vector<AABB>& out) {
    w.block_collisions(area, out);
    w.entity_collisions(area, out);
}

// Entity.collide
Vec3 entity_collide(const CollisionSource& world, const AABB& aabb, const Vec3& movement, bool on_ground, float max_up_step) {
    std::vector<AABB> colliders;
    collect(world, aabb.expand_towards(movement), colliders);
    Vec3 step = movement.length_sqr() == 0.0 ? movement : collide_with_shapes(movement, aabb, colliders);
    bool x_col = movement.x != step.x, y_col = movement.y != step.y, z_col = movement.z != step.z;
    bool on_ground_after = y_col && movement.y < 0.0;
    if (max_up_step > 0.0f && (on_ground_after || on_ground) && (x_col || z_col)) {
        AABB grounded = on_ground_after ? aabb.move(0.0, step.y, 0.0) : aabb;
        AABB step_up = grounded.expand_towards(movement.x, double(max_up_step), movement.z);
        if (!on_ground_after) step_up = step_up.expand_towards(0.0, -double(1.0E-5f), 0.0);
        std::vector<AABB> around;
        collect(world, step_up, around);
        float skip = float(step.y);
        // collectCandidateStepUpHeights: the y coordinates of all collider faces above the box, within the step height
        std::vector<float> candidates;
        for (const AABB& c : around) {
            for (double coord : {c.min_y, c.max_y}) {
                float rel = float(coord - grounded.min_y);
                if (rel < 0.0f || rel == skip) continue;
                if (rel > max_up_step) continue;
                if (std::find(candidates.begin(), candidates.end(), rel) == candidates.end()) candidates.push_back(rel);
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (float h : candidates) {
            Vec3 from_ground = collide_with_shapes(Vec3(movement.x, double(h), movement.z), grounded, around);
            if (!(from_ground.horizontal_distance_sqr() > step.horizontal_distance_sqr())) continue;
            double to_ground = aabb.min_y - grounded.min_y;
            return from_ground - Vec3(0.0, to_ground, 0.0);
        }
    }
    return step;
}

}  // namespace pn::sim
