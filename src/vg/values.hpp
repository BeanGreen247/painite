// Mojang's value providers read straight from their JSON: int, float and height providers, vertical anchors.
#pragma once
#include "json.hpp"
#include "random.hpp"

namespace pn::vg {

// Height range of the thing being generated (WorldGenerationContext): anchors resolve against it
struct GenRange {
    int min_y = -64, height = 384;
    int sea_level = 63;   // for relative_to_sea_level anchors (26.3)
};

int resolve_anchor(const Json& anchor, const GenRange& r);                   // {"absolute": n} | {"above_bottom": n} | {"below_top": n}
int sample_int(const Json& provider, Rng& random);                           // number or {"type": ...}
float sample_float(const Json& provider, Rng& random);
int sample_height(const Json& provider, Rng& random, const GenRange& r);
// WeightedList.getRandomOrThrow over a JSON list of {"data": ..., "weight": n}
const Json& pick_weighted(const Json& list, Rng& random);
int int_provider_min(const Json& provider);                                  // minInclusive()
int int_provider_max(const Json& provider);                                  // maxInclusive()

}  // namespace pn::vg
