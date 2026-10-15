// Structure templates and the template system (StructureTemplate, StructurePlaceSettings, processors and rule tests).
#include "template.hpp"
#include "../anvil.hpp"
#include "noise.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace pn::vg {

namespace {

inline int rot_index(Rotation r) { return int(r); }

std::string with_ns(const std::string& id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }

}  // namespace

BlockPos transform_pos(const BlockPos& pos, Mirror mirror, Rotation rotation, const BlockPos& pivot) {
    int x = pos.x, y = pos.y, z = pos.z;
    bool mirrored = true;
    switch (mirror) {
        case Mirror::LEFT_RIGHT: z = -z; break;
        case Mirror::FRONT_BACK: x = -x; break;
        default: mirrored = false;
    }
    int px = pivot.x, pz = pivot.z;
    switch (rotation) {
        case Rotation::CLOCKWISE_180: return BlockPos(px + px - x, y, pz + pz - z);
        case Rotation::COUNTERCLOCKWISE_90: return BlockPos(px - pz + z, y, px + pz - x);
        case Rotation::CLOCKWISE_90: return BlockPos(px + pz - z, y, pz - px + x);
        default: return mirrored ? BlockPos(x, y, z) : pos;
    }
}

BlockPos zero_position_with_transform(const BlockPos& zero, Mirror mirror, Rotation rotation, int size_x, int size_z) {
    int mdx = mirror == Mirror::FRONT_BACK ? --size_x : 0;
    int mdz = mirror == Mirror::LEFT_RIGHT ? --size_z : 0;
    switch (rotation) {
        case Rotation::CLOCKWISE_90: return zero.offset(size_z - mdz, 0, mdx);
        case Rotation::CLOCKWISE_180: return zero.offset(size_x - mdx, 0, size_z - mdz);
        case Rotation::COUNTERCLOCKWISE_90: return zero.offset(mdz, 0, size_x - mdx);
        default: return zero.offset(mdx, 0, mdz);
    }
}

int PlaceSettings::palette_choice(size_t n, const BlockPos& pos) const {
    if (n == 0) return 0;
    if (random) return random->next_int(int(n));
    LegacyRandom r(pos_seed(pos.x, pos.y, pos.z));
    return r.next_int(int(n));
}

// ---------------------------------------------------------------- rule tests

bool RuleTest::test(const BlockData& bd, uint16_t s, Rng& random) const {
    switch (kind) {
        case ALWAYS_TRUE: return true;
        case BLOCK_MATCH: return bd.name(s) == block;
        case BLOCKSTATE_MATCH: return s == state;
        case RANDOM_BLOCK_MATCH: return bd.name(s) == block && random.next_float() < probability;
        case RANDOM_BLOCKSTATE_MATCH: return s == state && random.next_float() < probability;
        case TAG_MATCH: return bd.in_tag(s, block);
    }
    return false;
}


bool PosRuleTest::test(const BlockPos& world, const BlockPos& reference, Rng& random) const {
    switch (kind) {
        case ALWAYS_TRUE: return true;
        case LINEAR: {
            int dist = std::abs(world.x - reference.x) + std::abs(world.y - reference.y) + std::abs(world.z - reference.z);
            float rnd = random.next_float();
            return rnd <= clamped_lerp(inverse_lerp(dist, min_dist, max_dist), min_chance, max_chance);
        }
        case AXIS_ALIGNED_LINEAR: {
            float xd = axis == 0 ? float(std::abs(world.x - reference.x)) : 0.0f;
            float yd = axis == 1 ? float(std::abs(world.y - reference.y)) : 0.0f;
            float zd = axis == 2 ? float(std::abs(world.z - reference.z)) : 0.0f;
            int dist = int(xd + yd + zd);
            float rnd = random.next_float();
            return rnd <= clamped_lerp(inverse_lerp(dist, min_dist, max_dist), min_chance, max_chance);
        }
    }
    return true;
}

// ---------------------------------------------------------------- processors

namespace {

struct RuleProcessor : StructureProcessor {
    std::vector<ProcessorRule> rules;
    bool process_block(const Level& level, const BlockData& bd, const BlockPos&, const BlockPos& reference, const BlockPos&, TemplateBlock& info, const PlaceSettings&) const override {
        LegacyRandom random(pos_seed(info.pos.x, info.pos.y, info.pos.z));
        for (auto& r : rules) {
            if (!r.input.test(bd, info.state, random)) continue;
            if (!r.location.test(bd, level.get(info.pos), random)) continue;
            if (!r.position.test(info.pos, reference, random)) continue;
            info.state = r.output;
            switch (r.modifier) {
                case ProcessorRule::PASSTHROUGH: break;
                case ProcessorRule::CLEAR: info.nbt = std::make_shared<pn::Nbt>(pn::Nbt::compound()); break;
                case ProcessorRule::APPEND_STATIC: {
                    auto n = info.nbt ? std::make_shared<pn::Nbt>(*info.nbt) : std::make_shared<pn::Nbt>(pn::Nbt::compound());
                    for (auto& kv : r.modifier_data.map) n->put(kv.first, kv.second);
                    info.nbt = n;
                    break;
                }
                case ProcessorRule::APPEND_LOOT: {
                    auto n = info.nbt ? std::make_shared<pn::Nbt>(*info.nbt) : std::make_shared<pn::Nbt>(pn::Nbt::compound());
                    n->put("LootTable", pn::Nbt::str(r.modifier_data.gets("loot_table")));
                    n->put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, random.next_long()));
                    info.nbt = n;
                    break;
                }
            }
            return true;
        }
        return true;
    }
};

struct BlockIgnoreProcessor : StructureProcessor {
    std::vector<std::string> ignore;
    bool process_block(const Level&, const BlockData& bd, const BlockPos&, const BlockPos&, const BlockPos&, TemplateBlock& info, const PlaceSettings&) const override {
        return std::find(ignore.begin(), ignore.end(), bd.name(info.state)) == ignore.end();
    }
};

struct BlockRotProcessor : StructureProcessor {
    float integrity = 1.0f;
    bool has_tag = false;
    std::string tag;
    bool process_block(const Level&, const BlockData& bd, const BlockPos&, const BlockPos&, const BlockPos&, TemplateBlock& info, const PlaceSettings& s) const override {
        std::unique_ptr<Rng> own;
        Rng* random = s.random;
        if (!random) { own = std::make_unique<LegacyRandom>(pos_seed(info.pos.x, info.pos.y, info.pos.z)); random = own.get(); }
        if ((has_tag && !bd.in_tag(info.state, tag)) || random->next_float() <= integrity) return true;
        return false;
    }
};

struct ProtectedBlocksProcessor : StructureProcessor {
    std::string tag;
    bool process_block(const Level& level, const BlockData& bd, const BlockPos&, const BlockPos&, const BlockPos&, TemplateBlock& info, const PlaceSettings&) const override {
        return !bd.in_tag(level.get(info.pos), tag);
    }
};

struct GravityProcessor : StructureProcessor {
    HeightmapType type = WORLD_SURFACE_WG;
    int offset = 0;
    bool process_block(const Level& level, const BlockData&, const BlockPos&, const BlockPos&, const BlockPos& rel, TemplateBlock& info, const PlaceSettings&) const override {
        int height = level.height(type, info.pos.x, info.pos.z) + offset;
        info.pos = BlockPos(info.pos.x, height + rel.y, info.pos.z);
        return true;
    }
};

struct JigsawReplacementProcessor : StructureProcessor {
    bool process_block(const Level&, const BlockData& bd, const BlockPos&, const BlockPos&, const BlockPos&, TemplateBlock& info, const PlaceSettings&) const override {
        if (bd.name(info.state) != "minecraft:jigsaw") return true;
        if (!info.nbt) return true;
        std::string fs = info.nbt->gets("final_state");
        if (fs.empty()) fs = "minecraft:air";
        // "minecraft:block[prop=value,...]"
        std::string name = fs, props;
        size_t br = fs.find('[');
        if (br != std::string::npos) {
            name = fs.substr(0, br);
            std::string inner = fs.substr(br + 1, fs.size() - br - 2);
            std::vector<std::string> kv;
            size_t p = 0;
            while (p <= inner.size() && !inner.empty()) {
                size_t c = inner.find(',', p);
                kv.push_back(inner.substr(p, c == std::string::npos ? std::string::npos : c - p));
                if (c == std::string::npos) break;
                p = c + 1;
            }
            std::sort(kv.begin(), kv.end());
            for (auto& e : kv) props += (props.empty() ? "" : ",") + e;
        }
        name = with_ns(name);
        if (name == "minecraft:structure_void") return false;
        info.state = props.empty() ? bd.default_state(name) : bd.find(name, props);
        info.nbt = nullptr;
        return true;
    }
};

struct CappedProcessor : StructureProcessor {
    ProcessorP delegate;
    int limit = 0;
    bool evaluates_entire_piece_state() const override { return true; }
    void finalize_processing(const Level& level, const BlockData& bd, const BlockPos& position, const BlockPos& reference, const std::vector<TemplateBlock>& original, std::vector<TemplateBlock>& processed, const PlaceSettings& s) const override {
        if (limit == 0 || processed.empty()) return;
        if (original.size() != processed.size()) return;
        LegacyRandom base(s.level_seed);
        std::unique_ptr<PosFactory> f = base.fork_positional();
        std::unique_ptr<Rng> random = f->at(position.x, position.y, position.z);
        int max_to_replace = std::min(limit, int(processed.size()));
        if (max_to_replace < 1) return;
        std::vector<int> indices(processed.size());
        for (size_t i = 0; i < indices.size(); i++) indices[i] = int(i);
        for (int i = int(indices.size()); i > 1; --i) std::swap(indices[size_t(i - 1)], indices[size_t(random->next_int(i))]);
        int replaced = 0;
        for (size_t k = 0; k < indices.size() && replaced < max_to_replace; k++) {
            size_t index = size_t(indices[k]);
            TemplateBlock altered = processed[index];
            if (!delegate->process_block(level, bd, position, reference, original[index].pos, altered, s)) continue;
            if (altered.state == processed[index].state && altered.pos == processed[index].pos && altered.nbt == processed[index].nbt) continue;
            replaced++;
            processed[index] = altered;
        }
    }
};

}  // namespace

ProcessorP block_ignore_processor(bool air, bool structure_block) {
    auto p = std::make_shared<BlockIgnoreProcessor>();
    if (air) p->ignore.push_back("minecraft:air");
    if (structure_block) p->ignore.push_back("minecraft:structure_block");
    return p;
}
ProcessorP jigsaw_replacement_processor() { return std::make_shared<JigsawReplacementProcessor>(); }
ProcessorP gravity_processor(HeightmapType type, int offset) { auto p = std::make_shared<GravityProcessor>(); p->type = type; p->offset = offset; return p; }

// ---------------------------------------------------------------- template manager

TemplateManager::TemplateManager(const Registry& reg, const BlockData& bd) : reg_(reg), bd_(bd) {
    randomizable_.assign(bd.blocks().count(), 0);
    std::ifstream f(reg.dir() + "/blockentities.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        // 1: has a block entity, 2: and holds a loot table seed
        uint8_t v = line.substr(b + 1) == "1" ? 3 : 1;
        std::string name = line.substr(0, a);
        if (!bd.blocks().known(name)) continue;
        for (size_t id = 0; id < bd.blocks().count(); id++) if (bd.name(uint16_t(id)) == name) randomizable_[id] = v;
        be_types_[name] = line.substr(a + 1, b - a - 1);
    }
}

bool TemplateManager::is_randomizable_container(uint16_t state) const { return state < randomizable_.size() && (randomizable_[state] & 2); }
bool TemplateManager::has_block_entity(uint16_t state) const { return state < randomizable_.size() && randomizable_[state]; }
std::string TemplateManager::block_entity_type(uint16_t state) const {
    auto it = be_types_.find(bd_.name(state));
    return it == be_types_.end() ? std::string() : it->second;
}

uint16_t TemplateManager::state_of(const Json& j) const { return bd_.state(j); }

RuleTest TemplateManager::make_rule_test(const Json& j) {
    RuleTest t;
    std::string type = Registry::strip(j.get_string("predicate_type"));
    if (type == "always_true") t.kind = RuleTest::ALWAYS_TRUE;
    else if (type == "block_match") { t.kind = RuleTest::BLOCK_MATCH; t.block = with_ns(j.get_string("block")); }
    else if (type == "blockstate_match") { t.kind = RuleTest::BLOCKSTATE_MATCH; t.state = state_of(j.at("block_state")); }
    else if (type == "random_block_match") { t.kind = RuleTest::RANDOM_BLOCK_MATCH; t.block = with_ns(j.get_string("block")); t.probability = j.at("probability").float_value(); }
    else if (type == "random_blockstate_match") { t.kind = RuleTest::RANDOM_BLOCKSTATE_MATCH; t.state = state_of(j.at("block_state")); t.probability = j.at("probability").float_value(); }
    else if (type == "tag_match") { t.kind = RuleTest::TAG_MATCH; t.block = j.get_string("tag"); }
    return t;
}

ProcessorP TemplateManager::make_processor(const Json& j) {
    std::string type = Registry::strip(j.get_string("processor_type"));
    if (type == "rule") {
        auto p = std::make_shared<RuleProcessor>();
        for (auto& rj : j.at("rules").arr) {
            ProcessorRule r;
            r.input = make_rule_test(rj.at("input_predicate"));
            r.location = make_rule_test(rj.at("location_predicate"));
            if (const Json* pp = rj.find("position_predicate")) {
                std::string pt = Registry::strip(pp->get_string("predicate_type"));
                if (pt == "linear_pos" || pt == "axis_aligned_linear_pos") {
                    r.position.kind = pt == "linear_pos" ? PosRuleTest::LINEAR : PosRuleTest::AXIS_ALIGNED_LINEAR;
                    r.position.min_chance = pp->at("min_chance").float_value();
                    r.position.max_chance = pp->at("max_chance").float_value();
                    r.position.min_dist = int(pp->get_int("min_dist", 0));
                    r.position.max_dist = int(pp->get_int("max_dist", 0));
                    std::string ax = pp->get_string("axis", "y");
                    r.position.axis = ax == "x" ? 0 : ax == "z" ? 2 : 1;
                }
            }
            r.output = state_of(rj.at("output_state"));
            if (const Json* m = rj.find("block_entity_modifier")) {
                std::string mt = Registry::strip(m->get_string("type"));
                if (mt == "clear") r.modifier = ProcessorRule::CLEAR;
                else if (mt == "append_loot") { r.modifier = ProcessorRule::APPEND_LOOT; r.modifier_data = pn::Nbt::compound(); r.modifier_data.put("loot_table", pn::Nbt::str(with_ns(m->get_string("loot_table")))); }
                else if (mt == "append_static") r.modifier = ProcessorRule::APPEND_STATIC;   // data: parsed when a pool uses it
            }
            p->rules.push_back(std::move(r));
        }
        return p;
    }
    if (type == "block_ignore") {
        auto p = std::make_shared<BlockIgnoreProcessor>();
        for (auto& b : j.at("blocks").arr) p->ignore.push_back(with_ns(b.get_string("Name")));
        return p;
    }
    if (type == "block_rot") {
        auto p = std::make_shared<BlockRotProcessor>();
        p->integrity = j.at("integrity").float_value();
        if (const Json* t = j.find("rottable_blocks")) { p->has_tag = true; p->tag = t->str; }
        return p;
    }
    if (type == "protected_blocks") {
        auto p = std::make_shared<ProtectedBlocksProcessor>();
        p->tag = j.get_string("value");
        return p;
    }
    if (type == "gravity") {
        auto p = std::make_shared<GravityProcessor>();
        std::string hm = j.get_string("heightmap", "WORLD_SURFACE_WG");
        p->type = hm == "OCEAN_FLOOR_WG" ? OCEAN_FLOOR_WG : hm == "WORLD_SURFACE" ? WORLD_SURFACE : hm == "OCEAN_FLOOR" ? OCEAN_FLOOR : hm == "MOTION_BLOCKING" ? MOTION_BLOCKING : WORLD_SURFACE_WG;
        p->offset = int(j.get_int("offset", 0));
        return p;
    }
    if (type == "capped") {
        auto p = std::make_shared<CappedProcessor>();
        p->delegate = make_processor(j.at("delegate"));
        p->limit = int(j.at("limit").int_value());
        return p;
    }
    if (type == "nop") return std::make_shared<StructureProcessor>();
    return std::make_shared<StructureProcessor>();
}

const std::vector<ProcessorP>& TemplateManager::processor_list(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = lists_.find(id);
    if (it != lists_.end()) return it->second;
    std::vector<ProcessorP> out;
    if (reg_.exists("processor_list", id)) {
        const Json& j = reg_.get("processor_list", id);
        for (auto& pj : j.at("processors").arr) out.push_back(make_processor(pj));
    }
    return lists_.emplace(id, std::move(out)).first->second;
}

std::vector<ProcessorP> TemplateManager::processors_from(const Json& j) {
    if (j.is_str()) return processor_list(j.str);
    std::vector<ProcessorP> out;
    const Json* arr = j.is_obj() ? j.find("processors") : &j;
    if (arr && arr->is_arr()) for (auto& pj : arr->arr) out.push_back(make_processor(pj));
    return out;
}

const StructureTemplate* TemplateManager::get(const std::string& id) {
    std::string key = Registry::strip(id);
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = templates_.find(key);
        if (it != templates_.end()) return it->second.get();
    }
    auto t = std::make_unique<StructureTemplate>();
    if (!t->load(reg_.dir() + "/data/minecraft/structure/" + key + ".nbt", bd_)) t.reset();
    std::lock_guard<std::mutex> l(mu_);
    auto it = templates_.find(key);
    if (it != templates_.end()) return it->second.get();
    return templates_.emplace(key, std::move(t)).first->second.get();
}

PoolElementP TemplateManager::make_element(const Json& j) {
    std::string type = Registry::strip(j.get_string("element_type"));
    if (type == "empty_pool_element") return empty_;
    auto e = std::make_shared<PoolElement>();
    e->rigid = j.get_string("projection") == "rigid";
    if (type == "single_pool_element" || type == "legacy_single_pool_element") {
        e->type = type == "single_pool_element" ? PoolElement::SINGLE : PoolElement::LEGACY;
        e->location = with_ns(j.get_string("location"));
        if (const Json* pj = j.find("processors")) e->processors = processors_from(*pj);
        std::string ls = j.get_string("override_liquid_settings");
        e->liquid_override = ls == "apply_waterlogging" ? 0 : ls == "ignore_waterlogging" ? 1 : -1;
    } else if (type == "list_pool_element") {
        e->type = PoolElement::LIST;
        for (auto& ej : j.at("elements").arr) e->elements.push_back(make_element(ej));
        e->set_projection(e->rigid);
    } else if (type == "feature_pool_element") {
        e->type = PoolElement::FEATURE;
        e->feature = with_ns(j.get_string("feature"));
    } else return empty_;
    return e;
}

Pool* TemplateManager::pool(const std::string& id) {
    std::string key = Registry::strip(id);
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = pools_.find(key);
        if (it != pools_.end()) return it->second.get();
    }
    std::unique_ptr<Pool> p;
    if (reg_.exists("template_pool", key)) {
        const Json& j = reg_.get("template_pool", key);
        p = std::make_unique<Pool>();
        p->fallback = with_ns(j.get_string("fallback", "minecraft:empty"));
        for (auto& ej : j.at("elements").arr) {
            PoolElementP e = make_element(ej.at("element"));
            int w = int(ej.get_int("weight", 1));
            for (int i = 0; i < w; i++) p->templates.push_back(e);
        }
    }
    std::lock_guard<std::mutex> l(mu_);
    auto it = pools_.find(key);
    if (it != pools_.end()) return it->second.get();
    return pools_.emplace(key, std::move(p)).first->second.get();
}

// ---------------------------------------------------------------- template

bool StructureTemplate::load(const std::string& path, const BlockData& bd) {
    pn::Nbt root;
    if (!pn::read_nbt_file(path, root)) return false;
    const pn::Nbt* size = root.get("size");
    if (!size || size->list.size() < 3) return false;
    size_x = int(size->list[0].i); size_y = int(size->list[1].i); size_z = int(size->list[2].i);
    auto read_palette = [&](const pn::Nbt& plist, const pn::Nbt& blocks) {
        std::vector<uint16_t> states;
        for (auto& e : plist.list) {
            std::string name = e.gets("Name"), props;
            if (const pn::Nbt* pr = e.get("Properties")) {
                std::vector<std::pair<std::string, std::string>> kv;
                for (auto& m : pr->map) kv.emplace_back(m.first, m.second.s);
                std::sort(kv.begin(), kv.end());
                for (auto& p : kv) props += (props.empty() ? "" : ",") + p.first + "=" + p.second;
            }
            states.push_back(props.empty() ? bd.default_state(name) : bd.find(name, props));
        }
        std::vector<TemplateBlock> full, other, entities;
        for (auto& b : blocks.list) {
            TemplateBlock tb;
            const pn::Nbt* pos = b.get("pos");
            if (!pos || pos->list.size() < 3) continue;
            tb.pos = BlockPos(int(pos->list[0].i), int(pos->list[1].i), int(pos->list[2].i));
            size_t si = size_t(b.geti("state", 0));
            tb.state = si < states.size() ? states[si] : 0;
            if (const pn::Nbt* n = b.get("nbt")) tb.nbt = std::make_shared<pn::Nbt>(*n);
            if (tb.nbt) entities.push_back(tb);
            else if ((bd.flags(tb.state) & BF_FULL_COLLISION) && !(bd.flags(tb.state) & BF_DYNAMIC_SHAPE)) full.push_back(tb);
            else other.push_back(tb);
        }
        auto cmp = [](const TemplateBlock& a, const TemplateBlock& b) { return a.pos.y != b.pos.y ? a.pos.y < b.pos.y : a.pos.x != b.pos.x ? a.pos.x < b.pos.x : a.pos.z < b.pos.z; };
        std::stable_sort(full.begin(), full.end(), cmp);
        std::stable_sort(other.begin(), other.end(), cmp);
        std::stable_sort(entities.begin(), entities.end(), cmp);
        TemplatePalette p;
        p.blocks = std::move(full);
        p.blocks.insert(p.blocks.end(), other.begin(), other.end());
        p.blocks.insert(p.blocks.end(), entities.begin(), entities.end());
        for (auto& tb : p.blocks) {
            if (bd.name(tb.state) != "minecraft:jigsaw" || !tb.nbt) continue;
            JigsawInfo ji;
            ji.info = tb;
            std::string joint = tb.nbt->gets("joint");
            if (joint.empty()) {
                std::string o = bd.prop(tb.state, "orientation");   // "front_top"; up/down fronts roll
                ji.rollable = o.rfind("up_", 0) == 0 || o.rfind("down_", 0) == 0;
            } else ji.rollable = joint == "rollable";
            ji.name = with_ns(tb.nbt->gets("name").empty() ? "minecraft:empty" : tb.nbt->gets("name"));
            ji.pool = with_ns(tb.nbt->gets("pool").empty() ? "minecraft:empty" : tb.nbt->gets("pool"));
            ji.target = with_ns(tb.nbt->gets("target").empty() ? "minecraft:empty" : tb.nbt->gets("target"));
            ji.placement_priority = int(tb.nbt->geti("placement_priority", 0));
            ji.selection_priority = int(tb.nbt->geti("selection_priority", 0));
            p.jigsaws.push_back(std::move(ji));
        }
        palettes.push_back(std::move(p));
    };
    const pn::Nbt* blocks = root.get("blocks");
    static const pn::Nbt empty_list = pn::Nbt::list_of(pn::Nbt::Compound);
    if (!blocks) blocks = &empty_list;
    if (const pn::Nbt* pl = root.get("palettes")) { for (auto& p : pl->list) read_palette(p, *blocks); }
    else if (const pn::Nbt* p = root.get("palette")) read_palette(*p, *blocks);
    if (const pn::Nbt* el = root.get("entities")) {
        for (auto& e : el->list) {
            TemplateEntity te;
            if (const pn::Nbt* p = e.get("pos")) if (p->list.size() >= 3) { te.x = p->list[0].f; te.y = p->list[1].f; te.z = p->list[2].f; }
            if (const pn::Nbt* p = e.get("blockPos")) if (p->list.size() >= 3) te.block_pos = BlockPos(int(p->list[0].i), int(p->list[1].i), int(p->list[2].i));
            if (const pn::Nbt* n = e.get("nbt")) { te.nbt = *n; entities.push_back(std::move(te)); }
        }
    }
    return true;
}

BoundingBox StructureTemplate::bounding_box(const BlockPos& position, Rotation rot, const BlockPos& pivot, Mirror mirror) const {
    BlockPos c1 = transform_pos(BlockPos(0, 0, 0), mirror, rot, pivot);
    BlockPos c2 = transform_pos(BlockPos(size_x - 1, size_y - 1, size_z - 1), mirror, rot, pivot);
    return BoundingBox(c1.x, c1.y, c1.z, c2.x, c2.y, c2.z).moved(position.x, position.y, position.z);
}

std::vector<JigsawInfo> StructureTemplate::jigsaws(const BlockData& bd, const BlockPos& position, Rotation rotation) const {
    std::vector<JigsawInfo> out;
    if (palettes.empty()) return out;
    PlaceSettings s;
    s.rotation = rotation;
    const TemplatePalette& p = palettes[size_t(s.palette_choice(palettes.size(), position))];
    for (auto& j : p.jigsaws) {
        JigsawInfo r = j;
        BlockPos rel = transform_pos(j.info.pos, s.mirror, s.rotation, s.pivot);
        r.info.pos = BlockPos(rel.x + position.x, rel.y + position.y, rel.z + position.z);
        r.info.state = bd.rotate(j.info.state, rot_index(rotation));
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<TemplateBlock> StructureTemplate::filter_blocks(const BlockData& bd, const BlockPos& position, const PlaceSettings& s, const std::string& block, bool absolute) const {
    std::vector<TemplateBlock> out;
    if (palettes.empty()) return out;
    const TemplatePalette& p = palettes[size_t(s.palette_choice(palettes.size(), position))];
    for (auto& b : p.blocks) {
        if (bd.name(b.state) != block) continue;
        BlockPos pos = b.pos;
        if (absolute) { BlockPos rel = transform_pos(b.pos, s.mirror, s.rotation, s.pivot); pos = BlockPos(rel.x + position.x, rel.y + position.y, rel.z + position.z); }
        if (s.has_bb && !s.bb.inside(pos)) continue;
        TemplateBlock r = b;
        r.pos = pos;
        r.state = bd.rotate(b.state, rot_index(s.rotation));
        out.push_back(std::move(r));
    }
    return out;
}

bool StructureTemplate::place(Level& level, const BlockData& bd, const BlockPos& position, const BlockPos& reference, const PlaceSettings& s, Rng& random) const {
    return place_with(level, bd, nullptr, position, reference, s, random);
}

bool StructureTemplate::place_with(Level& level, const BlockData& bd, class TemplateManager* tm, const BlockPos& position, const BlockPos& reference, const PlaceSettings& s, Rng& random) const {
    if (palettes.empty()) return false;
    const TemplatePalette& palette = palettes[size_t(s.palette_choice(palettes.size(), position))];
    if ((palette.blocks.empty() && (s.ignore_entities || entities.empty())) || size_x < 1 || size_y < 1 || size_z < 1) return false;

    // processBlockInfos
    std::vector<TemplateBlock> original, processed;
    bool only_current_chunk = true;
    for (auto& p : s.processors) if (p->evaluates_entire_piece_state()) { only_current_chunk = false; break; }
    for (auto& bi : palette.blocks) {
        BlockPos rel = transform_pos(bi.pos, s.mirror, s.rotation, s.pivot);
        BlockPos world(rel.x + position.x, rel.y + position.y, rel.z + position.z);
        if (only_current_chunk && s.has_bb && !s.bb.inside(world)) continue;
        TemplateBlock pb;
        pb.pos = world;
        pb.state = bi.state;
        pb.nbt = bi.nbt ? std::make_shared<pn::Nbt>(*bi.nbt) : nullptr;
        bool keep = true;
        for (auto& p : s.processors) { if (!p->process_block(level, bd, position, reference, bi.pos, pb, s)) { keep = false; break; } }
        if (!keep) continue;
        processed.push_back(std::move(pb));
        original.push_back(bi);
    }
    for (auto& p : s.processors) p->finalize_processing(level, bd, position, reference, original, processed, s);

    std::vector<BlockPos> to_fill, locked;
    std::vector<std::pair<BlockPos, bool>> placed;
    const int rot = rot_index(s.rotation), mir = int(s.mirror);
    for (auto& bi : processed) {
        if (s.has_bb && !s.bb.inside(bi.pos)) continue;
        uint16_t prev = s.apply_waterlogging ? level.get(bi.pos) : uint16_t(0);
        bool prev_water = s.apply_waterlogging && (bd.flags(prev) & BF_WATER);
        bool prev_source = prev_water && (bd.flags(prev) & BF_SOURCE);
        uint16_t state = bd.rotate(bd.mirror(bi.state, mir), rot);
        if (!level.set(bi.pos, state)) continue;
        placed.emplace_back(bi.pos, bi.nbt != nullptr);
        bool has_be = tm ? tm->has_block_entity(state) : false;
        if (bi.nbt && has_be) {
            pn::Nbt be = *bi.nbt;
            if (tm->is_randomizable_container(state)) be.put("LootTableSeed", pn::Nbt::make(pn::Nbt::Long, random.next_long()));
            be.put("x", pn::Nbt::make(pn::Nbt::Int, bi.pos.x));
            be.put("y", pn::Nbt::make(pn::Nbt::Int, bi.pos.y));
            be.put("z", pn::Nbt::make(pn::Nbt::Int, bi.pos.z));
            if (!be.get("id")) be.put("id", pn::Nbt::str(tm->block_entity_type(state)));
            level.set_block_entity(bi.pos, std::move(be));
        } else level.remove_block_entity(bi.pos);
        if (!s.apply_waterlogging) continue;
        if (bd.flags(state) & BF_SOURCE) { locked.push_back(bi.pos); continue; }
        if (!bd.has_prop(state, "waterlogged")) continue;
        // LiquidBlockContainer.placeLiquid: a source of water makes the block waterlogged
        if (prev_source && prev_water && !(bd.flags(prev) & BF_LAVA) && bd.prop(state, "waterlogged") == "false") level.set(bi.pos, bd.with(state, "waterlogged", "true"));
        if (prev_source) continue;
        to_fill.push_back(bi.pos);
    }
    bool filled = true;
    static const int dx[5] = {0, 0, 1, 0, -1}, dy[5] = {1, 0, 0, 0, 0}, dz[5] = {0, -1, 0, 1, 0};   // UP, NORTH, EAST, SOUTH, WEST
    while (filled && !to_fill.empty()) {
        filled = false;
        for (size_t i = 0; i < to_fill.size();) {
            BlockPos pos = to_fill[i];
            uint16_t here = level.get(pos);
            bool to_place_source = (bd.flags(here) & BF_WATER) && (bd.flags(here) & BF_SOURCE);
            bool water_neighbor = false;
            for (int d = 0; d < 5 && !to_place_source; d++) {
                BlockPos np(pos.x + dx[d], pos.y + dy[d], pos.z + dz[d]);
                uint16_t n = level.get(np);
                if (!((bd.flags(n) & BF_WATER) && (bd.flags(n) & BF_SOURCE)) || std::find(locked.begin(), locked.end(), np) != locked.end()) continue;
                water_neighbor = true;
                break;
            }
            if ((to_place_source || water_neighbor) && bd.has_prop(here, "waterlogged")) {
                if (bd.prop(here, "waterlogged") == "false") level.set(pos, bd.with(here, "waterlogged", "true"));
                filled = true;
                to_fill.erase(to_fill.begin() + long(i));
            } else i++;
        }
    }
    // (updateShapeAtEdge / updateFromNeighbourShapes for templates that are not known-shape: not ported yet)
    return true;
}

}  // namespace pn::vg
