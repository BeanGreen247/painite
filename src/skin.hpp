// Skins: PNG decoding for Java skins, the Bedrock Skin structure for the player list, and a plain default skin.
#pragma once
#include "json.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace pn {

// Decodes a non-interlaced 8 bit PNG to RGBA. Returns false on anything else.
bool decode_png(const uint8_t* p, size_t n, int& w, int& h, std::vector<uint8_t>& rgba);
// 64x32 legacy skins become 64x64 (left limbs copied from the right ones).
void upgrade_legacy_skin(int& w, int& h, std::vector<uint8_t>& rgba);
// Mojang "textures" property value (base64 JSON) -> skin URL path (/texture/<hash>) and model; false if there is no skin.
bool parse_textures_property(const std::string& value_b64, std::string& host, std::string& path, bool& slim);

std::vector<uint8_t> default_skin_rgba(bool alex);   // 64x64
// Bedrock "Skin" structure of a classic (64x64, standard geometry) skin.
std::vector<uint8_t> bedrock_classic_skin(const std::vector<uint8_t>& rgba64, bool slim, const std::string& skin_id);
// Bedrock Skin structure rebuilt from the client data JWT of a Bedrock player (keeps persona skins, capes and custom geometry).
bool bedrock_skin_from_client(const JV& client_data, std::vector<uint8_t>& out);
// The structure inside a recorded PlayerList entry: byte range [begin, end) of the skin starting at `begin`.
bool bedrock_skin_extent(const uint8_t* p, const uint8_t* e, const uint8_t*& end_out);

}  // namespace pn
