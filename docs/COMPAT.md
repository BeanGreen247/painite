# Client compatibility notes

How comparable servers handle official (authenticated) and cracked (offline) clients, and what Painite does.

## Findings

- **Cuberite** (C++): `src/Protocol/Authenticator.cpp` keeps a queue served by a dedicated thread that calls
  `sessionserver.mojang.com/session/minecraft/hasJoined`. A setting (`[Authentication] Authenticate`) turns it off;
  with it off the client's name is accepted as-is and the UUID comes from `cClientHandle::GenerateOfflineUUID`, a
  version-3 UUID of `"OfflinePlayer:" + name`.
- **Spigot / Paper**: the vanilla `online-mode` property in `server.properties` is the switch. Offline UUIDs use the same
  `OfflinePlayer:<name>` version-3 scheme. Painite's output for `Notch` (`b50ad385-829d-3141-a216-7e7d7539ba7f`) is the
  well-known value those servers produce, checked in `tests/unit.cpp`.
- A server is either online or offline at a time; cracked and authenticated clients do not share one mode.

## What Painite does

- `online-mode=false` / `--offline`: any client joins, official or cracked, with the Spigot/Paper-compatible offline UUID.
- `online-mode=true` / `--online`: login encryption (1024-bit RSA key, AES-128/CFB8), the session-server check on worker
  threads separate from the I/O loops, real UUID/name and skin properties forwarded in Login Success.
- Protocol data (packet IDs, registries) is recorded from Mojang's server per version, so each supported release is
  spoken the way the official client expects.

## Not covered

- Mixed mode (authenticated and cracked at once) is not implemented.
- Login against the real Mojang session server has not been exercised; tests use a local HTTPS fake.
