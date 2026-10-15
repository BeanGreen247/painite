# Painite handoff (written 2026-10-15, for whoever continues: another Claude session, Kimi, DeepSeek, a human)

Read this first, then `README.md`, `docs/DESIGN.md`, and the memory bank note `~/claude-memory-bank/projects/painite.md` if you have it.

## Owner's standing rules (do not break)
- Goal: **one-to-one vanilla parity, byte-exact where possible**: worldgen, what clients see, gameplay. Java first; **Bedrock parity is out of scope for now**.
- Gameplay/entity/physics/tick behaviour: model on **Cuberite's** implementation (accurate, readable open source), then adjust toward Mojang's. Worldgen is a bit-exact Mojang port (`src/vg`), verified against Mojang's own server.
- Do **not** propose or do repo cleanup (README refresh, sanitizer/fuzz runs, etc.) unless the owner asks. Testing against live Mojang/Microsoft login is deferred until everything else exists.
- Commits: one-line conventional style `type(scope): message`, **no Co-Authored-By or any AI attribution lines**. Never push unless asked.
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

## State of the code
- `src/vg/` is the bit-exact Mojang worldgen port (noise, biomes, surface, carvers, all features, sculk, nether/end). Open issue from the previous session: full-feature compare differs about 0.9 % because of **decoration order** (ore veins draw floats per candidate so differences desync later veins). Best global order tried is `gencmp --order colmajor` (0.866 %). Next idea: emulate Mojang's real per-request chunk scheduling.
- `src/structures*.cpp` are the OLD approximate structures (box primitives) used by the old generator only. They are not vanilla-exact and are to be replaced.
- **New this session, uncommitted before this file, compiles but is not wired in and untested:** `src/vg/structure.hpp`, `src/vg/structure.cpp`.
  - `BoundingBox`, `Rotation`, `Mirror`, `StructurePiece`, `StructureStart`, `Structure`, `StructureContext`, `WorldRandom` (Mojang's WorldgenRandom over the legacy LCG with `set_large_feature_seed` / `set_large_feature_with_salt`).
  - `StructureState`: loads every `structure_set` and `structure` JSON, placement (random_spread with linear/triangular spread, frequency reducers incl. legacy types 1-3, exclusion zones, concentric rings for strongholds with Mojang's biome search), `starts_at(cx,cz)` (= ChunkGenerator.createStructures, weighted selection loop), `starts_touching(cx,cz)` (= createReferences radius 8).
  - Structure types register via `structure_factories()["minecraft:jigsaw"] = ...`; **none are registered yet**, so no structure is generated yet.
  - Order assumption: structures within a decoration step are visited in alphabetical registry id order (checked against Mojang only by reading code; confirm with a real world).

## Next steps, in order (each verified with refgen `--structures` + gencmp)
1. **Terrain height without building chunks**: port `NoiseBasedChunkGenerator.getBaseHeight/getBaseColumn/iterateNoiseColumn` (26.2 version) into `StructureEnv::base_height/base_column`; fill `StructureEnv` in `src/vg/gen.cpp` (`biome_at` can wrap `Impl::biome_value`, names are `climate.biomes`). Construct `StructureState` in `Impl`.
2. **Structure templates**: read `data/worldgen/26.2/data/minecraft/structure/**/*.nbt` (gzip NBT, `src/nbt.hpp`), `StructureTemplate` placement with rotation/mirror/processors (`structure/templatesystem/*` in Mojang's source: rule/proc lists in `worldgen/processor_list`), block entities and loot tables on chests, jigsaw blocks.
3. **Jigsaw assembler** (`JigsawPlacement`, `pools/*`, `worldgen/template_pool`): villages (5 types), pillager outpost, trail ruins, ancient city, bastion remnant, trial chambers, abandoned camps.
4. **Other structure types**: mineshaft, stronghold, nether fortress, ocean monument, woodland mansion, desert pyramid, jungle temple, igloo, swamp hut, shipwreck, ocean ruins, ruined portals, buried treasure, nether fossil, end city. Classes under `net.minecraft.world.level.levelgen.structure.structures`.
5. **Wire into decoration**: `src/vg/decorate.cpp` line ~104 has the placeholder "(structures of this step would be placed here)". Per step, before features: for each structure of that step (alphabetical), `random.set_feature_seed(decorationSeed, index, step)` (`setFeatureSeed` = seed + index + 10000*step) then every start touching the chunk runs `placeInChunk` (pieces intersecting the chunk's writable box, `postProcess`). Mojang's `ChunkGenerator.applyBiomeDecoration` is the reference.
6. **Beardifier** (terrain adaptation around structures): `density.hpp` has a `Beardifier` forward declaration and `ChunkNoise::beardifier`; implement from Mojang's `Beardifier` and add it to `final_density` as the cache_all_in_cell wrapper does.
7. **Block entities in generation output** (chests with loot, spawners, signs, banners...): `vg::ChunkBlocks` has no block-entity storage; add a side channel and send/save them. Then entities spawned by generation (villagers, mobs in structures).
8. **Gameplay** (model on Cuberite, then Mojang): inventory and containers, drops/tools/damage/health/hunger, entities and mob AI/spawning/physics, fluids flowing, random ticks (crops, leaves, fire), weather, redstone, loot tables/recipes/advancements, `/locate` on the new starts, structure data saved in chunk NBT (`structures.starts/references`).
9. Later: 26.3 engine, default generator = vanilla, perf (parallel chunk generation, pre-generation), fix the 0.9 % decoration-order diff, Bedrock parity, live-service login tests.

## Gotchas
- `rm -rf` may be denied in some harnesses; use fresh dirs. Never `pkill -f` patterns that match your own command line.
- `tests/run_all.sh` needs network (generates a vanilla world first). Test worlds persist between runs, use fresh dirs.
- Debug env vars still in code: PAINITE_TRACE, PAINITE_CELLS, PAINITE_DBG/DBG2/PROF.
