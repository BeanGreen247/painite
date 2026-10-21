# Supported versions

Written 2026-10-21. Keep this current when a version is added or a gap is closed.

## Java and Bedrock pairs (by release date)

| Java | released | Bedrock | Bedrock protocol | BDS zips that exist | captured |
|------|----------|---------|------------------|---------------------|----------|
| 26.1 (775) | 2026-03-24 | 26.10 | 944 | 1.26.10.4 .. 1.26.14.1 | 1.26.14.1 |
| | | 26.20 | 975 | 1.26.20.5 .. 1.26.23.1 | 1.26.23.1 |
| 26.2 (776) | 2026-06-16 | 26.30 | 1001 | 1.26.30.5 .. 1.26.34.3 | 1.26.34.3 |
| | | 26.40 | 2168 | .. 1.26.44.3 | 1.26.44.3 |
| | | 26.45 | 2169 | 1.26.45.1 | 1.26.45.1 |
| 26.3 (777) | 2026-09-15 | 26.50 | 2193 | 1.26.50.5 .. 1.26.52.3 | 1.26.52.3 |

26.20, 26.40 and 26.45 have no Java release of their own; they are served because the data was cheap once the machinery existed. Geyser's block palettes exist for
26.10 and 26.20 (git history of `core/src/main/resources/bedrock`), 26.30, 26.40 and 26.50. Download addresses of old Bedrock
servers follow `https://www.minecraft.net/bedrockdedicatedserver/bin-linux/bedrock-server-1.26.X.Y.zip`; fetch with HTTP/1.1
and a browser user agent, one request per second (the site answers 403 to bursts).

## What works today

| | 26.1 | 26.2 | 26.3 |
|---|---|---|---|
| Java client login, play, chunks, movement, blocks | yes (tests `smoke-26.1`, `game-26.1`) | yes | yes |
| Item components in inventories | dropped (ids are translated by name) | kept | dropped |
| Terrain, structures and features of that version | `worldgen_version = "26.1"`: 1.236 % of blocks differ from Mojang's server over 338 chunks, seed 12345 | `"26.2"` (default): 1.255 % over the same area (the decoration order family, see HANDOFF) | `"26.3"`: ported engine, see below (overworld features stage 0.52 % of blocks differ in the interior, nether 0.12 %, end 0 %; 0.012 % with structures only) |
| Bedrock clients | protocols 944 and 975 | 1001, 2168, 2169 | 2193 |

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

## Java 26.3 (ported)

26.3 is a rewrite of the generation code, not a delta: float density with volume sampling, `feature` instead of `configured_feature`, material
rules instead of `surface_rule`, carvers that fill a mask, block states as `{id, properties}` and flat feature configs. Select it with
`worldgen_version = "26.3"`. Files: `noise3`, `df3` (noise and density functions), `terrain3` (aquifer, chunk fill, material rules, surface),
`carver3`, `Beardifier::compute_f`, `norm263` (rewrites the 26.3 data files into the shape the 26.2-era feature code reads), `climate` (19 children
per R-tree node, the search keeps its last result per thread like Mojang's, so exact ties follow the fill order).

Checked against Mojang's own code or server (seed 12345, `build/painite_v3check`, `painite_gencmp` on staged `painite_refgen` worlds):

| Stage | Result |
|---|---|
| Noise, density functions (8 router functions) | 0 mismatches over 800,000 floats, point and volume (`v3check --what all`) |
| Climate lists and chunk biomes | 0 of 884,736 biome cells differ |
| Aquifer and fill (overworld, nether, end) | 0 mismatches over about 10 million blocks (`v3check --what fill`) |
| Surface (material rules, ore veins) | 0.002 % of blocks, all of them water that Mojang's server settled after generation |
| Carvers | the same 0.002 % plus 1 block |
| Structures (starts and pieces) | all starts and about 1,700 pieces match: mineshaft, shipwreck, ruined portal, ocean ruin, ancient city, village, trial chambers; block level 0.012 % |
| Features (all), inside the area | overworld 0.52 %, nether 0.12 % (without the lava Mojang's server settles), end 0 % in the central chunks; ore, grass and cave features each match when run alone, the rest is the decoration order family (neighbouring features overlap and Mojang's server decorates in its own order) and `MossyCarpetBlock.placeAt`, which Mojang seeds from the level's own random |

Not checked: the nether and end beyond the fill, Bedrock translation of 26.3 chunks, large biomes and amplified, the End island noise in features.
Gotchas found on the way: density cache cells start empty (`NaN`), the climate sampler of structures is built with caches on, `blocktransforms.txt` and
`blockentities.txt` must exist for 26.3 (`tools/fetch_worldgen.sh 26.3`), pool aliases and `fallback` keys must not go through the state normaliser.

## Bedrock protocol groups (Cloudburst `bedrock-codec`, branch 3.0)

Serializers that differ, from the codec classes `Bedrock_v944`, `_v975`, `_v1001`, `_v2168(_hotfix4)`, `_v2169`, `_v2193`:

- 1001 -> 2168: StartGame, LevelChunk, SubChunk, SubChunkRequest, ResourcePackClientResponse, ResourcePacksInfo, PlayerList,
  PlayerSkin, MovePlayer, MoveEntityDelta, PlayerAuthInput, PlayerLocation, DimensionData, CreativeContent, CraftingData,
  ItemStackResponse, Transfer, SetScore, StructureBlockUpdate, PlaySound, ClientboundMapItemData, AnvilDamage.
- 2168 -> 2193: SubChunk (height map in 16 byte pieces), PlayerAuthInput, MoveEntityDelta, ItemStackResponse, DimensionData, PlaySound,
  BossEvent, CameraPresets, InventoryTransaction and a few debug packets.

Painite replays what a real BDS sent (`tools/fetch_bedrock_data.py --bds-version X`) and patches or builds StartGame, Respawn,
PlayerList, AddPlayer, LevelChunk, SubChunk, UpdateBlock and MovePlayer itself, and parses ResourcePackClientResponse,
SubChunkRequest and PlayerAuthInput. Per protocol (`BSession::proto`):

| packet | before 2168 (944, 975, 1001) | 2168, 2169 | 2193 |
|---|---|---|---|
| ResourcePackClientResponse | u8 status, u16 count | varint status + text | same as 2168 |
| SubChunk packet and entries | centre as zigzag varints, u32 count; entry: result, data, height map raw | centre as 3 ints, varint count; optional wrappers, height map raw | height map in 16 byte pieces |
| LevelChunk tail | -2 marker + u16 limit, cache byte | count, optional limit, cache byte, blob count | same as 2168 |
| PlayerList | action byte, count | count, add varint, action byte | same as 2168 |
| MovePlayer teleport data | implied by the mode | behind a presence byte | same as 2168 |
| PlayerAuthInput | flags as a bit set, no presence bytes | flag list behind a byte, presence byte pairs | flag list, one presence byte |

Checks that were possible without a real client: for every protocol the packet order of a Painite session, recorded by the same
Python client that recorded the BDS session, equals BDS's (only SetTime and one more packet are extra), the LevelChunk tail of BDS
and Painite are byte for byte equal for 2168 and the legacy form matches BDS's 944/1001 bytes, and the PlayerList header parses.
No real Bedrock client was available, so none of this has been tried in the game.

Fixed on the way: the PlayerList Painite sent for 2168+ was misaligned by one byte, and LevelChunk/SubChunk wrote the dimension as a
plain varint where it is a signed (zigzag) one, which only shows for the Nether and End.

## Data you need

```
./build.sh --data                                     # Java 26.2/26.3 protocol data and Bedrock data of the newest server
python3 tools/fetch_vanilla_data.py --version 26.1    # more Java versions
tools/fetch_worldgen.sh 26.1 26.2 26.3                # terrain data per version (JDK 25 on PATH)
tools/fetch_gamedata.sh 26.1 26.2 26.3                # item and block facts per version
python3 tools/fetch_bedrock_blocks.py --blocks data/vanilla-26.3.blocks --palette 26_50 --protocol 2193
#   palettes: 26_30 -> 1001, 26_40 -> 2168 and 2169; 1_26_10 -> 944 and 1_26_20 -> 975 need --ref 607564908190907bd5e42b1a3df96c434ae44168
python3 tools/fetch_bedrock_data.py --bds-version 1.26.52.3   # one Bedrock server version; the data file is named after it
```

The block list given to `fetch_bedrock_blocks.py` must be the newest Java version's (`vanilla-26.3.blocks`): Painite's internal
block ids are the newest version's state ids.
