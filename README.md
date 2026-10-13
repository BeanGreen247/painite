# Painite

A fast, low-resource Minecraft server for Linux, written in C++. Java and Bedrock clients
connect natively on one port, with no Geyser or other addon.

**Status: early prototype.** It is not a replacement for Paper yet. See "What works".

## Server directory

Same layout as a vanilla/Paper server, so existing tooling (CreeperCrest, panels, scripts) keeps working:

```
server.properties   vanilla keys: server-port, motd, max-players, view-distance, white-list, gamemode, ...
whitelist.json      read for white-list=true
banned-players.json read for bans
server-icon.png     shown in the server list
logs/latest.log     Paper-style lines, e.g. "[20:10:12] [20:10:12 INFO]: Name joined the game"
world/              reserved for the world
painite.toml        Painite-only settings (cores, workers, memory cap, bandwidth profile)
```

Console commands: `help`, `list`, `say <msg>`, `status`, `stop`. `nogui` is accepted and ignored.

## Build and run

Build on the machine that will run the server. `build.sh` installs the build dependencies (apt, dnf/yum, pacman, zypper or apk; needs root or sudo)
and compiles with `-O3 -march=native` and link-time optimisation, which ties the binary to that machine's CPU.

```
./build.sh --data                 # install deps, build, and generate the Java + Bedrock protocol data (downloads Mojang's servers)
./build.sh --prefix /opt/painite  # optional: install bin/ and data/ there
cd /path/to/server-dir && /path/to/painite
```

Other flags: `--pgo` (profile-guided build, about 3 minutes, optional), `--no-deps`, `--check`, `--jobs N`.
In my test (12 Java and 3 Bedrock joins) the server used 0.13 s of CPU either way, so `--pgo` made no measurable difference there; measure on your own load before relying on it.
Without the script: `cmake -S . -B build && cmake --build build -j`, then `python3 tools/fetch_vanilla_data.py` and `python3 tools/fetch_bedrock_data.py`.

`tools/fetch_vanilla_data.py` writes `data/vanilla-<version>.bin/.ids` (packet IDs, registries, a flat chunk) and
`tools/fetch_bedrock_data.py` writes `data/bedrock-<version>.bin` (the packet sequence a Bedrock client needs to join).
Nothing from Mojang is committed; the files are generated locally. Painite serves every Java version it finds data for.

Options: `--config`, `--port`, `--bedrock-port N|same`, `--cores 0-3`, `--max-workers`, `--memory-cap MB`, `--no-bedrock`.

## Official and cracked clients

One switch in `server.properties` (same as every Minecraft server), with CLI overrides:

| Setting | Behaviour |
|---|---|
| `online-mode=true` (or `--online`) | Encrypted login, verified against Mojang's session server; real UUID and skin properties. Cracked launchers cannot join. |
| `online-mode=false` (or `--offline`) | No verification; official and cracked clients both join. UUID is `md5("OfflinePlayer:"+name)`, identical to Spigot/Paper/Cuberite, so existing player data keeps matching. |

Verification runs on separate threads, so a slow session server never stalls the game loop.
`prevent-proxy-connections=true` is honoured. `[auth] session_host` / `session_ca_file` in `painite.toml` allow a private auth server.
See `docs/COMPAT.md` for how Paper, Spigot and Cuberite handle this.

## What works

- Java 26.3 (protocol 777) and 26.2 (776) handshake, status/ping, online and offline login (RSA + AES/CFB8, session-server check), configuration, play join into a flat world, chunk streaming, movement-driven chunk loading, keep-alive, global chat.
- Worker threads, each with its own epoll loop and `SO_REUSEPORT` listener, pinned to the cores from `painite.toml`. New workers start under load and idle ones retire, inside a total memory cap.
- Backpressure-driven chunk sending, so slow or high-ping links are not flooded; zlib compression for non-LAN peers.
- Bedrock (built in, same port as Java with `bedrock_port = "same"`): full RakNet session layer (reliability, ordering, split packets, ACK/NACK, resends, congestion window), zlib batches, the encryption handshake, offline login, the resource-pack handshake, a replayed StartGame, chunk and sub-chunk streaming, movement-driven chunk loading and chat shared with Java players. Joined Bedrock players use a `.` name prefix (like Floodgate) and count toward `max-players`, the whitelist and bans.

## What does not work yet

- Real-Mojang online login is untested (tests use a local fake session server); a Microsoft account is needed to try it.
- Bedrock: Xbox authentication (Bedrock logins are refused while `online-mode=true` unless `bedrock_unauthenticated = true`), skins of other players, seeing Java/Bedrock players across editions, block interaction, entities, commands.
- World loading/saving, blocks, entities, other players visible to each other, commands, anti-xray.
- Java plugins and mods. Painite is native code and does not load Paper/Spigot/Bukkit plugins or Forge/Fabric mods; keep using Paper for those.

Tests: `python3 tests/bedrock_smoke.py --port 25565` (Bedrock join), `tests/udp_lossy_proxy.py` (drop/delay UDP in between), `cmake --build build --target painite_unit && build/painite_unit`, `python3 tests/smoke.py --port 25565` (running offline server), `python3 tests/online_test.py` (starts its own server and fake session server), `python3 tests/check_layouts.py`.

Companion web manager: [CreeperCrest](https://github.com/BeanGreen247/creepercrest).

## License

Source-available, all rights reserved. Read, build and run for personal use; no forks,
redistribution or derivative works. See `LICENSE`.
