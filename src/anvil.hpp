// Anvil region files (.mca): read and write chunk NBT with zlib, gzip, none and LZ4 (Paper) compression.
#pragma once
#include "nbt.hpp"
#include <string>
#include <vector>

namespace pn {

// Decompresses gzip or zlib data.
bool inflate_any(const uint8_t* p, size_t n, std::vector<uint8_t>& out, size_t cap = 256u << 20);
bool gzip_compress(const uint8_t* p, size_t n, std::vector<uint8_t>& out);

// Reads one chunk's root NBT. Returns false if the chunk is absent or unreadable.
bool anvil_read(const std::string& region_dir, int cx, int cz, Nbt& root);
// Writes (or replaces) one chunk in r.<rx>.<rz>.mca, creating the directory and file as needed.
bool anvil_write(const std::string& region_dir, int cx, int cz, const Nbt& root);
bool read_nbt_file(const std::string& path, Nbt& root);          // gzip level.dat / playerdata
bool write_nbt_file(const std::string& path, const Nbt& root);   // gzip, via temp file + rename
bool region_dir_has_chunks(const std::string& region_dir);

}  // namespace pn
