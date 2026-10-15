# Prompt for GitHub Copilot (or any other coding assistant) taking over Painite

Paste everything below the line into the assistant. Keep `docs/HANDOFF.md` open next to it.

---

You are continuing work on **Painite**, a C++20 Minecraft server (Java Edition 26.x first; Bedrock parity is out of scope for now) in this repository (`main` branch). The owner's goal is **one-to-one parity with vanilla Minecraft**: world generation (byte-exact where possible), what clients see, and gameplay. Where Mojang's behaviour is unclear, read Mojang's decompiled code (see below); Cuberite is a secondary, readable reference for gameplay structure.

## Read first
1. `docs/HANDOFF.md` (state of the code, environment, verification workflow, next steps). It is the source of truth.
2. `README.md`, `docs/DESIGN.md`.
3. Memory bank note `~/claude-memory-bank/projects/painite.md` if present.

## Rules from the owner (do not break)
- Commits: one line, conventional style `type(scope): message`. **No Co-Authored-By trailers and no AI attribution lines of any kind.**
- **Push `main` to origin at the end of every session** (and after big milestones) so no work is lost.
- Do not propose or perform repo cleanup (README refreshes, sanitizer or fuzz runs, reformatting) unless asked. Do not test against live Mojang/Microsoft login services until everything else is implemented.
- Never call paid or metered APIs. Never call the Anthropic API from code.
- Keep working without asking questions; choose sensible defaults and record decisions in `docs/HANDOFF.md`.

## Environment
- Linux, CMake. `cmake -S . -B build && cmake --build build --target painite -j2` (about 90 s). Other targets: `painite_simtest` (gameplay core unit checks, fast), `painite_gencmp` / `painite_refgen` (world generation comparison against Mojang's own server), `painite_tests` (end-to-end bots; some groups need a reference world and network).
- Data is git-ignored. Needed: `data/vanilla-26.2.*` (`tools/fetch_vanilla_data.py`), `data/worldgen/26.2` (`tools/fetch_worldgen.sh 26.2`), `data/game/26.2` (`tools/fetch_gamedata.sh 26.2`). Mojang 26.x jars are Java 25 class files: install Temurin 25 and put its `bin` first on `PATH` before running any of those tools or `tools/oracle/*.java`.
- Mojang's source for reference: decompile `build/mojang/inner-26.2.jar` with CFR (`java -jar cfr.jar inner-26.2.jar --outputdir out`). Always use **26.2**, not 26.3.
- Never `pkill -f` a pattern that matches your own command line.

## What exists
- `src/vg/`: bit-exact port of Mojang's world generation including all structures (jigsaw, code-built pieces), verified piece-for-piece against Mojang's server for every structure type. Verification tools: `painite_refgen`, `painite_gencmp` (workflow in HANDOFF).
- `src/sim/` (new, namespace `pn::sim`): protocol-independent gameplay core ported from Mojang: items and stacks, containers and menus (inventory click logic), loot tables, tags, block/entity fact tables, collision (`phys.*`), entity base and `ItemEntity` (`entity.*`; compiles, not yet wired into the server). Tests: `tests/sim/simtest.cpp`.
- Server wiring so far: per-player `PlayerSim` in `Game`, container packets (click, close, creative slot, held slot) and their events, inventory persisted in player files, `/locate` through `vg`.
- The old, approximate world generator and structures (`src/worldgen*.cpp`, `src/structures*.cpp`) are only used when `generator` is not `vanilla`.

## Do next, in this order
1. **Entity system wiring**: a per-dimension entity manager implementing `sim::EntityWorld` over `World` (block collision boxes from `data/game/26.2/blockshapes*.txt` keyed by block name + properties; friction from `BlockTable`), ticked from `Game::tick`; entity tracking and packets ported from `ServerEntity.sendChanges` (add_entity, set_entity_data, move_entity_pos/pos_rot/rot, entity_position_sync, set_entity_motion with LpVec3, take_item_entity, remove_entities); new `GEvent` kinds encoded in `Worker::java_event` (`src/java_game.cpp`). Entity data serializer ids: BYTE 0, INT 1, LONG 2, FLOAT 3, STRING 4, COMPONENT 5, OPTIONAL_COMPONENT 6, ITEM_STACK 7, BOOLEAN 8 ...
2. **Survival digging and drops**: port `ServerPlayerGameMode` (destroy progress, `Player.getDestroySpeed`, tool rules via `ItemType::tool_rules`, `BlockTable` hardness), block drops through loot tables (`blocks/<name>`), `popResource`, item pickup (`ItemEntity.playerTouch`), placing consumes the held item, tool damage. Replace the creative-only code in `Worker::on_dig` / `on_place`.
3. Verify loot tables against Mojang: open the same structure chests (seeded with `LootTableSeed`) on a reference world and on Painite with a bot and compare. Then finish the missing loot functions (`enchant_randomly`, `enchant_with_levels`, `furnace_smelt`, `copy_components`, `exploration_map`).
4. Verify item physics against Mojang: `/summon` an item with motion on the reference server, record entity packets for 40 ticks, compare with Painite.
5. Health, hunger, damage, death/respawn; recipes and the crafting grid (`CraftingProvider`); then mobs and AI, fluids, random ticks, redstone, weather, `/locate` data in chunk NBT, 26.3 engine, performance.

## How to work
- Port Mojang's logic faithfully (same random call order, same formulas); the Java is the specification. Prefer small commits, each compiled and tested (`painite_simtest`, relevant `painite_tests` groups). Update `docs/HANDOFF.md` when you finish a milestone, then commit and push `main`.
