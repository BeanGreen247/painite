// Decodes the level_chunk_with_light packet body (sections, block entities, light structure) for the comparison tests.
#pragma once
#include "util.hpp"
#include <map>
#include <string>
#include <vector>

namespace pt {

struct SectionData {
    int nonair = 0, fluid = 0;
    std::vector<int> blocks;   // 4096 protocol state ids
    std::vector<int> biomes;   // 64
};

struct ChunkData {
    int x = 0, z = 0;
    std::vector<SectionData> sections;
    Bytes rest;                // block entities and light
    bool ok = false;
};

struct TailInfo {
    int block_entities = 0;
    uint64_t masks[4] = {0, 0, 0, 0};
    int arrays[2] = {0, 0};
    bool exact = false;
    std::vector<std::pair<int, int>> entities;   // (block entity type, y)
};

ChunkData parse_chunk(const Bytes& d);
TailInfo parse_tail(const Bytes& rest);
// block state names by protocol id from data/vanilla-<ver>.blocks: "minecraft:stone", with props "name[k=v]" when `with_props`
std::map<int, std::string> load_block_names(const std::string& path, bool with_props);

}  // namespace pt
