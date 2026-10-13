// Verification of Bedrock's Xbox Live login token (an RS256 JWT from Minecraft's authorization service).
#pragma once
#include "auth.hpp"
#include <string>

namespace pn {

struct XboxResult {
    bool ok = false;
    std::string xuid, gamertag, error;
};

// Blocking (HTTPS for the signing keys, cached): call on an auth thread. client_pub_b64 is the x5u of the client's own JWT;
// the token must be bound to that key through its cpk claim.
XboxResult xbox_verify(const Auth& auth, const std::string& token, const std::string& client_pub_b64);

}  // namespace pn
