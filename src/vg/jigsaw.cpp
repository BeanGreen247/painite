// Jigsaw structures: template pools, pool elements, the piece assembler (JigsawPlacement) and the jigsaw structure type.
// Villages, pillager outposts, trail ruins, ancient cities, bastion remnants, trial chambers and abandoned camps are all made here.
#include "structure.hpp"
#include "template.hpp"
#include <algorithm>
#include <deque>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "jcompat.hpp"

namespace pn::vg {

namespace {

// a box with double coordinates (an AABB); shapes of boxes are kept as lists of disjoint boxes
struct DBox {
    double x0, y0, z0, x1, y1, z1;
};
using Shape = std::vector<DBox>;
using ShapeP = std::shared_ptr<Shape>;

DBox aabb_of(const BoundingBox& b) { return {double(b.x0), double(b.y0), double(b.z0), double(b.x1 + 1), double(b.y1 + 1), double(b.z1 + 1)}; }

// a minus b, as disjoint boxes appended to `out`
void subtract(const DBox& a, const DBox& b, Shape& out) {
    double ix0 = std::max(a.x0, b.x0), iy0 = std::max(a.y0, b.y0), iz0 = std::max(a.z0, b.z0);
    double ix1 = std::min(a.x1, b.x1), iy1 = std::min(a.y1, b.y1), iz1 = std::min(a.z1, b.z1);
    if (ix0 >= ix1 || iy0 >= iy1 || iz0 >= iz1) { out.push_back(a); return; }
    if (a.y0 < iy0) out.push_back({a.x0, a.y0, a.z0, a.x1, iy0, a.z1});
    if (iy1 < a.y1) out.push_back({a.x0, iy1, a.z0, a.x1, a.y1, a.z1});
    if (a.x0 < ix0) out.push_back({a.x0, iy0, a.z0, ix0, iy1, a.z1});
    if (ix1 < a.x1) out.push_back({ix1, iy0, a.z0, a.x1, iy1, a.z1});
    if (a.z0 < iz0) out.push_back({ix0, iy0, a.z0, ix1, iy1, iz0});
    if (iz1 < a.z1) out.push_back({ix0, iy0, iz1, ix1, iy1, a.z1});
}

Shape shape_minus(const Shape& a, const DBox& b) {
    Shape out;
    for (auto& box : a) subtract(box, b, out);
    return out;
}

// Shapes.joinIsNotEmpty(free, box, ONLY_SECOND): does `box` reach outside `free`?
bool reaches_outside(const Shape& free_space, const DBox& box) {
    Shape rest{box};
    for (auto& f : free_space) {
        Shape next;
        for (auto& r : rest) subtract(r, f, next);
        rest = std::move(next);
        if (rest.empty()) return false;
    }
    return !rest.empty();
}

// Util.shuffle
template <class T>
void shuffle(std::vector<T>& list, Rng& random) {
    for (int i = int(list.size()); i > 1; --i) std::swap(list[size_t(i - 1)], list[size_t(random.next_int(i))]);
}

const char* kDirNames[6] = {"down", "up", "north", "south", "west", "east"};
int dir_of(const std::string& n) { for (int i = 0; i < 6; i++) if (n == kDirNames[i]) return i; return NORTH; }
int opposite(int d) { return d ^ 1; }   // down/up, north/south, west/east are paired

// JigsawBlock.ORIENTATION "front_top"
void orientation(const BlockData& bd, uint16_t state, int& front, int& top) {
    std::string o = bd.prop(state, "orientation");
    size_t u = o.find('_');
    front = dir_of(o.substr(0, u));
    top = u == std::string::npos ? UP : dir_of(o.substr(u + 1));
}

bool can_attach(const BlockData& bd, const JigsawInfo& source, const JigsawInfo& target) {
    int sf, st, tf, tt;
    orientation(bd, source.info.state, sf, st);
    orientation(bd, target.info.state, tf, tt);
    return sf == opposite(tf) && (source.rollable || st == tt) && source.target == target.name;
}

// ---------------------------------------------------------------- pool elements

BoundingBox element_bounds(TemplateManager& tm, const PoolElement& e, const BlockPos& position, Rotation rot);

BoundingBox element_bounds(TemplateManager& tm, const PoolElement& e, const BlockPos& position, Rotation rot) {
    switch (e.type) {
        case PoolElement::SINGLE:
        case PoolElement::LEGACY: {
            const StructureTemplate* t = tm.get(e.location);
            if (!t) return BoundingBox(position.x, position.y, position.z, position.x - 1, position.y - 1, position.z - 1);
            return t->bounding_box(position, rot, BlockPos(0, 0, 0), Mirror::NONE);
        }
        case PoolElement::LIST: {
            BoundingBox b = BoundingBox::infinite();
            for (auto& s : e.elements) if (s->type != PoolElement::EMPTY) b.encapsulate(element_bounds(tm, *s, position, rot));
            return b;
        }
        case PoolElement::FEATURE: return BoundingBox(position.x, position.y, position.z, position.x, position.y, position.z);
        case PoolElement::EMPTY: break;
    }
    return BoundingBox();
}

std::vector<JigsawInfo> element_jigsaws(TemplateManager& tm, const PoolElement& e, const BlockPos& position, Rotation rot, Rng& random) {
    std::vector<JigsawInfo> out;
    switch (e.type) {
        case PoolElement::SINGLE:
        case PoolElement::LEGACY: {
            const StructureTemplate* t = tm.get(e.location);
            if (!t) return out;
            out = t->jigsaws(tm.bd(), position, rot);
            shuffle(out, random);
            std::stable_sort(out.begin(), out.end(), [](const JigsawInfo& a, const JigsawInfo& b) { return a.selection_priority > b.selection_priority; });
            return out;
        }
        case PoolElement::LIST: return element_jigsaws(tm, *e.elements[0], position, rot, random);
        case PoolElement::FEATURE: {
            // a single jigsaw pointing down named "bottom", joined rollable
            JigsawInfo ji;
            ji.info.pos = position;
            ji.info.state = tm.bd().find("minecraft:jigsaw", "orientation=down_south");
            ji.rollable = true;
            ji.name = "minecraft:bottom";
            ji.pool = "minecraft:empty";
            ji.target = "minecraft:empty";
            out.push_back(std::move(ji));
            return out;
        }
        case PoolElement::EMPTY: break;
    }
    return out;
}

int element_ground_level_delta(const PoolElement&) { return 1; }

// SinglePoolElement.place / LegacySinglePoolElement / ListPoolElement / FeaturePoolElement
bool element_place(PieceCtx& ctx, const PoolElement& e, const BlockPos& position, const BlockPos& reference, Rotation rot, const BoundingBox& chunk_bb, bool ignore_waterlogging, bool keep_jigsaws) {
    TemplateManager& tm = ctx.templates;
    switch (e.type) {
        case PoolElement::SINGLE:
        case PoolElement::LEGACY: {
            const StructureTemplate* t = tm.get(e.location);
            if (!t) return false;
            PlaceSettings s;
            s.has_bb = true;
            s.bb = chunk_bb;
            s.rotation = rot;
            s.known_shape = true;
            s.ignore_entities = false;
            s.finalize_entities = true;
            s.level_seed = ctx.senv.seed;
            if (e.liquid_override >= 0) ignore_waterlogging = e.liquid_override == 1;
            s.apply_waterlogging = !ignore_waterlogging;
            s.processors.push_back(block_ignore_processor(false, true));
            if (!keep_jigsaws) s.processors.push_back(jigsaw_replacement_processor());
            for (auto& p : e.processors) s.processors.push_back(p);
            if (!e.rigid) s.processors.push_back(gravity_processor(WORLD_SURFACE_WG, -1));
            if (e.type == PoolElement::LEGACY) {   // popProcessor(STRUCTURE_BLOCK), addProcessor(STRUCTURE_AND_AIR)
                s.processors.erase(s.processors.begin());
                s.processors.push_back(block_ignore_processor(true, true));
            }
            return t->place_with(ctx.level, tm.bd(), &tm, position, reference, s, ctx.random);
        }
        case PoolElement::LIST:
            for (auto& sub : e.elements)
                if (!element_place(ctx, *sub, position, reference, rot, chunk_bb, ignore_waterlogging, keep_jigsaws)) return false;
            return true;
        case PoolElement::FEATURE: {
            const std::string id = e.feature;
            if (!ctx.senv.reg->exists("placed_feature", id)) return false;
            return place_placed_feature(ctx.fenv, ctx.senv.reg->get("placed_feature", id), ctx.random, position, nullptr);
        }
        case PoolElement::EMPTY: return true;
    }
    return true;
}

// PoolElementStructurePiece
class PoolPiece : public StructurePiece {
public:
    PoolElementP element;
    BlockPos position;
    bool ignore_waterlogging = false;
    void post_process(PieceCtx& ctx, const BlockPos& pivot) override {
        element_place(ctx, *element, position, pivot, rotation, ctx.chunk_bb, ignore_waterlogging, false);
    }
    void move(int dx, int dy, int dz) override { bb.move(dx, dy, dz); position = position.offset(dx, dy, dz); }
    std::string describe() const override {
        static const char* rn[4] = {"NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90"};
        char buf[512];
        snprintf(buf, sizeof buf, "%s BB[%d,%d,%d,%d,%d,%d] pos(%d,%d,%d) gld=%d rot=%s el=%s", type.c_str(), bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1, position.x, position.y, position.z,
                 ground_level_delta, rn[int(rotation)], element->type == PoolElement::SINGLE || element->type == PoolElement::LEGACY ? element->location.c_str() : element->type == PoolElement::LIST ? "list" : element->type == PoolElement::FEATURE ? element->feature.c_str() : "empty");
        return buf;
    }
};

// ---------------------------------------------------------------- placement

struct PieceState {
    std::shared_ptr<PoolPiece> piece;
    ShapeP free;
    int depth;
};

// SequencedPriorityIterator
class PriorityQueue {
public:
    void add(PieceState s, int priority) {
        if (priority == highest_ && highest_queue_) { highest_queue_->push_back(std::move(s)); return; }
        auto& q = queues_[priority];
        q.push_back(std::move(s));
        if (priority >= highest_) { highest_queue_ = &q; highest_ = priority; }
    }
    bool has_next() const { return highest_queue_ != nullptr && !highest_queue_->empty(); }
    PieceState next() {
        PieceState r = std::move(highest_queue_->front());
        highest_queue_->pop_front();
        if (highest_queue_->empty()) switch_queue();
        return r;
    }
private:
    void switch_queue() {
        int found = INT32_MIN;
        std::deque<PieceState>* fq = nullptr;
        for (auto& kv : queues_) {
            if (kv.first <= found || kv.second.empty()) continue;
            found = kv.first;
            fq = &kv.second;
        }
        highest_ = found;
        highest_queue_ = fq;
    }
    std::map<int, std::deque<PieceState>> queues_;
    std::deque<PieceState>* highest_queue_ = nullptr;
    int highest_ = INT32_MIN;
};

inline jc::JRandom jc_positional(int64_t seed, const BlockPos& pos) { return jc::positionalRandom(seed, pos); }

// PoolAliasBinding: direct, random or random_group
struct AliasBinding {
    enum Type { DIRECT, RANDOM, RANDOM_GROUP } type = DIRECT;
    std::string alias, target;
    std::vector<std::pair<std::string, int>> targets;                          // random
    std::vector<std::pair<std::vector<AliasBinding>, int>> groups;             // random_group
};

AliasBinding parse_alias(const Json& j) {
    AliasBinding b;
    std::string t = Registry::strip(j.get_string("type"));
    auto id = [](const std::string& s) { return s.rfind("minecraft:", 0) == 0 ? s : "minecraft:" + s; };
    if (t == "direct") { b.type = AliasBinding::DIRECT; b.alias = id(j.get_string("alias")); b.target = id(j.get_string("target")); }
    else if (t == "random") {
        b.type = AliasBinding::RANDOM;
        b.alias = id(j.get_string("alias"));
        for (auto& e : j.at("targets").arr) b.targets.emplace_back(id(e.get_string("data")), int(e.get_int("weight", 1)));
    } else {
        b.type = AliasBinding::RANDOM_GROUP;
        for (auto& g : j.at("groups").arr) {
            std::vector<AliasBinding> list;
            for (auto& e : g.at("data").arr) list.push_back(parse_alias(e));
            b.groups.emplace_back(std::move(list), int(g.get_int("weight", 1)));
        }
    }
    return b;
}

void resolve_alias(const AliasBinding& b, Rng& random, std::map<std::string, std::string>& out) {
    switch (b.type) {
        case AliasBinding::DIRECT: out[b.alias] = b.target; break;
        case AliasBinding::RANDOM: {
            int total = 0;
            for (auto& t : b.targets) total += t.second;
            int sel = random.next_int(total);
            for (auto& t : b.targets) { if (sel < t.second) { out[b.alias] = t.first; break; } sel -= t.second; }
            break;
        }
        case AliasBinding::RANDOM_GROUP: {
            int total = 0;
            for (auto& g : b.groups) total += g.second;
            int sel = random.next_int(total);
            for (auto& g : b.groups) {
                if (sel < g.second) { for (auto& e : g.first) resolve_alias(e, random, out); break; }
                sel -= g.second;
            }
            break;
        }
    }
}

struct JigsawParams {
    std::vector<AliasBinding> aliases;
    std::string start_pool, start_jigsaw;
    int max_depth = 6;
    bool expansion_hack = false;
    bool has_heightmap = false;
    HeightmapType heightmap = WORLD_SURFACE_WG;
    int max_horizontal = 80, max_vertical = 4064;
    int padding_bottom = 0, padding_top = 0;
    bool ignore_waterlogging = false;
    // start_height provider
    std::function<int(Rng&, int, int)> start_height;
};

int pool_max_size(TemplateManager& tm, Pool& pool) {
    if (pool.max_size == INT32_MIN) {
        int m = 0;
        bool any = false;
        for (auto& e : pool.templates) {
            if (e->type == PoolElement::EMPTY) continue;
            m = any ? std::max(m, element_bounds(tm, *e, BlockPos(0, 0, 0), Rotation::NONE).y_span()) : element_bounds(tm, *e, BlockPos(0, 0, 0), Rotation::NONE).y_span();
            any = true;
        }
        pool.max_size = m;
    }
    return pool.max_size;
}

std::vector<PoolElementP> shuffled_templates(const Pool& pool, Rng& random) {
    std::vector<PoolElementP> v = pool.templates;
    shuffle(v, random);
    return v;
}

class Placer {
public:
    Placer(const StructureContext& ctx, TemplateManager& tm, const std::map<std::string, std::string>& aliases, const JigsawParams& p, std::vector<std::shared_ptr<PoolPiece>>& pieces) : ctx_(ctx), tm_(tm), aliases_(aliases), p_(p), pieces_(pieces) {}
    std::string lookup(const std::string& id) const { auto it = aliases_.find(id); return it == aliases_.end() ? id : it->second; }
    PriorityQueue placing;

    void try_placing_children(const std::shared_ptr<PoolPiece>& source_piece, ShapeP context_free, int depth) {
        Rng& random = ctx_.random;
        const BlockData& bd = tm_.bd();
        const PoolElement& source_element = *source_piece->element;
        BlockPos source_box_position = source_piece->position;
        Rotation source_rotation = source_piece->rotation;
        bool source_rigid = source_element.rigid;
        ShapeP source_free;
        BoundingBox source_bb = source_piece->bb;
        int source_box_y = source_bb.y0;
        std::vector<JigsawInfo> sources = element_jigsaws(tm_, source_element, source_box_position, source_rotation, random);
        for (JigsawInfo& source_jigsaw : sources) {
            BlockPos source_pos = source_jigsaw.info.pos;
            int sf, st;
            orientation(bd, source_jigsaw.info.state, sf, st);
            BlockPos target_pos(source_pos.x + kDirX[sf], source_pos.y + kDirY[sf], source_pos.z + kDirZ[sf]);
            int source_local_y = source_pos.y - source_box_y;
            int source_base_height = INT32_MIN;
            Pool* target_pool = tm_.pool(lookup(source_jigsaw.pool));
            if (!target_pool || (target_pool->templates.empty() && Registry::strip(source_jigsaw.pool) != "empty")) continue;
            Pool* fallback = tm_.pool(target_pool->fallback);
            if (!fallback || (fallback->templates.empty() && Registry::strip(target_pool->fallback) != "empty")) continue;
            bool attach_inside_source = source_bb.inside(target_pos);
            ShapeP children_free;
            if (attach_inside_source) {
                children_free = source_free;
                if (!source_free) { source_free = std::make_shared<Shape>(Shape{aabb_of(source_bb)}); children_free = source_free; }
            } else children_free = context_free;
            std::vector<PoolElementP> target_pieces;
            if (depth != p_.max_depth) target_pieces = shuffled_templates(*target_pool, random);
            { auto fb = shuffled_templates(*fallback, random); target_pieces.insert(target_pieces.end(), fb.begin(), fb.end()); }
            int placement_priority = source_jigsaw.placement_priority;
            static const bool trace = std::getenv("PAINITE_JIGSAW_TRACE") != nullptr;
            if (trace) fprintf(stderr, "[jigsaw] depth %d src pos(%d,%d,%d) front=%s pool=%s name=%s target=%s inside=%d candidates=%zu\n", depth, source_pos.x, source_pos.y, source_pos.z, kDirNames[sf], source_jigsaw.pool.c_str(), source_jigsaw.name.c_str(), source_jigsaw.target.c_str(), int(attach_inside_source), target_pieces.size());
            bool placed_one = false;
            for (auto& target_element : target_pieces) {
                if (placed_one) break;
                if (target_element->type == PoolElement::EMPTY) break;
                std::vector<Rotation> rotations = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
                shuffle(rotations, random);
                for (Rotation target_rotation : rotations) {
                    if (placed_one) break;
                    std::vector<JigsawInfo> target_jigsaws = element_jigsaws(tm_, *target_element, BlockPos(0, 0, 0), target_rotation, random);
                    BoundingBox hack_box = element_bounds(tm_, *target_element, BlockPos(0, 0, 0), target_rotation);
                    int expand_to = 0;
                    if (p_.expansion_hack && hack_box.y_span() <= 16) {
                        for (auto& tj : target_jigsaws) {
                            int tf, tt;
                            orientation(bd, tj.info.state, tf, tt);
                            BlockPos front(tj.info.pos.x + kDirX[tf], tj.info.pos.y + kDirY[tf], tj.info.pos.z + kDirZ[tf]);
                            if (!hack_box.inside(front)) continue;
                            Pool* cp = tm_.pool(lookup(tj.pool));
                            Pool* cfb = cp ? tm_.pool(cp->fallback) : nullptr;
                            int cs = cp ? pool_max_size(tm_, *cp) : 0;
                            int cfs = cfb ? pool_max_size(tm_, *cfb) : 0;
                            expand_to = std::max(expand_to, std::max(cs, cfs));
                        }
                    }
                    for (JigsawInfo& target_jigsaw : target_jigsaws) {
                        if (!can_attach(bd, source_jigsaw, target_jigsaw)) continue;
                        BlockPos target_local = target_jigsaw.info.pos;
                        BlockPos raw_target_box_pos(target_pos.x - target_local.x, target_pos.y - target_local.y, target_pos.z - target_local.z);
                        BoundingBox raw_target_bb = element_bounds(tm_, *target_element, raw_target_box_pos, target_rotation);
                        int raw_target_y = raw_target_bb.y0;
                        bool target_rigid = target_element->rigid;
                        int target_local_y = target_local.y;
                        int delta_y = source_local_y - target_local_y + kDirY[sf];
                        int target_box_y;
                        if (source_rigid && target_rigid) target_box_y = source_box_y + delta_y;
                        else {
                            if (source_base_height == INT32_MIN) source_base_height = ctx_.env.base_height(source_pos.x, source_pos.z, WORLD_SURFACE_WG);
                            target_box_y = source_base_height - target_local_y;
                        }
                        int y_offset = target_box_y - raw_target_y;
                        BoundingBox target_bb = raw_target_bb.moved(0, y_offset, 0);
                        BlockPos target_box_position = raw_target_box_pos.offset(0, y_offset, 0);
                        if (expand_to > 0) {
                            int new_size = std::max(expand_to + 1, target_bb.y1 - target_bb.y0);
                            target_bb.encapsulate(BlockPos(target_bb.x0, target_bb.y0 + new_size, target_bb.z0));
                        }
                        DBox tb = aabb_of(target_bb);
                        DBox deflated{tb.x0 + 0.25, tb.y0 + 0.25, tb.z0 + 0.25, tb.x1 - 0.25, tb.y1 - 0.25, tb.z1 - 0.25};
                        if (reaches_outside(*children_free, deflated)) { if (trace) fprintf(stderr, "[jigsaw]   reject %s rot=%d bb[%d,%d,%d,%d,%d,%d] (outside free space)\n", target_element->location.c_str(), int(target_rotation), target_bb.x0, target_bb.y0, target_bb.z0, target_bb.x1, target_bb.y1, target_bb.z1); continue; }
                        if (trace) fprintf(stderr, "[jigsaw]   place %s rot=%d bb[%d,%d,%d,%d,%d,%d]\n", target_element->location.c_str(), int(target_rotation), target_bb.x0, target_bb.y0, target_bb.z0, target_bb.x1, target_bb.y1, target_bb.z1);
                        *children_free = shape_minus(*children_free, tb);
                        int source_ground = source_piece->ground_level_delta;
                        int target_ground = target_rigid ? source_ground - delta_y : element_ground_level_delta(*target_element);
                        auto target_piece = std::make_shared<PoolPiece>();
                        target_piece->type = "minecraft:jigsaw";
                        target_piece->element = target_element;
                        target_piece->position = target_box_position;
                        target_piece->ground_level_delta = target_ground;
                        target_piece->rotation = target_rotation;
                        target_piece->bb = target_bb;
                        target_piece->ignore_waterlogging = p_.ignore_waterlogging;
                        target_piece->terrain_adaptation = source_piece->terrain_adaptation;
                        target_piece->is_pool = true;
                        target_piece->rigid = target_element->rigid;
                        int junction_y;
                        if (source_rigid) junction_y = source_box_y + source_local_y;
                        else if (target_rigid) junction_y = target_box_y + target_local_y;
                        else {
                            if (source_base_height == INT32_MIN) source_base_height = ctx_.env.base_height(source_pos.x, source_pos.z, WORLD_SURFACE_WG);
                            junction_y = source_base_height + delta_y / 2;
                        }
                        source_piece->junctions.push_back({target_pos.x, junction_y - source_local_y + source_ground, target_pos.z, delta_y, target_rigid});
                        target_piece->junctions.push_back({source_pos.x, junction_y - target_local_y + target_ground, source_pos.z, -delta_y, source_rigid});
                        pieces_.push_back(target_piece);
                        if (depth + 1 <= p_.max_depth) placing.add(PieceState{target_piece, children_free, depth + 1}, placement_priority);
                        placed_one = true;
                        break;
                    }
                }
            }
        }
    }

private:
    const StructureContext& ctx_;
    TemplateManager& tm_;
    const std::map<std::string, std::string>& aliases_;
    const JigsawParams& p_;
    std::vector<std::shared_ptr<PoolPiece>>& pieces_;
};

std::optional<GenerationStub> add_pieces(const StructureContext& ctx, const JigsawParams& p, const BlockPos& position) {
    TemplateManager& tm = *ctx.env.templates;
    Rng& random = ctx.random;
    // Rotation.getRandom: Util.getRandom(Rotation.values(), random)
    static const Rotation kRots[4] = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
    Rotation center_rotation = kRots[random.next_int(4)];
    std::map<std::string, std::string> aliases;
    if (!p.aliases.empty()) {
        jc::JRandom alias_random = jc_positional(ctx.env.seed, position);
        for (auto& b : p.aliases) resolve_alias(b, alias_random.r(), aliases);
    }
    auto lookup = [&aliases](const std::string& id) { auto it = aliases.find(id); return it == aliases.end() ? id : it->second; };
    Pool* center_pool = tm.pool(lookup(p.start_pool));
    if (std::getenv("DBGST")) fprintf(stderr, "jigsaw pool %s: %p %zu\n", p.start_pool.c_str(), (void*)center_pool, center_pool ? center_pool->templates.size() : size_t(0));
    if (!center_pool || center_pool->templates.empty()) return std::nullopt;
    PoolElementP center_element = center_pool->templates[size_t(random.next_int(int(center_pool->templates.size())))];
    if (center_element->type == PoolElement::EMPTY) return std::nullopt;
    BlockPos anchored = position;
    if (!p.start_jigsaw.empty()) {
        std::vector<JigsawInfo> js = element_jigsaws(tm, *center_element, position, center_rotation, random);
        bool found = false;
        for (auto& j : js) if (j.name == p.start_jigsaw) { anchored = j.info.pos; found = true; break; }
        if (std::getenv("DBGST")) fprintf(stderr, "jigsaw start %s found %d of %zu\n", p.start_jigsaw.c_str(), int(found), js.size());
        if (!found) return std::nullopt;
    }
    BlockPos local_anchor(anchored.x - position.x, anchored.y - position.y, anchored.z - position.z);
    BlockPos adjusted(position.x - local_anchor.x, position.y - local_anchor.y, position.z - local_anchor.z);
    auto center_piece = std::make_shared<PoolPiece>();
    center_piece->type = "minecraft:jigsaw";
    center_piece->element = center_element;
    center_piece->position = adjusted;
    center_piece->ground_level_delta = element_ground_level_delta(*center_element);
    center_piece->rotation = center_rotation;
    center_piece->bb = element_bounds(tm, *center_element, adjusted, center_rotation);
    center_piece->ignore_waterlogging = p.ignore_waterlogging;
    center_piece->is_pool = true;
    center_piece->rigid = center_element->rigid;
    BoundingBox box = center_piece->bb;
    int center_x = (box.x1 + box.x0) / 2, center_z = (box.z1 + box.z0) / 2;
    int bottom_y = !p.has_heightmap ? adjusted.y : position.y + ctx.env.base_height(center_x, center_z, p.heightmap);
    int old_absolute_ground_y = box.y0 + center_piece->ground_level_delta;
    center_piece->move(0, bottom_y - old_absolute_ground_y, 0);
    if (p.padding_bottom || p.padding_top) {
        int min_with = ctx.env.min_y + p.padding_bottom, max_with = ctx.env.min_y + ctx.env.height - 1 - p.padding_top;
        if (center_piece->bb.y0 < min_with || center_piece->bb.y1 > max_with) return std::nullopt;
    }
    box = center_piece->bb;   // BoundingBox is mutated in place by move(): Mojang's `box` follows it
    int center_y = bottom_y + local_anchor.y;
    GenerationStub stub;
    stub.position = BlockPos(center_x, center_y, center_z);
    stub.generate = [&ctx, &p, aliases, center_piece, center_x, center_y, center_z, box](std::vector<std::shared_ptr<StructurePiece>>& out) {
        TemplateManager& tm2 = *ctx.env.templates;
        std::vector<std::shared_ptr<PoolPiece>> pieces;
        pieces.push_back(center_piece);
        if (p.max_depth > 0) {
            double min_y = std::max(double(center_y - p.max_vertical), double(ctx.env.min_y + p.padding_bottom));
            double max_y = std::min(double(center_y + p.max_vertical + 1), double(ctx.env.min_y + ctx.env.height - p.padding_top));
            DBox aabb{double(center_x - p.max_horizontal), min_y, double(center_z - p.max_horizontal), double(center_x + p.max_horizontal + 1), max_y, double(center_z + p.max_horizontal + 1)};
            auto free_space = std::make_shared<Shape>(shape_minus(Shape{aabb}, aabb_of(box)));
            Placer placer(ctx, tm2, aliases, p, pieces);
            placer.try_placing_children(center_piece, free_space, 0);
            while (placer.placing.has_next()) {
                PieceState st = placer.placing.next();
                placer.try_placing_children(st.piece, st.free, st.depth);
            }
        }
        for (auto& pc : pieces) out.push_back(pc);
    };
    return stub;
}

class JigsawStructure : public Structure {
public:
    JigsawParams params;
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        int height = params.start_height(ctx.random, ctx.env.gen_min_y, ctx.env.gen_height);
        BlockPos start(ctx.chunk.x * 16, height, ctx.chunk.z * 16);
        return add_pieces(ctx, params, start);
    }
};

struct RegisterJigsaw {
    RegisterJigsaw() {
        structure_factories()["minecraft:jigsaw"] = [](const Json& j, const Registry&) -> std::unique_ptr<Structure> {
            auto s = std::make_unique<JigsawStructure>();
            JigsawParams& p = s->params;
            p.start_pool = "minecraft:" + Registry::strip(j.at("start_pool").str);
            if (const Json* sj = j.find("start_jigsaw_name")) p.start_jigsaw = "minecraft:" + Registry::strip(sj->str);
            p.max_depth = int(j.at("size").int_value());
            p.expansion_hack = j.get_bool("use_expansion_hack", false);
            if (const Json* hm = j.find("project_start_to_heightmap")) {
                p.has_heightmap = true;
                const std::string& n = hm->str;
                p.heightmap = n == "OCEAN_FLOOR_WG" ? OCEAN_FLOOR_WG : n == "WORLD_SURFACE" ? WORLD_SURFACE : n == "OCEAN_FLOOR" ? OCEAN_FLOOR : n == "MOTION_BLOCKING" ? MOTION_BLOCKING : WORLD_SURFACE_WG;
            }
            const Json& md = j.at("max_distance_from_center");
            if (md.is_obj()) { p.max_horizontal = int(md.get_int("horizontal", 80)); p.max_vertical = int(md.get_int("vertical", 4064)); }
            else { p.max_horizontal = int(md.int_value()); p.max_vertical = p.max_horizontal; }
            if (const Json* dp = j.find("dimension_padding")) {
                if (dp->is_obj()) { p.padding_bottom = int(dp->get_int("bottom", 0)); p.padding_top = int(dp->get_int("top", 0)); }
                else { p.padding_bottom = p.padding_top = int(dp->int_value()); }
            }
            p.ignore_waterlogging = j.get_string("liquid_settings") == "ignore_waterlogging";
            if (const Json* al = j.find("pool_aliases")) for (auto& e : al->arr) p.aliases.push_back(parse_alias(e));
            p.start_height = make_height_provider(j.at("start_height"));
            return s;
        };
    }
} register_jigsaw;

}  // namespace

}  // namespace pn::vg
