# Painite handoff (written 2026-10-15, for whoever continues: another Claude session, Kimi, DeepSeek, a human)

Read this first, then `README.md`, `docs/DESIGN.md`, and the memory bank note `~/claude-memory-bank/projects/painite.md` if you have it.

## Owner's standing rules (do not break)
- Goal: **one-to-one vanilla parity, byte-exact where possible**: worldgen, what clients see, gameplay. Java first; **Bedrock parity is out of scope for now**.
- Gameplay/entity/physics/tick behaviour: model on **Cuberite's** implementation (accurate, readable open source), then adjust toward Mojang's. Worldgen is a bit-exact Mojang port (`src/vg`), verified against Mojang's own server.
- Do **not** propose or do repo cleanup (README refresh, sanitizer/fuzz runs, etc.) unless the owner asks. Testing against live Mojang/Microsoft login is deferred until everything else exists.
- Commits: one-line conventional style `type(scope): message`, **no Co-Authored-By or any AI attribution lines**. The owner asked (2026-10-15) to **push `main` to origin at the end of each session** so no work is lost.
- Never call paid or metered APIs. Keep working without asking questions; if a session limit hits, wait for the reset and continue.

## Environment you need (all free, local)
- Mojang 26.x jars are Java 25 class files; the box has Java 21. Download Temurin 25 (`https://api.adoptium.net/v3/binary/latest/25/ga/linux/x64/jdk/hotspot/normal/eclipse`) and put its `bin` first on `PATH` before running `tools/fetch_worldgen.sh`, `tools/oracle/*.java`, `tools/fetch_vanilla_data.py`.
- Data is git-ignored and per checkout: `tools/fetch_worldgen.sh 26.2` (writes `data/worldgen/26.2`, incl. `blockflags.txt`), and `cd /tmp && python3 -I <repo>/tools/fetch_vanilla_data.py --version 26.2 --out <repo>/data`.
- **The vanilla generator targets 26.2** (`src/world.cpp:51` sets `o.version = "26.2"`). 26.3 changed the noise engine (`DensityVolume`, `DensitySampler`, no `size_horizontal` in noise_settings) and is a separate to-do ("26.3 engine"). Always generate and decompile **26.2**, not 26.3.
- Reading Mojang's source: the jar is unobfuscated. `build/mojang/inner-26.2.jar` + CFR (`https://github.com/leibnitz27/cfr/releases/download/0.152/cfr-0.152.jar`):
  `unzip` the packages you want into a dir, `jar cf x.jar net`, then `java -jar cfr.jar x.jar --extraclasspath build/mojang/inner-26.2.jar --outputdir src --silent true`.
  (The first decompile this session used 26.3 by mistake; structure placement code was identical in the parts read, but re-read anything noise related from 26.2.)
- `build/` had a stale CMake cache (repo was moved); fixed by deleting `build/CMakeCache.txt build/CMakeFiles` and re-running `cmake -S . -B build`. A full build takes about 90 s with `-j2`.
- Reference worlds from Mojang's server: `build/painite_refgen --version 26.2 --seed 12345 --out DIR --area X1 Z1 X2 Z2 [--structures] [--dim overworld|the_nether|the_end]` (takes minutes, needs JDK 25 on PATH). Compare: `build/painite_gencmp --world DIR/run/world --version 26.2 --seed 12345 --stage features`. Build both with `cmake --build build --target painite_refgen painite_gencmp -j2`.

## State of the code (updated 2026-10-15, evening)
- `src/vg/` is the bit-exact Mojang worldgen port (noise, biomes, surface, carvers, all features, sculk, nether/end). Open issue from before: full-feature compare differs about 0.9 % because of **decoration order** (ore veins draw floats per candidate so differences desync later veins). Best global order tried is `gencmp --order colmajor` (0.866 %). Next idea: emulate Mojang's real per-request chunk scheduling.
- `src/structures*.cpp` (no `vg/`) are the OLD approximate structures used by the old generator only.
- **Structures (new, all under `src/vg/`)**, wired into decoration (`decorate.cpp` structure hook, `gen.cpp` `decorate_one`), the Beardifier (`beardifier.*`, terrain adaptation) and post-generation shape updates (`shape.*`):
  - `structure.hpp/.cpp`: BoundingBox, StructureStart/Piece, `StructureState` (structure_set placement incl. random_spread, frequency reducers, exclusion zones, concentric rings; `starts_at`, `starts_touching`, `place_step`), height providers, helpers.
  - `template.hpp/.cpp`: .nbt templates, placement (rotation/mirror, waterlogging), processors (rule, block_rot, protected_blocks, capped, gravity, jigsaw replacement, block_age, lava_submerged, blackstone_replace), pools from `worldgen/template_pool`.
  - `jigsaw.cpp`: JigsawPlacement incl. pool aliases. **Verified exact**: villages, trial chambers (all pieces match Mojang's saved starts).
  - Code-built pieces use the Java-like layer `jcompat.hpp` + `legacy_piece.hpp/.cpp` (generateBox/placeBlock/createChest/...), `template_piece.hpp` (TemplateStructurePiece). Files: `structures_scattered.cpp` (desert pyramid, jungle temple, swamp hut, igloo, buried treasure, nether fossil), `structures_mineshaft.cpp`, `structures_portal.cpp` (ruined portals), `structures_underwater.cpp` (shipwrecks, ocean ruins), `structures_stronghold2.cpp`, `structures_fortress.cpp`, `structures_endcity.cpp`, `structures_mansion.cpp`, `structures_monument.cpp`.
  - Mojang's Java was translated mechanically with `tools/j2cpp.py` (statement level) and `tools/j2cpp_class.py` (nested classes). The generated files are the source of truth now; edit them directly.
  - **Verified against Mojang's server** (reference worlds, seed 12345; see Verification workflow): piece lists (`gencmp --starts`) are identical for every structure type tested: villages (5 biomes), ocean monument, stronghold, shipwrecks, ocean ruins, desert pyramid, swamp hut, jungle temple, igloo, pillager outpost, ancient city, trail ruins, ruined portals (overworld, nether), buried treasure, trial chambers, mineshaft, woodland mansion, nether fortress, bastion remnant, nether fossil, end city. Block scans: end city 0 diffs, nether sites ~0.001 %, overworld sites 0.001 to 0.05 %; what is left is reference-server tick noise (crops, doors, leaves, fence/bars connections, lava/water flowing into voids), terrain aquifer water vs air next to monument/ancient city/stronghold (unexplained, probably flowing water in the reference), and the desert pyramid whose heightPosition depends on which chunk is decorated first (decoration order, same family as the 0.9 % diff).
  - Fixes found by this verification worth knowing: mansion 1x1 rooms need template size 6 in `getZeroPositionWithTransform`; a chunk's starts of one structure are visited in fastutil `LongOpenHashSet` order (`reference_order` in structure.cpp); `canHangChainBelow` needs the center-sturdy block fact (blockflags bit 19, rerun `DumpBlocks.java` / `tools/fetch_worldgen.sh` to get it); surface rule `noise_threshold` has `is_3d` (sulfur caves); height providers use the generator's depth (`env.gen_min_y/gen_height`, nether fossil).
  - Entities created by structures (witch, drowned, elder guardian, chest minecart, ...) are saved as minimal NBT in `ChunkBlocks::entities`; block entities (chests with LootTable + LootTableSeed, spawners, ...) in `ChunkBlocks::block_entities`. Block entities are now copied into `Chunk::extra["block_entities"]` (sent to clients by jchunk.cpp, saved with the chunk); entities go to `Chunk::gen_entities`, which nothing consumes yet (no entity system).
  - Structure starts are not saved in chunk NBT (`structures.starts` / `References`) yet; `/locate` does not use them.

## Gameplay core (new, `src/sim/`, namespace `pn::sim`)
Protocol independent rules, ported from Mojang's classes, tested without a server by `painite_simtest` (`tests/sim/simtest.cpp`, needs `data/game/26.2`: run `tools/fetch_gamedata.sh 26.2` with JDK 25 on PATH).
- `item.*`: `Items` registry (from the data generator's item component reports), `Registries` (protocol ids of every registry from `registries.json`; dynamic ones like enchantments come from the recorded config packets via `set_dynamic`), `ItemStack` (a stack keeps its component patch as the network encodings of the changed components, so stacks round trip byte for byte), wire read/write.
- `components.*`: typed access to enchantments, potion, text components (JSON to network NBT), damage.
- `inventory.*`: `Container`, `Slot`s, `PlayerInventory` (36 + equipment, container indexes 36..42), `Menu` (AbstractContainerMenu: click modes, quick craft/drag, quick move, broadcast with a `ContainerSynchronizer`), `InventoryMenu`, crafting result slot (needs a `CraftingProvider`, recipes not ported yet).
- `player.*`: `PlayerSim` (inventory, open menu, game mode; `handle_container_click` etc. as ServerGamePacketListenerImpl).
- `loot.*`: data driven loot tables (pools, entries, conditions, functions) rolled with Mojang's random call order; `fill()` for seeded containers. Not yet: enchant_randomly/enchant_with_levels, furnace_smelt, copy_components (block entities), exploration_map, entity predicates beyond type/flags/variants. Unverified against Mojang yet: compare by opening the same structure chests on a reference world and on Painite with a bot.
- `data.*`: `Tags`, `BlockTable` (hardness, resistance, requires tool, ...), `EntityTable` (size, tracking range, default attributes).
- Server wiring: `Game` owns a `PlayerSim` per player (`inv_*` methods), container events go to the worker as `GEvent::Container*`; player inventories are saved in `Inventory` (stacks with components keep Painite's `painite_patch` byte array until persistent component codecs exist).
- `phys.*` (AABB collision per Mojang's Entity.collide incl. step up), `entity.*` (Entity base, ItemEntity with Mojang's tick, merge, push out of blocks) are wired through `entity_manager.*`: one manager per dimension ticks entities on loaded chunks and provides block collision boxes, friction, full-block checks, and entity queries. Java entity tracking sends add/remove, movement, position sync, motion and metadata; a take-item packet encoder exists but has no producer. Shapes are loaded from `data/game/26.2/blockshapes_states.txt` and `blockshapes.txt`; `tools/oracle/DumpShapes.java` writes those files. Entities are currently memory-only, Java-only for client tracking, and have no production spawn path. See `docs/AI_HANDOFF.md` for detailed continuation notes.
- A prompt for handing the work to another assistant is in `docs/COPILOT_PROMPT.md`.
- Next: survival digging and drops (port `ServerPlayerGameMode`: progress, tool speed, drops via block loot tables), create dropped `ItemEntity` objects through `Game::add_entity`, implement player pickup, and consume items when placing. Then health/hunger/damage, mobs, fluids, random ticks, redstone, recipes. Entity infrastructure and collision-shape loading are already wired; persistence, generated chunk entities, and Bedrock tracking remain outstanding.

## Verification workflow (needs JDK 25 on PATH, see Environment)
- Locate structures in Mojang's server: `build/painite_refgen --version 26.2 --seed 12345 --structures --out DIR --locate stronghold --locate monument ...` (prints `LOCATE name x z`).
- Reference world with only structures (no biome features): `build/painite_refgen --version 26.2 --seed 12345 --structures --stage structures --out DIR --area X1 Z1 X2 Z2 [--area ...]` (several areas load in one session; chunks are loaded in 7x7 batches so the server keeps up). The finished world is `DIR/world` (not `DIR/run/world`).
- Compare piece lists: `build/painite_gencmp --world DIR/world --version 26.2 --seed 12345 --stage structures --starts --range CX0 CZ0 CX1 CZ1`. Compare blocks: `... --stage structures --scan CX0 CZ0 CX1 CZ1 [-v] [-vv --vname NAME]`. Only chunks with status full in the reference are compared.
- `PAINITE_JIGSAW_TRACE=1` prints every jigsaw attach decision.
- The reference server keeps ticking, so crops grow, doors open, grass spreads, water flows: ignore those few diffs.

## Next steps, in order
1. (done) structure verification. Left: structure starts/references into chunk NBT; `/locate` already works through `VanillaGen::locate`. Transfer `Chunk::gen_entities` into the entity manager and persist entities when that work is scheduled.
2. Look at the aquifer water vs air diffs near structures (monument y=-2, ancient city) in the `full` stage to see whether they are real.
3. Performance: `starts_touching` computes up to 289 start lookups per chunk, chunk generation holds a global decoration lock; measure and fix.
4. Make `generator = "vanilla"` the default once gencmp is clean; fix the 0.9 % decoration-order diff.
5. **Gameplay** (model on Cuberite first, then Mojang; Cuberite source: github.com/cuberite/cuberite): entity persistence and generated entities from chunk NBT, player pickup and entity interactions, block entities to clients, inventory and containers, drops/tools/damage/health/hunger, mob AI/spawning, fluids flowing, random ticks (crops, leaves, fire), weather, redstone, loot tables/recipes/advancements, `/locate`.
6. Later: 26.3 engine, Bedrock parity, live-service login tests (only when the owner says).

## Gotchas
- `rm -rf` may be denied in some harnesses; use fresh dirs. Never `pkill -f` patterns that match your own command line.
- `tests/run_all.sh` needs network (generates a vanilla world first). Test worlds persist between runs, use fresh dirs.
- Debug env vars still in code: PAINITE_TRACE, PAINITE_CELLS, PAINITE_DBG/DBG2/PROF.
