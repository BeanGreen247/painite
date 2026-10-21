# AI continuation notes

> Superseded for the current state by `docs/NEXT.md` (2026-10-21): pickup, drops, health, mobs, redstone and random ticks are all implemented now; the "Current boundaries" list below is historical.

Updated 2026-10-15 after entity simulation wiring (`e16ce9c`). This is an implementation-oriented handoff for coding assistants. `docs/HANDOFF.md` remains the broader project status and verification reference; `docs/COPILOT_PROMPT.md` records the owner's project rules and ordered gameplay plan.

## Start here

1. Read `docs/COPILOT_PROMPT.md`, then `docs/HANDOFF.md`.
2. Check `git status --short --branch` before editing; keep unrelated user changes intact.
3. For entity work, trace the current code in `src/sim/entity.hpp`, `src/sim/entity_manager.hpp`, `src/game.hpp`, `src/game.cpp`, and `src/java_game.cpp` before changing it.
4. Use the local generated data in `data/game/26.2` when available. It is git-ignored and includes `blockprops.txt`, `entityprops.txt`, `blockshapes_states.txt`, and `blockshapes.txt`.

## Entity milestone that is implemented

- `Game::start` creates one `sim::EntityManager` per dimension. The managers implement `sim::EntityWorld` over the corresponding `World`.
- `EntityManager` owns entities, allocates runtime ids and UUIDs, attaches each entity to its manager, applies dimensions from `EntityTable`, and ticks entities on the game thread.
- `Game::add_entity(dim, ...)` queues entity creation onto the game thread. It is the entry point gameplay code should use; do not mutate a manager from a worker/network thread.
- World queries use already-loaded chunks only. Unloaded chunks behave as air for collision and do not tick entities. This avoids entity simulation causing chunk loads.
- `BlockCollisionShapes` maps the oracle's 26.2 block-name/property table to Painite's internal block-state ids. `EntityManager` uses it for collision boxes and full-block checks, and gets friction from `BlockTable`.
- `Game::tick` tracks entity visibility for Java players using the entity's `EntityTable` tracking range and viewer view distance. It sends add/remove, relative movement/rotation, absolute position sync, velocity, and metadata events. Disconnecting viewers are removed from entity tracking.
- `Worker::java_event` encodes those events and has an encoder for the take-item packet (`EntityTake`), but nothing currently produces that event. The `LpVec3` codec is used for velocity in add-entity and set-entity-motion packets; metadata values are the wire bytes returned by `Entity::collect_data`.
- `painite_simtest` loads the shape oracle output and checks solid-block and top-slab geometry. CMake globs `src/sim/*.cpp` into the server; the lightweight simtest excludes `entity_manager.cpp` because it depends on `World`.

## Current boundaries (do not assume these are done)

- No production path currently creates item entities or imports `Chunk::gen_entities`; entity manager creation/ticking and Java tracking are infrastructure only.
- Entity persistence and chunk save/load are not implemented. Entities are lost on restart and do not move between chunks.
- Bedrock entity add/move/remove packets are not wired. The game tracker currently omits Bedrock viewers.
- `EntityTake` packet encoding exists but has no producer. Player pickup, collision with players, and inventory collection are not implemented.
- Entity simulation currently ticks only when the containing chunk is already loaded. There is no chunk activity policy or entity persistence tie-in.
- No server command for `/summon` is connected to the entity manager.
- Fluid-height queries still use `EntityWorld`'s default dry response. Do not treat water/lava item motion as verified.
- Collision shapes require the generated 26.2 gameplay data. Startup warns if the files are absent; retrieve them using the project setup workflow in `docs/HANDOFF.md` rather than adding generated data to Git.

## Next gameplay work

Follow the order in `docs/COPILOT_PROMPT.md`. The next gameplay milestone is survival digging and drops:

1. Read `Worker::on_dig` / `Worker::on_place` and `ItemType::tool_rules`, `BlockTable`, `LootTables`, `PlayerSim`, and the existing game-thread task/event patterns.
2. Port Mojang `ServerPlayerGameMode` destroy progress and `Player.getDestroySpeed` behavior, including tool correctness, hardness, creative mode, and relevant packet acknowledgements.
3. For valid drops, construct `sim::ItemEntity` instances and call `Game::add_entity` rather than touching an `EntityManager` from the worker. Keep Mojang's loot RNG and call order.
4. Add pickup as a separate behavior: validate range, delay, stack/component compatibility and capacity; mutate inventory on the game thread, emit `EntityTake`, update entity metadata, and discard only when empty.
5. Make placing consume the selected stack outside infinite-material modes and apply tool damage where the port requires it.
6. Add focused simulation/integration tests and update `docs/HANDOFF.md` when behavior is actually complete. Verify both Java protocols where packet behavior is shared.

Then continue with the prompt's loot-table comparison and missing loot functions, followed by item-physics packet comparison. Preserve existing behavior outside the targeted feature.

## Verification used for the entity milestone

```sh
cmake -S . -B build
cmake --build build --target painite painite_simtest painite_tests -j2
build/painite_simtest data/game/26.2
build/painite_tests --filter two-bots --serial
git diff --check
```

The entity-wiring change was committed as `e16ce9c`; the sim test and Java 26.2/26.3 two-bot tests passed. These tests cover existing gameplay regressions and shape mapping, not a live item-entity spawn/tick/pickup flow (no production spawn path is connected yet).
