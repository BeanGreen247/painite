#include "values.hpp"
#include <algorithm>
#include <stdexcept>

namespace pn::vg {

namespace {

inline int rand_inclusive(Rng& r, int lo, int hi) { return r.next_int(hi - lo + 1) + lo; }
inline int mth_next_int(Rng& r, int lo, int hi) { return lo >= hi ? lo : r.next_int(hi - lo + 1) + lo; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : std::min(v, hi); }

std::string type_of(const Json& j) {
    std::string t = j.at("type").str;
    return t.rfind("minecraft:", 0) == 0 ? t.substr(10) : t;
}

// WeightedList: entries {"data": ..., "weight": n}
const Json& pick_weighted(const Json& list, Rng& random) {
    int total = 0;
    for (auto& e : list.arr) total += int(e.get_int("weight", 1));
    int sel = random.next_int(total);
    for (auto& e : list.arr) {
        sel -= int(e.get_int("weight", 1));
        if (sel < 0) return e.at("data");
    }
    return list.arr.back().at("data");
}

}  // namespace

int resolve_anchor(const Json& a, const GenRange& r) {
    if (const Json* v = a.find("absolute")) return int(v->int_value());
    if (const Json* v = a.find("above_bottom")) return r.min_y + int(v->int_value());
    if (const Json* v = a.find("below_top")) return r.height - 1 + r.min_y - int(v->int_value());
    throw std::runtime_error("bad vertical anchor");
}

int sample_int(const Json& p, Rng& random) {
    if (p.is_num()) return int(p.int_value());
    std::string t = type_of(p);
    if (t == "constant") return int(p.at("value").int_value());
    if (t == "uniform") return rand_inclusive(random, int(p.at("min_inclusive").int_value()), int(p.at("max_inclusive").int_value()));
    if (t == "biased_to_bottom") {
        int lo = int(p.at("min_inclusive").int_value()), hi = int(p.at("max_inclusive").int_value());
        return lo + random.next_int(random.next_int(hi - lo + 1) + 1);
    }
    if (t == "clamped") {
        int v = sample_int(p.at("source"), random);
        return std::min(std::max(v, int(p.at("min_inclusive").int_value())), int(p.at("max_inclusive").int_value()));
    }
    if (t == "clamped_normal") {
        float mean = p.at("mean").float_value(), dev = p.at("deviation").float_value();
        float lo = float(p.at("min_inclusive").int_value()), hi = float(p.at("max_inclusive").int_value());
        float n = mean + float(random.next_gaussian()) * dev;
        return int(clampf(n, lo, hi));
    }
    if (t == "weighted_list") return sample_int(pick_weighted(p.at("distribution"), random), random);
    if (t == "trapezoid") {
        int lo = int(p.at("min").int_value()), hi = int(p.at("max").int_value()), plateau = int(p.at("plateau").int_value());
        if (plateau == 0 && hi == -lo) { int a = random.next_int(hi + 1); int b = random.next_int(hi + 1); return a - b; }
        int range = hi - lo;
        if (plateau == range) return rand_inclusive(random, lo, hi);
        int ps = (range - plateau) / 2, pe = range - ps;
        int a = rand_inclusive(random, 0, pe);
        int b = rand_inclusive(random, 0, ps);
        return lo + a + b;
    }
    throw std::runtime_error("unknown int provider " + t);
}

int int_provider_min(const Json& p) {
    if (p.is_num()) return int(p.int_value());
    std::string t = type_of(p);
    if (t == "constant") return int(p.at("value").int_value());
    if (t == "uniform" || t == "biased_to_bottom" ) return int(p.at("min_inclusive").int_value());
    if (t == "clamped") return std::max(int(p.at("min_inclusive").int_value()), int_provider_min(p.at("source")));
    if (t == "clamped_normal") return int(p.at("min_inclusive").int_value());
    if (t == "trapezoid") return int(p.at("min").int_value());
    if (t == "weighted_list") {
        int m = INT32_MAX;
        for (auto& e : p.at("distribution").arr) m = std::min(m, int_provider_min(e.at("data")));
        return m;
    }
    throw std::runtime_error("unknown int provider " + t);
}

int int_provider_max(const Json& p) {
    if (p.is_num()) return int(p.int_value());
    std::string t = type_of(p);
    if (t == "constant") return int(p.at("value").int_value());
    if (t == "uniform" || t == "biased_to_bottom") return int(p.at("max_inclusive").int_value());
    if (t == "clamped") return std::min(int(p.at("max_inclusive").int_value()), int_provider_max(p.at("source")));
    if (t == "clamped_normal") return int(p.at("max_inclusive").int_value());
    if (t == "trapezoid") return int(p.at("max").int_value());
    if (t == "weighted_list") {
        int m = INT32_MIN;
        for (auto& e : p.at("distribution").arr) m = std::max(m, int_provider_max(e.at("data")));
        return m;
    }
    throw std::runtime_error("unknown int provider " + t);
}

float sample_float(const Json& p, Rng& random) {
    if (p.is_num()) return p.float_value();
    std::string t = type_of(p);
    if (t == "constant") return p.at("value").float_value();
    if (t == "uniform") {
        float lo = p.at("min_inclusive").float_value(), hi = p.at("max_exclusive").float_value();
        return random.next_float() * (hi - lo) + lo;
    }
    if (t == "trapezoid") {
        float lo = p.at("min").float_value(), hi = p.at("max").float_value(), plateau = p.at("plateau").float_value();
        float range = hi - lo;
        float ps = (range - plateau) / 2.0f;
        float pe = range - ps;
        float a = random.next_float();
        float b = random.next_float();
        return lo + a * pe + b * ps;
    }
    if (t == "clamped_normal") {
        float mean = p.at("mean").float_value(), dev = p.at("deviation").float_value(), lo = p.at("min").float_value(), hi = p.at("max").float_value();
        return clampf(mean + float(random.next_gaussian()) * dev, lo, hi);
    }
    throw std::runtime_error("unknown float provider " + t);
}

int sample_height(const Json& p, Rng& random, const GenRange& r) {
    if (p.is_obj() && !p.has("type")) return resolve_anchor(p, r);   // a bare anchor is a constant height
    std::string t = type_of(p);
    if (t == "constant") return resolve_anchor(p.at("value"), r);
    if (t == "uniform") {
        int lo = resolve_anchor(p.at("min_inclusive"), r), hi = resolve_anchor(p.at("max_inclusive"), r);
        return lo > hi ? lo : rand_inclusive(random, lo, hi);
    }
    if (t == "biased_to_bottom") {
        int lo = resolve_anchor(p.at("min_inclusive"), r), hi = resolve_anchor(p.at("max_inclusive"), r), inner = int(p.get_int("inner", 1));
        if (hi - lo - inner + 1 <= 0) return lo;
        int limit = random.next_int(hi - lo - inner + 1);
        return random.next_int(limit + inner) + lo;
    }
    if (t == "very_biased_to_bottom") {
        int lo = resolve_anchor(p.at("min_inclusive"), r), hi = resolve_anchor(p.at("max_inclusive"), r), inner = int(p.get_int("inner", 1));
        if (hi - lo - inner + 1 <= 0) return lo;
        int upper = mth_next_int(random, lo + inner, hi);
        int biased = mth_next_int(random, lo, upper - 1);
        return mth_next_int(random, lo, biased - 1 + inner);
    }
    if (t == "trapezoid") {
        int lo = resolve_anchor(p.at("min_inclusive"), r), hi = resolve_anchor(p.at("max_inclusive"), r), plateau = int(p.get_int("plateau", 0));
        if (lo > hi) return lo;
        int range = hi - lo;
        if (plateau >= range) return rand_inclusive(random, lo, hi);
        int ps = (range - plateau) / 2, pe = range - ps;
        int a = rand_inclusive(random, 0, pe);
        int b = rand_inclusive(random, 0, ps);
        return lo + a + b;
    }
    if (t == "weighted_list") return sample_height(pick_weighted(p.at("distribution"), random), random, r);
    throw std::runtime_error("unknown height provider " + t);
}

}  // namespace pn::vg
