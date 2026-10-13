# Painite design draft

## Goals
Fast, low-resource Linux-first Minecraft server in C++. Java and Bedrock clients
join the same world natively. No plugin API compatibility with Paper/Bukkit.

## Implemented so far

- `world.cpp`/`anvil.cpp`: chunk map with per-chunk locks, region files, flat/void fallback, idle unloading, light (`ensure_light`).
- `game.cpp`: tick thread, player registry, interest management (512 block region grid), world time, autosave; workers talk to it through
  `Game::` calls and receive `GEvent`s through `Worker::post_fn`, so every protocol layer only encodes events for its own clients.
- `java_game.cpp` and `bedrock_game.cpp`: the Java and Bedrock encoders for those events and the input handlers (movement, digging, placing).
- Regions are currently an indexing structure for visibility, not yet independent ticking units: all ticking happens on one thread. That is
  enough while nothing but players is simulated; entities, fluids and redstone would be sharded by region first.

## Threading and autoscaling
- Folia-style regions: the world is split into regions, each ticked by one worker.
- Worker pool grows when a region's tick time exceeds `spawn_at_tick_ms`, shrinks after idle.
- Pool-wide `memory_cap_mb` is enforced by per-worker arenas; at the cap no new workers spawn
  and chunk cache is evicted first.
- CPU set comes from `painite.toml` (`[cpu].cores`); CLI flags override the file.

## I/O
- io_uring for sockets and region files, with an epoll fallback.
- splice/vmsplice for relaying and file-to-socket sends where payloads are pre-encoded.
- Pre-encoded chunk packets are cached, so a chunk is serialized once and sent to many clients.

## Protocols
- Java: TCP 25565.
- Bedrock: RakNet over UDP, default 19132 (Geyser's default). `bedrock_port = "same"` reuses `java_port`
  over UDP, so one number (e.g. 28258) serves both editions, matching Geyser's `clone-remote-port`.
- One internal world and entity model; each protocol has its own encoder/decoder layer.

## Bandwidth for high-ping / lossy / proxied links
- Packet batching and per-connection compression. Java clients only speak zlib; zstd is not available on the Java protocol, so it is not used.
- Adaptive view distance and chunk send rate driven by measured RTT and loss.
- Delta entity updates, position quantization, and skipping off-screen entity updates.
- Chunk send priority by distance and look direction; client-ACK based pacing.
- PROXY protocol v2, Velocity modern forwarding and BungeeCord forwarding for remote proxies.

## Anti-xray
Chunk-encode step hides or replaces ore blocks not exposed to air (modes 0/1/2),
with per-world config, similar in behavior to Paper's anti-xray.

## Build
`-O3 -march=native`, LTO, then PGO and BOLT builds, each kept only if measured faster.

## Server directory and plugins
Painite reads the vanilla directory layout (server.properties, whitelist.json, banned-players.json, server-icon.png,
logs/latest.log in Paper's line format) so panels like CreeperCrest keep working.
Java plugins (Bukkit/Spigot/Paper) are not supported: they need a JVM and the Bukkit/Paper API plus server internals.
Planned native replacements: Bedrock crossplay (Geyser/Floodgate), anti-xray (Orebfuscator), multi-version
protocol support (ViaVersion), built-in profiler (spark).

## Protocol data
Packet IDs and registry packets change with every Minecraft release, so they are not hardcoded.
`tools/fetch_vanilla_data.py` records them from Mojang's own server for a given version and Painite loads them at start.

## Bedrock
`src/rak.cpp` is a RakNet session layer written from the protocol (frames, reliability, ordering, split packets, ACK/NACK,
resend timers, a small congestion window). `src/bedrock.cpp` runs the Bedrock protocol on top of it: batches with zlib
(per-batch threshold from `network-compression-threshold`), the ECDH/AES-256-CTR handshake, login, and the join sequence.
The join sequence (StartGame, registries, item/creative/crafting data, commands) is not hand-built: it is recorded from a
real Bedrock Dedicated Server by `tools/bedrock_capture.py` and replayed, with the per-player parts (player list entry,
name, chunk coordinates) patched. Static parts are compressed once at startup and only encrypted per client.
Measured against a UDP relay with 5% loss and 60 ms one-way delay the full join (about 0.7 MB compressed) completes in ~7 s.
