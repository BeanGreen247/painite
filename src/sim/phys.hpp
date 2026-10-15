// Vectors, boxes and the collision rules of Mojang's Entity.move (Entity.collide, collideWithShapes, Shapes.collide).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace pn::sim {

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 scale(double f) const { return {x * f, y * f, z * f}; }
    Vec3 multiply(double a, double b, double c) const { return {x * a, y * b, z * c}; }
    double length_sqr() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(length_sqr()); }
    double horizontal_distance_sqr() const { return x * x + z * z; }
    double horizontal_distance() const { return std::sqrt(horizontal_distance_sqr()); }
    double get(int axis) const { return axis == 0 ? x : axis == 1 ? y : z; }
    Vec3 with(int axis, double v) const { Vec3 r = *this; (axis == 0 ? r.x : axis == 1 ? r.y : r.z) = v; return r; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    Vec3 normalize() const { double d = length(); return d < 1.0E-5 ? Vec3() : Vec3(x / d, y / d, z / d); }
};

struct AABB {
    double min_x = 0, min_y = 0, min_z = 0, max_x = 0, max_y = 0, max_z = 0;
    AABB() = default;
    AABB(double x0, double y0, double z0, double x1, double y1, double z1) : min_x(std::min(x0, x1)), min_y(std::min(y0, y1)), min_z(std::min(z0, z1)), max_x(std::max(x0, x1)), max_y(std::max(y0, y1)), max_z(std::max(z0, z1)) {}
    double min(int a) const { return a == 0 ? min_x : a == 1 ? min_y : min_z; }
    double max(int a) const { return a == 0 ? max_x : a == 1 ? max_y : max_z; }
    AABB move(double dx, double dy, double dz) const { return AABB(min_x + dx, min_y + dy, min_z + dz, max_x + dx, max_y + dy, max_z + dz); }
    AABB move(const Vec3& v) const { return move(v.x, v.y, v.z); }
    AABB expand_towards(double dx, double dy, double dz) const {
        double a = min_x, b = min_y, c = min_z, d = max_x, e = max_y, f = max_z;
        if (dx < 0) a += dx; else if (dx > 0) d += dx;
        if (dy < 0) b += dy; else if (dy > 0) e += dy;
        if (dz < 0) c += dz; else if (dz > 0) f += dz;
        return AABB(a, b, c, d, e, f);
    }
    AABB expand_towards(const Vec3& v) const { return expand_towards(v.x, v.y, v.z); }
    AABB inflate(double x, double y, double z) const { return AABB(min_x - x, min_y - y, min_z - z, max_x + x, max_y + y, max_z + z); }
    AABB inflate(double v) const { return inflate(v, v, v); }
    AABB deflate(double v) const { return inflate(-v); }
    AABB set_min_y(double v) const { AABB r = *this; r.min_y = v; return r; }
    AABB set_max_y(double v) const { AABB r = *this; r.max_y = v; return r; }
    bool intersects(const AABB& o) const { return min_x < o.max_x && max_x > o.min_x && min_y < o.max_y && max_y > o.min_y && min_z < o.max_z && max_z > o.min_z; }
    bool contains(double x, double y, double z) const { return x >= min_x && x < max_x && y >= min_y && y < max_y && z >= min_z && z < max_z; }
    Vec3 center() const { return {(min_x + max_x) * 0.5, (min_y + max_y) * 0.5, (min_z + max_z) * 0.5}; }
};

// The solid boxes of the blocks around a box, in world coordinates (what Level.getBlockCollisions yields, flattened into boxes)
class CollisionSource {
public:
    virtual ~CollisionSource() = default;
    virtual void block_collisions(const AABB& area, std::vector<AABB>& out) const = 0;
    virtual void entity_collisions(const AABB& area, std::vector<AABB>& out) const { (void)area; (void)out; }   // boats, shulkers
};

// Shapes.collide: how far `box` may move along `axis` (0 x, 1 y, 2 z) before it hits one of the shapes
double shapes_collide(int axis, const AABB& box, const std::vector<AABB>& shapes, double movement);
// Entity.collideWithShapes
Vec3 collide_with_shapes(const Vec3& movement, const AABB& box, const std::vector<AABB>& shapes);
// Entity.collideBoundingBox plus the step up part of Entity.collide
Vec3 entity_collide(const CollisionSource& world, const AABB& box, const Vec3& movement, bool on_ground, float max_up_step);

}  // namespace pn::sim
