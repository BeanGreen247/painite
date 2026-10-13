// MD5 (offline UUIDs) and Minecraft's signed-hex SHA-1 digest format (session server serverId).
#pragma once
#include <cstdint>
#include <string>

namespace pn {
void md5(const std::string& in, uint8_t out[16]);
// Offline-mode UUID exactly as Spigot/Paper compute it: UUID.nameUUIDFromBytes("OfflinePlayer:" + name).
void offline_uuid(const std::string& name, uint8_t out[16]);
// SHA-1 bytes interpreted as a signed big-endian integer, printed in hex without leading zeros.
std::string mc_hex_digest(const uint8_t sha1[20]);
}  // namespace pn
