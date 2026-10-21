// Bedrock chunk data from world chunks: LevelChunk payload (biomes) and SubChunk entries (block layers + heightmaps).
#pragma once
#include "world.hpp"
#include <vector>

namespace pn {

// Bedrock biome ids by World::biome_names index (loaded from data/bedrock-biomes.txt, plains when unknown).
void bedrock_set_biomes(const std::vector<std::string>& java_names, const std::string& biome_file);

// Tail of a LevelChunk packet after the x and z varints (dimension, sub chunk request mode, biomes).
void encode_bedrock_levelchunk(Chunk& c, std::vector<uint8_t>& tail, int dim = 0);
// SubChunk entry (after the 3 offset bytes) for world section index 0..23.
// bedrock_protocol selects the block palette of the client's version.
void encode_bedrock_subchunk(Chunk& c, int section, std::vector<uint8_t>& out, int bedrock_protocol);

}  // namespace pn
