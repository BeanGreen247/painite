#include "loot.hpp"
#include "components.hpp"
#include "data.hpp"
#include <cmath>
#include <mutex>

namespace pn::sim {

using vg::Json;

namespace {

std::string with_ns(const std::string& s) { return s.find(':') == std::string::npos ? "minecraft:" + s : s; }

// ---- number providers

struct NumberProvider {
    virtual ~NumberProvider() = default;
    virtual int get_int(LootContext& c) const = 0;
    virtual float get_float(LootContext& c) const = 0;
};
using NP = std::unique_ptr<NumberProvider>;

struct Constant : NumberProvider {
    float value;
    explicit Constant(float v) : value(v) {}
    int get_int(LootContext&) const override { return int(std::floor(value + 0.5f)); }   // Math.round
    float get_float(LootContext&) const override { return value; }
};
static int nextInt3(vg::Rng& r, int lo, int hi) { if (lo >= hi) return lo; return r.next_int(hi - lo + 1) + lo; }
static float nextFloat3(vg::Rng& r, float lo, float hi) { if (lo >= hi) return lo; return r.next_float() * (hi - lo) + lo; }
NP parse_np(const Json& j);
struct Uniform : NumberProvider {
    NP min, max;
    int get_int(LootContext& c) const override { int a = min->get_int(c), b = max->get_int(c); return nextInt3(c.random, a, b); }
    float get_float(LootContext& c) const override { float a = min->get_float(c), b = max->get_float(c); return nextFloat3(c.random, a, b); }
};
struct Binomial : NumberProvider {
    NP n, p;
    int get_int(LootContext& c) const override {
        int nn = n->get_int(c);
        float pp = p->get_float(c);
        int r = 0;
        for (int i = 0; i < nn; i++) if (c.random.next_float() < pp) ++r;
        return r;
    }
    float get_float(LootContext& c) const override { return float(get_int(c)); }
};
struct SumProvider : NumberProvider {
    std::vector<NP> summands;
    int get_int(LootContext& c) const override { int s = 0; for (auto& x : summands) s += x->get_int(c); return s; }
    float get_float(LootContext& c) const override { float s = 0; for (auto& x : summands) s += x->get_float(c); return s; }
};
NP parse_np(const Json& j) {
    if (j.is_num()) return std::make_unique<Constant>(j.float_value());
    std::string type = j.get_string("type", "minecraft:constant");
    if (type == "minecraft:constant" || type == "constant") return std::make_unique<Constant>(j.at("value").float_value());
    if (type == "minecraft:uniform" || type == "uniform") { auto u = std::make_unique<Uniform>(); u->min = parse_np(j.at("min")); u->max = parse_np(j.at("max")); return u; }
    if (type == "minecraft:binomial" || type == "binomial") { auto b = std::make_unique<Binomial>(); b->n = parse_np(j.at("n")); b->p = parse_np(j.at("p")); return b; }
    if (type == "minecraft:sum" || type == "sum") { auto s = std::make_unique<SumProvider>(); for (auto& e : j.at("summands").arr) s->summands.push_back(parse_np(e)); return s; }
    return std::make_unique<Constant>(0);   // score, storage, enchantment_level, environment attribute: not available to loot rolls here yet
}

struct IntRange {
    NP min, max;
    static IntRange parse(const Json& j) {
        IntRange r;
        if (j.is_num()) { r.min = std::make_unique<Constant>(j.float_value()); r.max = std::make_unique<Constant>(j.float_value()); return r; }
        if (const Json* a = j.find("min")) r.min = parse_np(*a);
        if (const Json* b = j.find("max")) r.max = parse_np(*b);
        return r;
    }
    int clamp(LootContext& c, int v) const {
        if (!min && !max) return v;
        if (!min) return std::min(max->get_int(c), v);
        if (!max) return std::max(min->get_int(c), v);
        int lo = min->get_int(c), hi = max->get_int(c);
        return v < lo ? lo : v > hi ? hi : v;
    }
    bool test(LootContext& c, int v) const {
        if (min && v < min->get_int(c)) return false;
        if (max && v > max->get_int(c)) return false;
        return true;
    }
};

// ---- LevelBasedValue (enchantment effects)
struct LevelValue {
    std::string type = "constant";
    float a = 0, b = 0;   // constant: a; linear: base a, per level b; levels_squared: added a; fraction etc. not needed by loot
    float calc(int level) const {
        if (type == "linear") return a + b * float(level - 1);
        if (type == "levels_squared") return float(level * level) + a;
        return a;
    }
    static LevelValue parse(const Json& j) {
        LevelValue v;
        if (j.is_num()) { v.a = j.float_value(); return v; }
        std::string t = j.get_string("type", "minecraft:constant");
        if (t.rfind("minecraft:", 0) == 0) t = t.substr(10);
        v.type = t;
        if (t == "constant") v.a = j.at("value").float_value();
        else if (t == "linear") { v.a = j.at("base").float_value(); v.b = j.at("per_level_above_first").float_value(); }
        else if (t == "levels_squared") v.a = j.at("added").float_value();
        return v;
    }
};

// ---- item predicate (match_tool)
struct ItemPredicate {
    std::string items;                 // tag, id or comma separated ids
    bool has_count = false;
    IntRange count;
    struct Ench { std::string enchantments; bool has_min = false; int min = 0; bool has_max = false; int max = 0; };
    std::vector<Ench> enchants;
    bool matches(LootContext& c, const ItemStack& s) const {
        if (s.is_empty()) return false;
        if (!items.empty() && !Tags::get().matches("item", items, s.item()->name)) return false;
        if (has_count && !count.test(c, s.count())) return false;
        for (auto& e : enchants) {
            auto have = get_enchantments(s);
            bool ok = false;
            if (e.enchantments.empty()) {
                for (auto& kv : have) { if ((!e.has_min || kv.second >= e.min) && (!e.has_max || kv.second <= e.max)) ok = true; }
            } else {
                for (auto& kv : have) {
                    if (!Tags::get().matches("enchantment", e.enchantments, kv.first)) continue;
                    if ((!e.has_min || kv.second >= e.min) && (!e.has_max || kv.second <= e.max)) ok = true;
                }
            }
            if (!ok) return false;
        }
        return true;
    }
    static ItemPredicate parse(const Json& j) {
        ItemPredicate p;
        if (const Json* it = j.find("items")) {
            if (it->is_str()) p.items = it->str;
            else for (auto& e : it->arr) p.items += (p.items.empty() ? "" : ",") + e.str;
        }
        if (const Json* c = j.find("count")) { p.has_count = true; p.count = IntRange::parse(*c); }
        const Json* preds = j.find("predicates");
        if (preds) {
            if (const Json* en = preds->find("minecraft:enchantments"))
                for (auto& e : en->arr) {
                    Ench x;
                    if (const Json* en2 = e.find("enchantments")) {
                        if (en2->is_str()) x.enchantments = en2->str;
                        else for (auto& q : en2->arr) x.enchantments += (x.enchantments.empty() ? "" : ",") + q.str;
                    }
                    if (const Json* lv = e.find("levels")) {
                        if (const Json* m = lv->find("min")) { x.has_min = true; x.min = int(m->int_value()); }
                        if (const Json* m = lv->find("max")) { x.has_max = true; x.max = int(m->int_value()); }
                    }
                    p.enchants.push_back(x);
                }
        }
        return p;
    }
};

// ---- conditions

struct Condition {
    virtual ~Condition() = default;
    virtual bool test(LootContext& c) const = 0;
};
using Cond = std::unique_ptr<Condition>;
Cond parse_condition(const Json& j);
std::vector<Cond> parse_conditions(const Json* arr) {
    std::vector<Cond> out;
    if (arr) for (auto& e : arr->arr) out.push_back(parse_condition(e));
    return out;
}
bool all_of(const std::vector<Cond>& cs, LootContext& c) { for (auto& x : cs) if (!x->test(c)) return false; return true; }

struct SurvivesExplosion : Condition {
    bool test(LootContext& c) const override {
        if (c.params.explosion_radius >= 0) { float p = 1.0f / c.params.explosion_radius; return c.random.next_float() <= p; }
        return true;
    }
};
struct BlockStateProperty : Condition {
    std::string block;
    struct Prop { std::string name, exact, lo, hi; bool ranged = false; };
    std::vector<Prop> props;
    bool test(LootContext& c) const override {
        const LootBlockState* s = c.params.block_state;
        if (!s || s->name != block) return false;
        for (auto& p : props) {
            auto it = s->props.find(p.name);
            if (it == s->props.end()) return false;
            if (!p.ranged) { if (it->second != p.exact) return false; continue; }
            // integer properties compare numerically, others (enums) by their ordering which data packs do not use
            int v = atoi(it->second.c_str());
            if (!p.lo.empty() && v < atoi(p.lo.c_str())) return false;
            if (!p.hi.empty() && v > atoi(p.hi.c_str())) return false;
        }
        return true;
    }
};
struct MatchTool : Condition {
    ItemPredicate pred;
    bool test(LootContext& c) const override { return c.params.tool && pred.matches(c, *c.params.tool); }
};
struct TableBonus : Condition {
    std::string enchantment;
    std::vector<float> chances;
    bool test(LootContext& c) const override {
        int level = c.params.tool ? enchantment_level(*c.params.tool, enchantment) : 0;
        float chance = chances[size_t(std::min<int>(level, int(chances.size()) - 1))];
        return c.random.next_float() < chance;
    }
};
struct RandomChance : Condition {
    NP chance;
    bool test(LootContext& c) const override { return c.random.next_float() < chance->get_float(c); }
};
struct RandomChanceEnchanted : Condition {
    float unenchanted = 0;
    LevelValue enchanted;
    std::string enchantment;
    bool test(LootContext& c) const override {
        int level = 0;
        if (c.params.attacker && c.params.attacker->enchantment_level) level = c.params.attacker->enchantment_level(enchantment);
        float chance = level > 0 ? enchanted.calc(level) : unenchanted;
        return c.random.next_float() < chance;
    }
};
struct Inverted : Condition {
    Cond term;
    bool test(LootContext& c) const override { return !term->test(c); }
};
struct AnyOf : Condition {
    std::vector<Cond> terms;
    bool test(LootContext& c) const override { for (auto& t : terms) if (t->test(c)) return true; return false; }
};
struct AllOf : Condition {
    std::vector<Cond> terms;
    bool test(LootContext& c) const override { return all_of(terms, c); }
};
struct KilledByPlayer : Condition {
    bool test(LootContext& c) const override { return c.params.killed_by_player; }
};
struct EntityProperties : Condition {
    std::string entity = "this";
    std::string type;                                      // predicate.type (id or tag)
    std::map<std::string, bool> flags;
    std::map<std::string, std::string> components;         // minecraft:components values compared as strings (animal variants)
    bool test(LootContext& c) const override {
        const LootEntityInfo* e = entity == "attacker" || entity == "direct_attacker" || entity == "attacking_player" ? c.params.attacker : c.params.this_entity;
        if (!e) return false;
        if (!type.empty() && !Tags::get().matches("entity_type", type, e->type)) return false;
        for (auto& f : flags) {
            bool v = f.first == "is_on_fire" ? e->on_fire : f.first == "is_baby" ? e->baby : f.first == "is_in_water" ? e->in_water : f.first == "is_sneaking" ? e->sneaking : false;
            if (v != f.second) return false;
        }
        for (auto& kv : components) {
            auto it = e->components.find(kv.first);
            if (it == e->components.end() || it->second != kv.second) return false;
        }
        return true;
    }
};
struct DamageSourceProperties : Condition {
    struct Tag { std::string id; bool expected = true; };
    std::vector<Tag> tags;
    bool test(LootContext& c) const override {
        for (auto& t : tags) {
            bool has = false;
            for (auto& s : c.params.damage_source_tags) if (s == t.id) has = true;
            if (has != t.expected) return false;
        }
        return true;
    }
};
struct LocationCheck : Condition {
    int off_x = 0, off_y = 0, off_z = 0;
    std::vector<std::string> biomes;
    bool test(LootContext& c) const override {
        if (!c.params.has_origin) return false;
        if (!biomes.empty()) {
            if (!c.params.level) return false;
            std::string b = c.params.level->biome_at(int(std::floor(c.params.ox)) + off_x, int(std::floor(c.params.oy)) + off_y, int(std::floor(c.params.oz)) + off_z);
            for (auto& x : biomes) if (Tags::get().matches("worldgen/biome", x, b)) return true;
            return false;
        }
        return true;   // block predicates at an offset are checked by the caller's level query when available; treated as passing
    }
};
struct Reference : Condition {
    std::string name;
    bool test(LootContext&) const override { return true; }
};

Cond parse_condition(const Json& j) {
    std::string t = j.get_string("condition");
    if (t.rfind("minecraft:", 0) == 0) t = t.substr(10);
    if (t == "survives_explosion") return std::make_unique<SurvivesExplosion>();
    if (t == "block_state_property") {
        auto c = std::make_unique<BlockStateProperty>();
        c->block = with_ns(j.get_string("block"));
        if (const Json* pr = j.find("properties"))
            for (auto& kv : pr->obj) {
                BlockStateProperty::Prop p;
                p.name = kv.first;
                if (kv.second.is_obj()) { p.ranged = true; p.lo = kv.second.get_string("min"); p.hi = kv.second.get_string("max"); }
                else p.exact = kv.second.is_str() ? kv.second.str : (kv.second.is_bool() ? (kv.second.b ? "true" : "false") : std::to_string(kv.second.int_value()));
                c->props.push_back(p);
            }
        return c;
    }
    if (t == "match_tool") { auto c = std::make_unique<MatchTool>(); c->pred = ItemPredicate::parse(j.at("predicate")); return c; }
    if (t == "table_bonus") {
        auto c = std::make_unique<TableBonus>();
        c->enchantment = with_ns(j.get_string("enchantment"));
        for (auto& v : j.at("chances").arr) c->chances.push_back(v.float_value());
        if (c->chances.empty()) c->chances.push_back(0);
        return c;
    }
    if (t == "random_chance") { auto c = std::make_unique<RandomChance>(); c->chance = parse_np(j.at("chance")); return c; }
    if (t == "random_chance_with_enchanted_bonus") {
        auto c = std::make_unique<RandomChanceEnchanted>();
        c->unenchanted = j.at("unenchanted_chance").float_value();
        c->enchanted = LevelValue::parse(j.at("enchanted_chance"));
        c->enchantment = with_ns(j.get_string("enchantment"));
        return c;
    }
    if (t == "inverted") { auto c = std::make_unique<Inverted>(); c->term = parse_condition(j.at("term")); return c; }
    if (t == "any_of") { auto c = std::make_unique<AnyOf>(); c->terms = parse_conditions(j.find("terms")); return c; }
    if (t == "all_of") { auto c = std::make_unique<AllOf>(); c->terms = parse_conditions(j.find("terms")); return c; }
    if (t == "killed_by_player") return std::make_unique<KilledByPlayer>();
    if (t == "entity_properties") {
        auto c = std::make_unique<EntityProperties>();
        c->entity = j.get_string("entity", "this");
        if (const Json* p = j.find("predicate")) {
            if (const Json* ty = p->find("type")) c->type = ty->is_str() ? ty->str : "";
            if (const Json* fl = p->find("flags")) for (auto& kv : fl->obj) c->flags[kv.first] = kv.second.boolean();
            if (const Json* co = p->find("minecraft:components")) for (auto& kv : co->obj) c->components[kv.first] = kv.second.is_str() ? kv.second.str : "";
        }
        return c;
    }
    if (t == "damage_source_properties") {
        auto c = std::make_unique<DamageSourceProperties>();
        if (const Json* p = j.find("predicate"))
            if (const Json* tg = p->find("tags"))
                for (auto& e : tg->arr) { DamageSourceProperties::Tag x; x.id = with_ns(e.get_string("id")); x.expected = e.get_bool("expected", true); c->tags.push_back(x); }
        return c;
    }
    if (t == "location_check") {
        auto c = std::make_unique<LocationCheck>();
        c->off_x = int(j.get_int("offsetX", 0)); c->off_y = int(j.get_int("offsetY", 0)); c->off_z = int(j.get_int("offsetZ", 0));
        if (const Json* p = j.find("predicate"))
            if (const Json* b = p->find("biomes")) {
                if (b->is_str()) c->biomes.push_back(b->str);
                else for (auto& e : b->arr) c->biomes.push_back(e.str);
            }
        return c;
    }
    if (t == "reference") return std::make_unique<Reference>();
    return std::make_unique<Reference>();   // unknown: passes (data packs of newer versions)
}

// ---- functions

struct Function {
    std::vector<Cond> conditions;
    virtual ~Function() = default;
    ItemStack apply(ItemStack s, LootContext& c) const {
        if (!all_of(conditions, c)) return s;
        return run(std::move(s), c);
    }
    virtual ItemStack run(ItemStack s, LootContext& c) const = 0;
};
using Fn = std::unique_ptr<Function>;
Fn parse_function(const Json& j);

struct SetCount : Function {
    NP count; bool add = false;
    ItemStack run(ItemStack s, LootContext& c) const override { int base = add ? s.count() : 0; s.set_count(base + count->get_int(c)); return s; }
};
struct ExplosionDecay : Function {
    ItemStack run(ItemStack s, LootContext& c) const override {
        if (c.params.explosion_radius >= 0) {
            float p = 1.0f / c.params.explosion_radius;
            int cur = s.count(), r = 0;
            for (int i = 0; i < cur; i++) if (c.random.next_float() <= p) ++r;
            s.set_count(r);
        }
        return s;
    }
};
struct LimitCountFn : Function {
    IntRange limit;
    ItemStack run(ItemStack s, LootContext& c) const override { s.set_count(limit.clamp(c, s.count())); return s; }
};
struct EnchantedCountIncrease : Function {
    std::string enchantment; NP count; int limit = 0;
    ItemStack run(ItemStack s, LootContext& c) const override {
        if (c.params.attacker && c.params.attacker->enchantment_level) {
            int level = c.params.attacker->enchantment_level(enchantment);
            if (level == 0) return s;
            float add = float(level) * count->get_float(c);
            s.grow(int(std::floor(add + 0.5f)));
            if (limit > 0) s.limit_size(limit);
        }
        return s;
    }
};
struct ApplyBonus : Function {
    std::string enchantment, formula;
    int multiplier = 1, extra = 0;
    float probability = 0;
    ItemStack run(ItemStack s, LootContext& c) const override {
        if (!c.params.tool) return s;
        int level = enchantment_level(*c.params.tool, enchantment);
        int count = s.count();
        if (formula == "minecraft:uniform_bonus_count") count = count + c.random.next_int(multiplier * level + 1);
        else if (formula == "minecraft:ore_drops") {
            if (level > 0) { int bonus = c.random.next_int(level + 2) - 1; if (bonus < 0) bonus = 0; count = count * (bonus + 1); }
        } else if (formula == "minecraft:binomial_with_bonus_count") {
            for (int i = 0; i < level + extra; i++) if (c.random.next_float() < probability) ++count;
        }
        s.set_count(count);
        return s;
    }
};
struct SetDamage : Function {
    NP damage; bool add = false;
    ItemStack run(ItemStack s, LootContext& c) const override {
        if (s.damageable()) {
            int max = s.max_damage();
            float base = add ? 1.0f - float(s.damage_value()) / float(max) : 0.0f;
            float v = damage->get_float(c) + base;
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            float pct = 1.0f - v;
            s.set_damage_value(int(std::floor(pct * float(max))));
        }
        return s;
    }
};
struct SetPotion : Function {
    std::string potion;
    ItemStack run(ItemStack s, LootContext&) const override { set_potion(s, potion); return s; }
};
struct SetName : Function {
    Json name; bool has = false; bool item_name = false;
    ItemStack run(ItemStack s, LootContext&) const override {
        if (has) set_text_component(s, item_name ? "minecraft:item_name" : "minecraft:custom_name", name);
        return s;
    }
};
struct SetOminous : Function {
    NP amplifier;
    ItemStack run(ItemStack s, LootContext& c) const override {
        int a = amplifier->get_int(c);
        a = a < 0 ? 0 : a > 4 ? 4 : a;
        set_varint_component(s, "minecraft:ominous_bottle_amplifier", a);
        return s;
    }
};
struct SetInstrument : Function {
    std::string options;
    ItemStack run(ItemStack s, LootContext& c) const override {
        std::vector<std::string> list = options[0] == '#' ? Tags::get().ordered("instrument", options) : std::vector<std::string>{with_ns(options)};
        if (list.empty()) return s;
        std::string pick = list[size_t(c.random.next_int(int(list.size())))];
        int id = Registries::get().id("minecraft:instrument", pick);
        if (id >= 0) {
            // InstrumentComponent: a holder (id + 1; 0 would mean a direct value)
            std::vector<uint8_t> b;
            put_varint_bytes(b, id + 1);
            s.patch().added[Registries::get().id("minecraft:data_component_type", "minecraft:instrument")] = std::move(b);
        }
        return s;
    }
};
struct SetStew : Function {
    struct E { std::string type; NP duration; };
    std::vector<E> effects;
    ItemStack run(ItemStack s, LootContext& c) const override {
        if (!s.is("minecraft:suspicious_stew") || effects.empty()) return s;
        const E& e = effects[size_t(c.random.next_int(int(effects.size())))];
        int duration = e.duration->get_int(c);
        static const std::set<std::string> instant = {"minecraft:instant_health", "minecraft:instant_damage", "minecraft:saturation"};
        if (!instant.count(e.type)) duration *= 20;
        int id = Registries::get().id("minecraft:mob_effect", e.type);
        if (id >= 0) {
            std::vector<uint8_t> b;
            put_varint_bytes(b, 1);
            put_varint_bytes(b, id);
            put_varint_bytes(b, duration);
            s.patch().added[Registries::get().id("minecraft:data_component_type", "minecraft:suspicious_stew_effects")] = std::move(b);
        }
        return s;
    }
};
struct CopyState : Function {
    std::string block;
    std::vector<std::string> properties;
    ItemStack run(ItemStack s, LootContext& c) const override {
        const LootBlockState* st = c.params.block_state;
        if (!st) return s;
        // BlockItemStateProperties: a map of property name to value
        std::vector<std::pair<std::string, std::string>> m;
        for (auto& p : properties) { auto it = st->props.find(p); if (it != st->props.end()) m.emplace_back(p, it->second); }
        if (m.empty()) return s;
        std::vector<uint8_t> b;
        put_varint_bytes(b, int(m.size()));
        for (auto& kv : m) {
            put_varint_bytes(b, int(kv.first.size())); b.insert(b.end(), kv.first.begin(), kv.first.end());
            put_varint_bytes(b, int(kv.second.size())); b.insert(b.end(), kv.second.begin(), kv.second.end());
        }
        s.patch().added[Registries::get().id("minecraft:data_component_type", "minecraft:block_state")] = std::move(b);
        return s;
    }
};
struct Unsupported : Function {
    std::string name;
    ItemStack run(ItemStack s, LootContext&) const override { return s; }
};

Fn parse_function(const Json& j) {
    std::string t = j.get_string("function");
    if (t.rfind("minecraft:", 0) == 0) t = t.substr(10);
    Fn f;
    if (t == "set_count") { auto x = std::make_unique<SetCount>(); x->count = parse_np(j.at("count")); x->add = j.get_bool("add", false); f = std::move(x); }
    else if (t == "explosion_decay") f = std::make_unique<ExplosionDecay>();
    else if (t == "limit_count") { auto x = std::make_unique<LimitCountFn>(); x->limit = IntRange::parse(j.at("limit")); f = std::move(x); }
    else if (t == "enchanted_count_increase") {
        auto x = std::make_unique<EnchantedCountIncrease>();
        x->enchantment = with_ns(j.get_string("enchantment"));
        x->count = parse_np(j.at("count"));
        x->limit = int(j.get_int("limit", 0));
        f = std::move(x);
    } else if (t == "apply_bonus") {
        auto x = std::make_unique<ApplyBonus>();
        x->enchantment = with_ns(j.get_string("enchantment"));
        x->formula = with_ns(j.get_string("formula"));
        if (const Json* pa = j.find("parameters")) {
            x->multiplier = int(pa->get_int("bonusMultiplier", 1));
            x->extra = int(pa->get_int("extra", 0));
            x->probability = pa->find("probability") ? pa->at("probability").float_value() : 0;
        }
        f = std::move(x);
    } else if (t == "set_damage") { auto x = std::make_unique<SetDamage>(); x->damage = parse_np(j.at("damage")); x->add = j.get_bool("add", false); f = std::move(x); }
    else if (t == "set_potion") { auto x = std::make_unique<SetPotion>(); x->potion = j.get_string("id"); f = std::move(x); }
    else if (t == "set_name") {
        auto x = std::make_unique<SetName>();
        if (const Json* n = j.find("name")) { x->name = *n; x->has = true; }
        x->item_name = j.get_string("target", "custom_name") == "item_name";
        f = std::move(x);
    } else if (t == "set_ominous_bottle_amplifier") { auto x = std::make_unique<SetOminous>(); x->amplifier = parse_np(j.at("amplifier")); f = std::move(x); }
    else if (t == "set_instrument") { auto x = std::make_unique<SetInstrument>(); const Json& o = j.at("options"); x->options = o.is_str() ? o.str : (o.arr.empty() ? "" : o.arr[0].str); f = std::move(x); }
    else if (t == "set_stew_effect") {
        auto x = std::make_unique<SetStew>();
        if (const Json* ef = j.find("effects")) for (auto& e : ef->arr) { SetStew::E en; en.type = with_ns(e.get_string("type")); en.duration = parse_np(e.at("duration")); x->effects.push_back(std::move(en)); }
        f = std::move(x);
    } else if (t == "copy_state") {
        auto x = std::make_unique<CopyState>();
        x->block = with_ns(j.get_string("block"));
        if (const Json* pr = j.find("properties")) for (auto& e : pr->arr) x->properties.push_back(e.str);
        f = std::move(x);
    } else { auto x = std::make_unique<Unsupported>(); x->name = t; f = std::move(x); }
    f->conditions = parse_conditions(j.find("conditions"));
    return f;
}
std::vector<Fn> parse_functions(const Json* arr) {
    std::vector<Fn> out;
    if (arr) for (auto& e : arr->arr) out.push_back(parse_function(e));
    return out;
}
ItemStack apply_functions(const std::vector<Fn>& fs, ItemStack s, LootContext& c) {
    for (auto& f : fs) s = f->apply(std::move(s), c);
    return s;
}

// ---- entries

using Out = std::vector<ItemStack>;

struct Expanded;                       // one entry that can be rolled
struct Entry {
    std::vector<Cond> conditions;
    virtual ~Entry() = default;
    bool can_run(LootContext& c) const { return all_of(conditions, c); }
    // adds the rollable entries (LootPoolEntryContainer.expand); false when it could not run
    virtual bool expand(LootContext& c, std::vector<const Entry*>& out) const = 0;
    virtual int weight(float luck) const { return 1; }
    virtual void create(Out& out, LootContext& c) const = 0;
};
using Ent = std::unique_ptr<Entry>;
Ent parse_entry(const Json& j);

struct Singleton : Entry {
    int w = 1, quality = 0;
    std::vector<Fn> functions;
    int weight(float luck) const override { int v = int(std::floor(float(w) + float(quality) * luck)); return v < 0 ? 0 : v; }
    bool expand(LootContext& c, std::vector<const Entry*>& out) const override { if (can_run(c)) { out.push_back(this); return true; } return false; }
    virtual void create_stack(Out& out, LootContext& c) const = 0;
    void create(Out& out, LootContext& c) const override {
        Out tmp;
        create_stack(tmp, c);
        for (auto& s : tmp) out.push_back(apply_functions(functions, std::move(s), c));
    }
};
struct ItemEntry : Singleton {
    const ItemType* item = nullptr;
    void create_stack(Out& out, LootContext&) const override { if (item) out.push_back(ItemStack(item, 1)); }
};
struct EmptyEntry : Singleton {
    void create_stack(Out&, LootContext&) const override {}
};
struct TagEntry : Singleton {
    std::string tag; bool expand_members = true;
    void create_stack(Out& out, LootContext&) const override {
        for (auto& id : Tags::get().ordered("item", tag)) if (const ItemType* t = Items::get().by_name(id)) out.push_back(ItemStack(t, 1));
    }
};
struct NestedTable : Singleton {
    std::string table;
    void create_stack(Out& out, LootContext& c) const override {
        const LootTable* t = LootTables::get(table);
        if (t) t->get_random_items_raw(c, out);
    }
};
struct Alternatives : Entry {
    std::vector<Ent> children;
    bool expand(LootContext& c, std::vector<const Entry*>& out) const override {
        if (!can_run(c)) return false;
        for (auto& ch : children) if (ch->expand(c, out)) return true;
        return false;
    }
    void create(Out&, LootContext&) const override {}
};
struct Group : Entry {
    std::vector<Ent> children;
    bool expand(LootContext& c, std::vector<const Entry*>& out) const override {
        if (!can_run(c)) return false;
        for (auto& ch : children) ch->expand(c, out);
        return true;
    }
    void create(Out&, LootContext&) const override {}
};
struct Sequence : Entry {
    std::vector<Ent> children;
    bool expand(LootContext& c, std::vector<const Entry*>& out) const override {
        if (!can_run(c)) return false;
        for (auto& ch : children) if (!ch->expand(c, out)) return true;
        return true;
    }
    void create(Out&, LootContext&) const override {}
};

Ent parse_entry(const Json& j) {
    std::string t = j.get_string("type");
    if (t.rfind("minecraft:", 0) == 0) t = t.substr(10);
    Ent e;
    auto singleton_fields = [&](Singleton* s) {
        s->w = int(j.get_int("weight", 1));
        s->quality = int(j.get_int("quality", 0));
        s->functions = parse_functions(j.find("functions"));
    };
    if (t == "item") { auto x = std::make_unique<ItemEntry>(); singleton_fields(x.get()); x->item = Items::get().by_name(with_ns(j.get_string("name"))); e = std::move(x); }
    else if (t == "empty") { auto x = std::make_unique<EmptyEntry>(); singleton_fields(x.get()); e = std::move(x); }
    else if (t == "tag") { auto x = std::make_unique<TagEntry>(); singleton_fields(x.get()); x->tag = j.get_string("name"); e = std::move(x); }
    else if (t == "loot_table") { auto x = std::make_unique<NestedTable>(); singleton_fields(x.get()); const Json& v = j.at("value"); x->table = v.is_str() ? with_ns(v.str) : ""; e = std::move(x); }
    else if (t == "alternatives" || t == "group" || t == "sequence") {
        std::vector<Ent> ch;
        if (const Json* c = j.find("children")) for (auto& k : c->arr) ch.push_back(parse_entry(k));
        if (t == "alternatives") { auto x = std::make_unique<Alternatives>(); x->children = std::move(ch); e = std::move(x); }
        else if (t == "group") { auto x = std::make_unique<Group>(); x->children = std::move(ch); e = std::move(x); }
        else { auto x = std::make_unique<Sequence>(); x->children = std::move(ch); e = std::move(x); }
    } else { auto x = std::make_unique<EmptyEntry>(); singleton_fields(x.get()); e = std::move(x); }   // dynamic: the contents of a block entity, not rolled here
    e->conditions = parse_conditions(j.find("conditions"));
    return e;
}

struct Pool {
    std::vector<Ent> entries;
    std::vector<Cond> conditions;
    std::vector<Fn> functions;
    NP rolls, bonus_rolls;
    void add_random_item(Out& out, LootContext& c) const {
        std::vector<const Entry*> valid;
        int total = 0;
        for (auto& e : entries) {
            std::vector<const Entry*> ex;
            e->expand(c, ex);
            for (auto* x : ex) {
                int w = x->weight(c.params.luck);
                if (w > 0) { valid.push_back(x); total += w; }
            }
        }
        if (total == 0 || valid.empty()) return;
        auto emit = [&](const Entry* x) { Out tmp; x->create(tmp, c); for (auto& s : tmp) out.push_back(apply_functions(functions, std::move(s), c)); };
        if (valid.size() == 1) { emit(valid[0]); return; }
        int index = c.random.next_int(total);
        for (auto* x : valid) {
            if ((index -= x->weight(c.params.luck)) >= 0) continue;
            emit(x);
            return;
        }
    }
    void add_random_items(Out& out, LootContext& c) const {
        if (!all_of(conditions, c)) return;
        int count = rolls->get_int(c) + int(std::floor(bonus_rolls->get_float(c) * c.params.luck));
        for (int i = 0; i < count; i++) add_random_item(out, c);
    }
};

}  // namespace

struct LootTable::Impl {
    std::vector<Pool> pools;
    std::vector<Fn> functions;
};

std::unique_ptr<LootTable> LootTable::parse(const Json& j) {
    auto t = std::make_unique<LootTable>();
    t->impl = std::make_unique<Impl>();
    if (const Json* pools = j.find("pools"))
        for (auto& pj : pools->arr) {
            Pool p;
            for (auto& e : pj.at("entries").arr) p.entries.push_back(parse_entry(e));
            p.conditions = parse_conditions(pj.find("conditions"));
            p.functions = parse_functions(pj.find("functions"));
            p.rolls = parse_np(pj.at("rolls"));
            p.bonus_rolls = pj.find("bonus_rolls") ? parse_np(pj.at("bonus_rolls")) : std::make_unique<Constant>(0);
            t->impl->pools.push_back(std::move(p));
        }
    t->impl->functions = parse_functions(j.find("functions"));
    return t;
}

// LootTable.getRandomItemsRaw
void LootTable::get_random_items_raw(LootContext& ctx, std::vector<ItemStack>& out) const {
    if (!ctx.visited.insert(this).second) return;   // an infinite loop of nested tables
    std::vector<ItemStack> mine;
    for (auto& pool : impl->pools) pool.add_random_items(mine, ctx);
    for (auto& s : mine) out.push_back(apply_functions(impl->functions, std::move(s), ctx));
    ctx.visited.erase(this);
}

void LootTable::random_items_split(LootContext& ctx, std::vector<ItemStack>& out) const {
    std::vector<ItemStack> raw;
    get_random_items_raw(ctx, raw);
    // createStackSplitter
    for (auto& r : raw) {
        if (r.count() < r.max_stack_size()) { out.push_back(r); continue; }
        for (int count = r.count(); count > 0;) {
            ItemStack copy = r.copy_with_count(std::min(r.max_stack_size(), count));
            count -= copy.count();
            out.push_back(copy);
        }
    }
}

void LootTable::get_random_items(const LootParams& params, int64_t seed, std::vector<ItemStack>& out) const {
    std::unique_ptr<vg::LegacyRandom> seeded;
    vg::Rng* rng = params.level_random;
    if (seed != 0) { seeded = std::make_unique<vg::LegacyRandom>(seed); rng = seeded.get(); }
    static vg::LegacyRandom fallback(0x5DEECE66DLL);
    if (!rng) rng = &fallback;
    LootContext ctx(params, *rng);
    random_items_split(ctx, out);
}

static void shuffle_list(std::vector<int>& list, vg::Rng& random) {
    for (int i = int(list.size()); i > 1; --i) std::swap(list[size_t(i - 1)], list[size_t(random.next_int(i))]);
}

void LootTable::shuffle_and_split(std::vector<ItemStack>& result, int available, vg::Rng& random) {
    std::vector<ItemStack> splittable;
    for (auto it = result.begin(); it != result.end();) {
        if (it->is_empty()) { it = result.erase(it); continue; }
        if (it->count() <= 1) { ++it; continue; }
        splittable.push_back(*it);
        it = result.erase(it);
    }
    while (available - int(result.size()) - int(splittable.size()) > 0 && !splittable.empty()) {
        size_t idx = size_t(nextInt3(random, 0, int(splittable.size()) - 1));
        ItemStack stack = splittable[idx];
        splittable.erase(splittable.begin() + long(idx));
        int remove = nextInt3(random, 1, stack.count() / 2);
        ItemStack copy = stack.split(remove);
        if (stack.count() > 1 && random.next_boolean()) splittable.push_back(stack);
        else result.push_back(stack);
        if (copy.count() > 1 && random.next_boolean()) { splittable.push_back(copy); continue; }
        result.push_back(copy);
    }
    for (auto& s : splittable) result.push_back(s);
    for (int i = int(result.size()); i > 1; --i) std::swap(result[size_t(i - 1)], result[size_t(random.next_int(i))]);
}

void LootTable::fill(Container& container, const LootParams& params, int64_t seed) const {
    std::unique_ptr<vg::LegacyRandom> seeded;
    vg::Rng* rng = params.level_random;
    if (seed != 0) { seeded = std::make_unique<vg::LegacyRandom>(seed); rng = seeded.get(); }
    static vg::LegacyRandom fallback(0x5DEECE66DLL);
    if (!rng) rng = &fallback;
    LootContext ctx(params, *rng);
    std::vector<ItemStack> items;
    random_items_split(ctx, items);
    std::vector<int> slots;
    for (int i = 0; i < container.size(); i++) if (container.item(i).is_empty()) slots.push_back(i);
    shuffle_list(slots, *rng);
    shuffle_and_split(items, int(slots.size()), *rng);
    for (auto& s : items) {
        if (slots.empty()) return;
        int slot = slots.back();
        slots.pop_back();
        container.set_item(slot, s.is_empty() ? ItemStack() : s);
    }
}

// ---- the loot table registry

namespace {
std::mutex g_mu;
std::string g_dir;
std::map<std::string, std::unique_ptr<LootTable>> g_cache;
}

void LootTables::set_dir(const std::string& d) { std::lock_guard<std::mutex> l(g_mu); g_dir = d; g_cache.clear(); }
const LootTable* LootTables::get(const std::string& id) {
    std::lock_guard<std::mutex> l(g_mu);
    std::string key = with_ns(id);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second.get();
    std::unique_ptr<LootTable> t;
    try {
        Json j = Json::parse_file(g_dir + "/data/minecraft/loot_table/" + key.substr(10) + ".json");
        t = LootTable::parse(j);
    } catch (const std::exception&) {}
    return g_cache.emplace(key, std::move(t)).first->second.get();
}

}  // namespace pn::sim
