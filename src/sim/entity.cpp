#include "entity.hpp"
#include "../blocks.hpp"
#include <cstring>
#include <chrono>

namespace pn::sim {

static int64_t unique_seed() {
    static int64_t s = 8682522807148012LL;
    s *= 1181783497276652981LL;
    return s ^ int64_t(std::chrono::steady_clock::now().time_since_epoch().count());
}

Entity::Entity(const std::string& t, EntityWorld* w) : type(t), level(w), rng(std::make_unique<vg::LegacyRandom>(unique_seed())) {}

void Entity::collect_data(std::vector<DataItem>&) const {}

void Entity::move_self(Vec3 d) {
    if (no_physics) {
        pos = pos + d;
        horizontal_collision = vertical_collision = vertical_collision_below = false;
        return;
    }
    Vec3 movement = entity_collide(*level, bounding_box(), d, on_ground, step_height);
    double movement_length = movement.length_sqr();
    if (movement_length > 1.0E-7 || d.length_sqr() - movement_length < 1.0E-7) pos = pos + movement;
    auto equal = [](double a, double b) { return std::fabs(b - a) < 9.999999747378752E-6; };   // Mth.equal(double, double)
    bool x_col = !equal(d.x, movement.x), z_col = !equal(d.z, movement.z);
    horizontal_collision = x_col || z_col;
    bool moved_vertically = std::fabs(d.y) > 0.0;
    if (moved_vertically) {
        vertical_collision = d.y != movement.y;
        vertical_collision_below = vertical_collision && d.y < 0.0;
        on_ground = vertical_collision_below;   // setOnGroundWithMovement
    }
    // restituteMovementAfterCollisions
    if (moved_vertically && vertical_collision || horizontal_collision) {
        double restitution = entity_bounciness();
        Vec3 current = delta, after = current;
        if (x_col) after = after.with(0, -current.x * restitution);
        if (z_col) after = after.with(2, -current.z * restitution);
        if (vertical_collision) {
            double drag = 1.0, comp = 0.0;
            if (vertical_collision_below) restitution = -current.y < gravity() ? 0.0 : restitution;   // block bounciness (slime) is not applied yet
            if (restitution > 0.0) {
                double portion = movement.y / current.y;
                comp = portion * gravity();
                drag = 1.0 + (double(air_drag()) - 1.0) * portion;   // Mth.lerp(portion, 1.0, airDrag)
            }
            after = after.with(1, (comp - current.y) * drag * restitution);
        }
        delta = after;
    }
}

void Entity::move_towards_closest_space(double x, double y, double z) {
    int bx = int(std::floor(x)), by = int(std::floor(y)), bz = int(std::floor(z));
    double d[3] = {x - bx, y - by, z - bz};
    struct Dir { int ax; int step; };
    static const Dir dirs[5] = {{2, -1}, {2, 1}, {0, -1}, {0, 1}, {1, 1}};   // north, south, west, east, up
    Dir closest = {1, 1};
    double closest_d = 1.7976931348623157e308;
    for (const Dir& dir : dirs) {
        int nx = bx + (dir.ax == 0 ? dir.step : 0), ny = by + (dir.ax == 1 ? dir.step : 0), nz = bz + (dir.ax == 2 ? dir.step : 0);
        if (level->is_full_block(nx, ny, nz)) continue;
        double oriented = dir.step > 0 ? 1.0 - d[dir.ax] : d[dir.ax];
        if (!(oriented < closest_d)) continue;
        closest_d = oriented;
        closest = dir;
    }
    float speed = random().next_float() * 0.2f + 0.1f;
    float step = float(closest.step);
    Vec3 scaled = delta.scale(0.75);
    if (closest.ax == 0) delta = Vec3(double(step * speed), scaled.y, scaled.z);
    else if (closest.ax == 1) delta = Vec3(scaled.x, double(step * speed), scaled.z);
    else delta = Vec3(scaled.x, scaled.y, double(step * speed));
}

ItemEntity::ItemEntity(EntityWorld* w, double x, double y, double z, const ItemStack& stack) : Entity("minecraft:item", w), item(stack) {
    width = 0.25; height = 0.25; eye_height = 0.2125;
    set_pos(x, y, z);
    yaw = rng->next_float() * 360.0f;
    bob_offs = rng->next_float() * float(M_PI) * 2.0f;
    delta = Vec3(rng->next_double() * 0.2 - 0.1, 0.2, rng->next_double() * 0.2 - 0.1);
}

bool ItemEntity::are_mergable(const ItemStack& a, const ItemStack& b) {
    if (b.count() + a.count() > b.max_stack_size()) return false;
    return ItemStack::is_same_item_same_components(a, b);
}

void ItemEntity::merge(ItemEntity* to, ItemStack& to_stack, ItemEntity* from, ItemStack& from_stack) {
    int delta = std::min(std::min(to_stack.max_stack_size(), 64) - to_stack.count(), from_stack.count());
    to->item = to_stack.copy_with_count(to_stack.count() + delta);
    from_stack.shrink(delta);
    to->pickup_delay = std::max(to->pickup_delay, from->pickup_delay);
    to->age = std::min(to->age, from->age);
    if (from_stack.is_empty()) from->discard();
}

void ItemEntity::try_to_merge(ItemEntity* other) {
    ItemStack& this_stack = item;
    ItemStack& other_stack = other->item;
    if (has_target != other->has_target || (has_target && memcmp(target, other->target, 16) != 0) || !are_mergable(this_stack, other_stack)) return;
    if (other_stack.count() < this_stack.count()) merge(this, this_stack, other, other_stack);
    else merge(other, other_stack, this, this_stack);
}

void ItemEntity::merge_with_neighbours() {
    if (!mergable()) return;
    std::vector<Entity*> near;
    level->entities_in_box(bounding_box().inflate(0.5, 0.0, 0.5), this, near);
    for (Entity* e : near) {
        auto* other = dynamic_cast<ItemEntity*>(e);
        if (!other || !other->mergable()) continue;
        try_to_merge(other);
        if (removed) break;
    }
}

// FlowingFluid.getFlow summed over the fluid blocks the box touches (each weighted by how deep the box is in it), then normalized.
// amount(x, y, z) is the 0..8 fluid in a block, falling(x, y, z) tells falling fluid, solid(x, y, z) a block that stops fluid.
Vec3 compute_fluid_flow(const AABB& box, const std::function<int(int, int, int)>& amount, const std::function<bool(int, int, int)>& falling,
                        const std::function<bool(int, int, int)>& solid) {
    AABB b = box.deflate(0.001);
    int x0 = int(std::floor(b.min_x)), x1 = int(std::ceil(b.max_x)), y0 = int(std::floor(b.min_y)), y1 = int(std::ceil(b.max_y)), z0 = int(std::floor(b.min_z)), z1 = int(std::ceil(b.max_z));
    static const int DX[4] = {0, 0, -1, 1}, DZ[4] = {-1, 1, 0, 0};
    Vec3 total;
    int count = 0;
    for (int y = y0; y < y1; ++y)
        for (int z = z0; z < z1; ++z)
            for (int x = x0; x < x1; ++x) {
                int a = amount(x, y, z);
                if (a == 0) continue;
                double h = a / 9.0, d = y + (amount(x, y + 1, z) > 0 ? 1.0 : h) - box.min_y;
                if (d < 0.001) continue;
                Vec3 flow;
                for (int k = 0; k < 4; k++) {
                    int nx = x + DX[k], nz = z + DZ[k];
                    int na = amount(nx, y, nz);
                    double f = 0.0;
                    if (na == 0) {
                        if (!solid(nx, y, nz)) {
                            int ba = amount(nx, y - 1, nz);
                            if (ba > 0) f = h - (ba / 9.0 - 0.8888889);
                        }
                    } else f = h - na / 9.0;
                    if (f != 0.0) flow = flow + Vec3(DX[k] * f, 0, DZ[k] * f);
                }
                if (falling(x, y, z)) {   // falling fluid pulled down against walls
                    for (int k = 0; k < 4; k++)
                        if (solid(x + DX[k], y, z + DZ[k]) || solid(x + DX[k], y + 1, z + DZ[k])) { flow = flow.normalize() + Vec3(0, -6.0, 0); break; }
                }
                flow = flow.normalize();
                if (d < 0.4) flow = flow.scale(d);
                total = total + flow;
                count++;
            }
    if (count == 0 || total.length_sqr() == 0.0) return Vec3();
    return total.scale(1.0 / count).normalize();
}

void Entity::fluid_push() {
    {   // BubbleColumnBlock: soul sand lifts, magma drags down; the air just above a column launches harder
        const Blocks& B = Blocks::get();
        int bx = int(std::floor(pos.x)), by = int(std::floor(pos.y)), bz = int(std::floor(pos.z));
        const BlockInfo& here = B.info(level->block_state(bx, by, bz));
        bool inside = here.name == "minecraft:bubble_column";
        const BlockInfo& under = B.info(level->block_state(bx, by - 1, bz));
        bool above = !inside && here.air && under.name == "minecraft:bubble_column";
        if (inside || above) {
            bool drag = (inside ? here : under).props.find("drag=true") != std::string::npos;
            if (above) delta = delta.with(1, drag ? std::max(-0.9, delta.y - 0.03) : std::min(1.8, delta.y + 0.1));
            else delta = delta.with(1, drag ? std::max(-0.3, delta.y - 0.03) : std::min(0.7, delta.y + 0.06));
        }
    }
    Vec3 flow = level->fluid_flow(bounding_box(), false);
    if (flow.length_sqr() == 0.0) return;
    if (std::fabs(delta.x) < 0.003 && std::fabs(delta.z) < 0.003 && flow.length() < 0.0045) flow = flow.normalize().scale(0.0045);
    delta = delta + flow.scale(0.014);
}

void ItemEntity::tick() {
    if (item.is_empty()) { discard(); return; }
    Entity::tick();
    if (pickup_delay > 0 && pickup_delay != 32767) --pickup_delay;
    double xo = pos.x, yo = pos.y, zo = pos.z;
    Vec3 old = delta;
    AABB box = bounding_box();
    if (level->fluid_height(box, false) > double(0.1f)) {   // setUnderwaterMovement
        fluid_push();
        delta = Vec3(delta.x * 0.99f, delta.y + (delta.y < double(0.06f) ? double(5.0E-4f) : 0.0), delta.z * 0.99f);
    } else if (level->fluid_height(box, true) > double(0.1f)) {
        delta = Vec3(delta.x * 0.95f, delta.y + (delta.y < double(0.06f) ? double(5.0E-4f) : 0.0), delta.z * 0.95f);
    } else apply_gravity();
    no_physics = !level->no_collision(bounding_box().deflate(1.0E-7));
    if (no_physics) {
        move_towards_closest_space(pos.x, (bounding_box().min_y + bounding_box().max_y) / 2.0, pos.z);
    }
    if (!on_ground || delta.horizontal_distance_sqr() > double(1.0E-5f) || (tick_count + id) % 4 == 0) {
        move_self(delta);
        float air = air_drag();
        float ground_friction = air;
        if (on_ground) ground_friction *= level->block_friction(int(std::floor(pos.x)), int(std::floor(pos.y - 0.999999f)), int(std::floor(pos.z)));
        delta = delta.multiply(ground_friction, air, ground_friction);
        if (on_ground && delta.y < 0.0) delta = delta.multiply(1.0, -0.5, 1.0);
    }
    bool moved = int(std::floor(xo)) != block_x() || int(std::floor(yo)) != block_y() || int(std::floor(zo)) != block_z();
    int rate = moved ? 2 : 40;
    if (tick_count % rate == 0 && mergable()) merge_with_neighbours();
    if (age != -32768) ++age;
    if ((delta - old).length_sqr() > 0.01) needs_sync = true;
    if (age >= 6000) discard();
}

void ItemEntity::collect_data(std::vector<DataItem>& out) const {
    DataItem d;
    d.index = 8;
    d.serializer = 7;   // ITEM_STACK
    item.write(d.value);
    out.push_back(std::move(d));
}

FallingBlockEntity::FallingBlockEntity(EntityWorld* w, double x, double y, double z, uint16_t st) : Entity("minecraft:falling_block", w), state(st) {
    width = 0.98; height = 0.98; eye_height = 0.833;
    set_pos(x, y + (1.0 - height) / 2.0, z);   // FallingBlockEntity.fall
    delta = Vec3(0, 0, 0);
}

void FallingBlockEntity::drop_as_item() {
    const BlockInfo& bi = Blocks::get().info(state);
    if (const ItemType* t = Items::get().by_name(bi.name)) level->add_entity(std::make_unique<ItemEntity>(level, pos.x, pos.y, pos.z, ItemStack(t, 1)));
}

void FallingBlockEntity::tick() {
    const Blocks& B = Blocks::get();
    if (B.info(state).air) { discard(); return; }
    Entity::tick();
    time++;
    apply_gravity();
    move_self(delta);
    if (removed) return;
    int bx = block_x(), by = block_y(), bz = block_z();
    if (!on_ground) {
        if ((time > 100 && (by <= level->min_build_y() || by > level->max_build_y())) || time > 600) { drop_as_item(); discard(); return; }
    } else {
        delta = delta.multiply(0.7, -0.5, 0.7);
        const BlockInfo& here = B.info(level->block_state(bx, by, bz));
        const BlockInfo& below = B.info(level->block_state(bx, by - 1, bz));
        bool free_below = below.air || below.fluid || below.replaceable;   // FallingBlock.isFree: keep falling through water and plants
        if (free_below && by > level->min_build_y()) { on_ground = false; }
        else if (here.air || here.fluid || here.replaceable) {
            level->set_block_state(bx, by, bz, state);
            discard();
            return;
        } else { drop_as_item(); discard(); return; }
    }
    delta = delta.scale(0.98);
    needs_sync = true;
}

PrimedTnt::PrimedTnt(EntityWorld* w, double x, double y, double z) : Entity("minecraft:tnt", w) {
    width = 0.98; height = 0.98; eye_height = 0.15;
    set_pos(x, y, z);
    double angle = rng->next_double() * 6.283185307179586;
    delta = Vec3(-std::sin(angle) * 0.02, 0.2, -std::cos(angle) * 0.02);
}

void PrimedTnt::tick() {
    Entity::tick();
    apply_gravity();
    move_self(delta);
    delta = delta.scale(0.98);
    if (on_ground) delta = delta.multiply(0.7, -0.5, 0.7);
    if (--fuse <= 0) {
        level->explode(Vec3(pos.x, pos.y + height / 16.0, pos.z), 4.0f, this);
        discard();
    }
    needs_sync = true;
}

void PrimedTnt::collect_data(std::vector<DataItem>& out) const {
    DataItem d;
    d.index = 8;
    d.serializer = 1;   // INT: the fuse
    uint32_t v = uint32_t(fuse);
    do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; d.value.push_back(b); } while (v);
    out.push_back(std::move(d));
}

}  // namespace pn::sim
