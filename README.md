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

```
python3 tools/fetch_vanilla_data.py          # one-time: protocol data from Mojang's own server jar (needs Java download)
python3 tools/fetch_vanilla_data.py --version 26.2
cmake -S . -B build && cmake --build build -j
cd /path/to/server-dir && /path/to/painite/build/painite
```

`tools/fetch_vanilla_data.py` writes `data/vanilla-<version>.bin/.ids` (packet IDs, registries, a flat chunk).
Nothing from Mojang is committed; the files are generated locally. Painite serves every version it finds data for.

Options: `--config`, `--port`, `--bedrock-port N|same`, `--cores 0-3`, `--max-workers`, `--memory-cap MB`, `--no-bedrock`.

## What works

- Java 26.3 (protocol 777) handshake, status/ping, offline login, configuration, play join into a flat world, chunk streaming, movement-driven chunk loading, keep-alive, global chat.
- Worker threads, each with its own epoll loop and `SO_REUSEPORT` listener, pinned to the cores from `painite.toml`. New workers start under load and idle ones retire, inside a total memory cap.
- Backpressure-driven chunk sending, so slow or high-ping links are not flooded; zlib compression for non-LAN peers.
- Bedrock: server-list ping and the RakNet open-connection handshake only.

## What does not work yet

- Online-mode authentication (the server refuses to start with `online-mode=true`).
- Bedrock sessions (RakNet frames, login, world sync).
- World loading/saving, blocks, entities, other players visible to each other, commands, anti-xray.
- Java plugins and mods. Painite is native code and does not load Paper/Spigot/Bukkit plugins or Forge/Fabric mods; keep using Paper for those.

Smoke test (needs a running server and its `.ids` file): `python3 tests/smoke.py --port 25565`.

Companion web manager: [CreeperCrest](https://github.com/BeanGreen247/creepercrest).

## License

Source-available, all rights reserved. Read, build and run for personal use; no forks,
redistribution or derivative works. See `LICENSE`.
