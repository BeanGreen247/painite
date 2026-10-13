# Painite

A fast, low-resource Minecraft server for Linux, written in C++. Java and Bedrock clients
connect natively on one port, with no Geyser or other addon.

**Status: early prototype.** It is not a replacement for Paper yet. See "What works".

## Server directory

Same layout as a vanilla/Paper server, so existing tooling (CreeperCrest, panels, scripts) keeps working:

```
server.properties   vanilla keys: server-port, motd, max-players, view-distance, white-list, gamemode, level-name, spawn-protection ...
whitelist.json      white-list=true; edited by /whitelist or by hand (reloaded within 3 s)
banned-players.json /ban, /pardon (uuid, name, created, source, expires, reason)
banned-ips.json     /ban-ip, /pardon-ip
ops.json            /op, /deop (level 1-4)
server-icon.png     64x64 PNG shown in the server list; picked up when it changes
logs/latest.log     Paper-style lines, e.g. "[20:10:12] [20:10:12 INFO]: Name joined the game"
world/              a vanilla or Paper world: region/*.mca (zlib, gzip, uncompressed or LZ4), level.dat, player data. 26.x layouts
                    (dimensions/minecraft/overworld/region, players/data) and the older ones (region, playerdata) both work
painite.toml        Painite-only settings (cores, workers, memory cap, bandwidth profile, world generator, autosave)
```

Console and in-game commands: `help`, `list`, `tps`, `version`, `me`, `tell`/`msg`/`w`, `say`, `kick`, `ban`, `ban-ip`, `pardon`, `pardon-ip`, `banlist`,
`whitelist on|off|add|remove|list|reload`, `op`, `deop`, `gamemode`, `time set|add|query`, `tp`, `setblock`, `fill`, `save-all`, `save-on`, `save-off`, `stop`.
Permissions follow ops.json (level 2 for world-changing commands, 3 for bans, whitelist and op, 4 for stop and saving); the console is level 4.
`nogui` is accepted and ignored.

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
`tools/fetch_bedrock_data.py` writes `data/bedrock-<version>.bin` (the packet sequence a Bedrock client needs to join) and
`tools/fetch_bedrock_blocks.py` writes `data/blocks-bedrock.bin` (Java block state to Bedrock runtime id, from Geyser's MIT-licensed block palette).
Use `./build.sh --data --java 26.2,26.3` to generate several Java versions.
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

- Java 26.3 (protocol 777) and 26.2 (776) handshake, status/ping, online and offline login (RSA + AES/CFB8, session-server check), configuration, play join, chunk streaming, movement-driven chunk loading, keep-alive, chat, a client command tree.
- **Real worlds.** Region files are read and written in the 26.x format (palette entries as strings or compounds, block states mapped by name and properties between versions), with skylight and block light computed for the client. Chunks Painite saves load in Mojang's own server (`tests/vanilla_roundtrip.py`). Chunks missing on disk come from a flat generator for a new world and are void in an existing world (`[world] generator`). Players spawn at their saved position (`players/data/<uuid>.dat`, updated on leave and on autosave) or the world spawn.
- **Editing.** Players break and place blocks (creative building from the hotbar, survival breaking without drops), `/setblock` and `/fill` work, changes are broadcast to everybody who can see the chunk and are saved (autosave every `[world] autosave_s`, on `/save-all` and at shutdown). Placement derives log axis, stairs, slabs, torches and facing from where you click and look.
- **Game loop and regions.** A 20 Hz tick thread owns world time, the player registry, visibility between players, movement relay, autosave and unloading of idle chunks. Block updates are scheduled per 512 x 512 block region and the regions that have work run in parallel on `tick_threads` threads (like Folia): sand, gravel and concrete powder fall, plants, torches, rails and carpets pop off when their ground goes, tall plants lose their other half. Water and lava do not flow yet.
- **Players see each other**, across editions: tab list, spawn, movement with relative or absolute updates, head and body rotation, arm swing, skin layers, leaving. Java sees Bedrock players with the default Steve/Alex skin (Java clients only accept Mojang-signed textures); Bedrock sees Java players with their real skin when the Java server is in online mode, otherwise a plain default skin, and sees other Bedrock players' own skins.
- **Bedrock**, built in on the same port with `bedrock_port = "same"`: a full RakNet layer, zlib batches, the encryption handshake, login, the resource-pack handshake, a replayed StartGame patched per player, chunks and sub chunks from the shared world (Java blocks translated to Bedrock runtime ids by `tools/fetch_bedrock_blocks.py`), movement (PlayerAuthInput as of 1.26.40), creative and predicted block breaking, block updates in both directions, time, chat and commands shared with Java. Bedrock players use a `.` name prefix (like Floodgate) and count toward `max-players`, the whitelist and bans.
- **Bedrock online login.** With `online-mode=true` Bedrock clients must present a Minecraft authorization token (RS256, issuer, expiry and client key binding are checked against the service's published keys, on the auth threads). Their UUID follows Floodgate's scheme (`00000000-0000-0000-<xuid>`). It is tested against a local fake service (`tests/xbox_test.py`); a real Microsoft account has not been tried.
- Worker threads, each with its own epoll loop and `SO_REUSEPORT` listener, pinned to the cores from `painite.toml`. New workers start under load and idle ones retire, inside a total memory cap.
- Backpressure-driven chunk sending, so slow or high-ping links are not flooded; zlib compression for non-LAN peers.

## What does not work yet

- No inventory: Java creative players can place what they pick, survival players cannot build or collect drops; Bedrock players can break blocks but not place them (their creative inventory is server-authoritative).
- No mobs, no physics beyond what the clients simulate, no fluids flowing or redstone, no block entities (chests and signs are not sent to clients, so they are invisible; their data is kept in the world files), no weather, no nether or end (dimensions other than the overworld are ignored).
- Terrain is not generated: use an existing world, or the flat fallback.
- Real-Mojang online login for Java and Microsoft login for Bedrock are untested against the live services (tests use local fakes).
- Anti-xray, Velocity/BungeeCord forwarding, io_uring.
- Java plugins and mods. Painite is native code and does not load Paper/Spigot/Bukkit plugins or Forge/Fabric mods; keep using Paper for those.

Tests (all use a server you start yourself unless noted):
`cmake --build build --target painite_unit && build/painite_unit`; `tests/smoke.py` (Java join), `tests/game_test.py` (two Java bots: entities, blocks, commands),
`tests/crossplay_test.py` (Java and Bedrock together), `tests/bedrock_smoke.py`, `tests/lists_test.py` (bans, whitelist, icon), `tests/world_compare.py` and
`tests/vanilla_roundtrip.py` (worlds made by `tools/make_test_world.py`; the second one starts Mojang's server on a world Painite saved),
`tests/online_test.py` and `tests/xbox_test.py` (start their own server and a fake auth service), `tests/udp_lossy_proxy.py`, `tests/check_layouts.py`.
Packet layouts were checked against real servers with `tools/capture_play.py` (Java) and `tools/bedrock_capture_entities.py` (Bedrock).

Companion web manager: [CreeperCrest](https://github.com/BeanGreen247/creepercrest).

## License

Source-available, all rights reserved. Read, build and run for personal use; no forks,
redistribution or derivative works. See `LICENSE`.
