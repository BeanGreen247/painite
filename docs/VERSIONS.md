# Supported versions

Written 2026-10-21. Keep this current when a version is added or a gap is closed.

## Java and Bedrock pairs (by release date)

| Java | released | Bedrock | Bedrock protocol | BDS zips that exist |
|------|----------|---------|------------------|---------------------|
| 26.1 (775) | 2026-03-24 | 26.10 | not captured yet | 1.26.10.4 .. 1.26.14.1 |
| 26.2 (776) | 2026-06-16 | 26.30 | 1001 | 1.26.30.5 .. 1.26.34.3 |
| 26.3 (777) | 2026-09-15 | 26.50 | 2193 | 1.26.50.5 .. 1.26.52.3 |

Bedrock versions in between (26.20, 26.40, 26.45) are not paired with a Java release. Geyser's block palettes exist for
26.10 and 26.20 (git history of `core/src/main/resources/bedrock`), 26.30, 26.40 and 26.50. Download addresses of old Bedrock
servers follow `https://www.minecraft.net/bedrockdedicatedserver/bin-linux/bedrock-server-1.26.X.Y.zip`; fetch with HTTP/1.1
and a browser user agent, one request per second (the site answers 403 to bursts).

## What works today

| | 26.1 | 26.2 | 26.3 |
|---|---|---|---|
| Java client login, play, chunks, movement, blocks | yes (tests `smoke-26.1`, `game-26.1`) | yes | yes |
| Item components in inventories | dropped (ids are translated by name) | kept | dropped |
| Terrain, structures and features of that version | `worldgen_version = "26.1"`, 1.2 % of blocks differ from Mojang's server (decoration order family, see HANDOFF) | `"26.2"` (default), same family | **not ported** (the 26.3 engine) |
| Bedrock client | no | no | yes (protocol 2193 only) |

The server holds one world. `worldgen_version` in `painite.toml` chooses whose terrain it reproduces; clients of every
supported Java version can join it. Blocks cross versions by name (`Blocks`, newest version is the internal id space).

## Java 26.1 versus 26.2, what is different

Found by decompiling both server jars (CFR, `net/minecraft/network` and `net/minecraft/world/level`) and comparing:

- Protocol: no session id in Login Finished (`vd.protocol >= 776` adds it), one boolean fewer at the end of the play Login packet.
  Serverbound `spectate_entity` became `spectator_action` (not used by Painite).
- Biomes: no `sulfur_caves` (`overworld_climate(false)`).
- Density functions: 26.1 uses `weird_scaled_sampler`; 26.2 spells the same caves out with `interval_select`.
- Features: `dripstone_cluster` and `pointed_dripstone` are 26.2's `speleothem_cluster` and `speleothem` with fixed blocks
  (`dripstone_as_speleothem`); `lake` has no `can_place_feature`/`can_replace_with_*` settings (`lake_pred` supplies 26.1's tests);
  `root_system` has no level test and no world-surface check (`FeatureEnv::pre_262`); `large_dripstone` has no `replaceable_blocks`
  and its wind is not clamped to `16 - radius`.
- Not yet compared line by line: structures (`DesertPyramidPiece`, `RuinedPortal*`, `WoodlandMansionPieces`, `StructurePiece`,
  `EndCityPieces`), processors (`BlockAge`, `BlockIgnore`, `BlockRot`, `Capped`, `BlackstoneReplace`), `SurfaceRules`, `NoiseChunk`.
  The decompiled trees are what `/tmp/.../scratchpad/dc` held; regenerate with the CFR steps in HANDOFF.

## Java 26.3 (to do)

26.3 is a rewrite of the generation code, not a delta: `configured_feature` became `feature`, carvers have a new framework
(`ConfiguredWorldCarver`, `CarvingContext`, `CarverOutput`), `DensityFunctions`/`DensityFunction` changed, 43 network packet
classes changed (`MovementPacket`, `VecDelta`, swing/punch packets, `AddTransientBlock`). Painite already speaks the 26.3
protocol; terrain still comes from the 26.2 generator. Plan: port stage by stage (noise, surface, carvers, features,
structures) with `painite_refgen --version 26.3` reference worlds and `painite_gencmp` at each stage.

## Bedrock protocol groups (Cloudburst `bedrock-codec`, branch 3.0)

Packet serializers that differ, from the codec classes `Bedrock_v1001`, `Bedrock_v2168(_hotfix4)`, `Bedrock_v2169`, `Bedrock_v2193`:

- 1001 -> 2168: StartGame, LevelChunk, SubChunk, SubChunkRequest (v1001 only), ResourcePackClientResponse, ResourcePacksInfo,
  PlayerList, PlayerSkin, MovePlayer, MoveEntityDelta, PlayerAuthInput, PlayerLocation, DimensionData, CreativeContent,
  CraftingData, ItemStackResponse, Transfer, SetScore, StructureBlockUpdate, PlaySound, ClientboundMapItemData, AnvilDamage.
- 2168 -> 2193: SubChunk, PlayerAuthInput, MoveEntityDelta, ItemStackResponse, DimensionData, PlaySound, BossEvent,
  CameraPresets, InventoryTransaction, RecordStarted, and a few debug/diagnostic packets.
- Resource pack response: until 2167 it is `u8 status, u16 count, ids`; from 2168 it is `varint status` plus a text.

Painite replays what a real BDS sent (`tools/fetch_bedrock_data.py`) and patches StartGame, Respawn, PlayerList, AddPlayer,
LevelChunk, SubChunk and UpdateBlock itself, so those and the packets it parses (`ResourcePackClientResponse`, `SubChunkRequest`,
`PlayerAuthInput`, `MovePlayer`) are what each protocol group needs. What exists so far: the server loads every
`data/bedrock-*.bin` and `data/blocks-bedrock-<protocol>.bin`, keeps the protocol per session, and answers "outdated" only for
protocols it has no file for. The capture tools read the server's protocol from the RakNet ping. Capturing 1.26.34.3 fails
because `tools/bedrock_capture.py` writes the 2168 resource pack response; the server side has the same assumption.

## Data you need

```
./build.sh --data                                     # Java 26.2/26.3 protocol data and Bedrock data of the newest server
python3 tools/fetch_vanilla_data.py --version 26.1    # more Java versions
tools/fetch_worldgen.sh 26.1 26.2 26.3                # terrain data per version (JDK 25 on PATH)
tools/fetch_gamedata.sh 26.1 26.2 26.3                # item and block facts per version
python3 tools/fetch_bedrock_blocks.py --blocks data/vanilla-26.3.blocks --palette 26_50 --protocol 2193
python3 tools/fetch_bedrock_data.py --bds-version 1.26.52.3   # one Bedrock server version
```

The block list given to `fetch_bedrock_blocks.py` must be the newest Java version's (`vanilla-26.3.blocks`): Painite's internal
block ids are the newest version's state ids.
