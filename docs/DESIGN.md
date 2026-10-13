# Painite design draft

## Goals
Fast, low-resource Linux-first Minecraft server in C++. Java and Bedrock clients
join the same world natively. No plugin API compatibility with Paper/Bukkit.

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
- Packet batching and per-connection compression (zstd where the client supports it, else zlib).
- Adaptive view distance and chunk send rate driven by measured RTT and loss.
- Delta entity updates, position quantization, and skipping off-screen entity updates.
- Chunk send priority by distance and look direction; client-ACK based pacing.
- PROXY protocol v2, Velocity modern forwarding and BungeeCord forwarding for remote proxies.

## Anti-xray
Chunk-encode step hides or replaces ore blocks not exposed to air (modes 0/1/2),
with per-world config, similar in behavior to Paper's anti-xray.

## Build
`-O3 -march=native`, LTO, then PGO and BOLT builds, each kept only if measured faster.
