#pragma once
#include "util.hpp"
#include "world.hpp"

namespace pn {
// Appends x, z, heightmaps, sections, block entities and light data (the packet payload after the id).
void encode_java_chunk(Chunk& c, const VanillaData& vd, const std::vector<std::string>& biome_names, Wr& out, int dim = 0);
}
