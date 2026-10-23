# Where Painite stands and what is next (end of 2026-10-21)

Read with `docs/HANDOFF.md` (worldgen port, versions, verification) and `docs/AI_HANDOFF.md` (entity notes). The owner's rules: no AI attribution lines in commit messages, push only when asked, no new Python tools (C++ and shell), measure before optimizing, after each task report what is done and what is left.

## Built today (tests in `tests/cpp/t_java_game.cpp`, groups `survival`, `redstone`, `persist`; unit tests in `tests/sim/simtest.cpp`)
- Server: worldgen fixes for 26.3 (weighted selectors, dual-noise variety lists), a missing climate dump falls back to the built-in generator instead of crashing, gameplay data follows the newest `data/game/<ver>`, setup asks which worldgen version to follow, the inventory is kept through dimension changes.
- Commands: `painite <cmd>` / `pn <cmd>` prefix and listing, `/summon`, `/kill`, `/gamerule randomTickSpeed`, `cache` (every cache with entries, size, limit, hits, misses, hit rate, coloured), `perf`.
- Entities: falling blocks, fluid height, items, primed TNT, arrows, thrown snowballs, eggs and ender pearls.
- Survival: block drops from loot tables with tool rules, durability, pickup, placing consumes the held stack, health/hunger/damage/death/respawn (`src/vitals.cpp`), eating, armor, bows.
- Mobs (`src/sim/mob.*`, `src/mobs.cpp`, `src/sim/path.*`): living entities, A* pathfinding, wandering animals that flee, melee monsters, creepers (explosion with cover and drops), skeletons, burning in daylight, natural spawning (surface, caves, Nether, biome spawn lists), despawning, mobs saved in `painite_entities_<dim>.nbt`.
- Redstone (`src/redstone.*`): dust networks, lever, button, pressure plate, redstone block, torch, repeater, lamp, door/trapdoor/gate, piston/sticky piston, TNT.
- Random ticks (`src/blocktick.cpp`): crops, stems, cane, cactus, nether wart, saplings to simple trees, grass/mycelium spread, leaf distance and decay, farmland, fire.
- Performance (stress scene `PAINITE_PERF=1 build/painite_tests --filter perf-mobs --verbose`, 150 zombies + 150 cows): tick 9.4 ms -> about 3 ms (random ticks add about 0.5 ms), 72 MB resident.

## Known problems
- The two-bots tests fail when the whole suite runs in parallel (`tests/run_all.sh` without `--serial`); they pass alone and serially. Older binaries fail the same way, the cause was not found (probably interference between test groups).
- The 26.1 tests fail because `data/vanilla-26.1.ids` is not built here; harmless.
- Light is estimated (open sky by day, light sources within 3 blocks): there is no lighting engine.
- Tree growth uses simple shapes of my own, not Mojang's tree features; dark oak, mangrove and azalea saplings do nothing yet.
- Mob walking speed is derived from Mojang's formulas and has not been compared with a real server.
- Players are trusted on mining time (no dig-speed check) and on movement.

## To assess tomorrow, ranked by my guess of value (the owner decides the order)
1. Fluids, remaining: flow pushing entities, drowning of mobs, lava starting fires and random-tick behaviour, bubble columns, kelp and seagrass growth, cauldrons, dispensers with buckets, powder snow and milk buckets, flowing water entering waterloggable blocks. Built (`src/fluids.cpp`): FlowingFluid tick, spread and slope search for water and lava (Nether rates), infinite water, lava meeting water (obsidian, cobblestone, stone, basalt), empty/water/lava buckets, waterlogging on place and dig; test `redstone/fluids`. The region ticker now keeps the earliest due time per position, so a neighbour change can wake a block sooner than its queued tick.
2. Containers and crafting: chests and other block entities, crafting table and recipes, furnaces, hoppers, droppers and dispensers (the inventory and menu code in `src/sim/inventory.*` exists, recipes are not ported).
3. Remaining redstone parts: observer, comparator, note block, daylight sensor, tripwire, target block, slime and honey, piston animation, quasi-connectivity, torch burnout.
4. More world behaviour: bone meal, vines, bamboo, kelp, snow, ice, weather and lightning, lava fire spread, copper, vanilla tree shapes.
5. Mob behaviour: villagers, piglins, endermen, wolves, bees, breeding and taming, shields and enchantments, light-level spawning once there is a light engine, End spawning.
6. Player rules: mining-time check, enchantments, XP, advancements, game rules other than randomTickSpeed, bed spawn and respawn in the right dimension.
7. Bedrock parity: mobs, items, survival inventory (ItemStackRequest), redstone visuals.
8. Performance and caching (the owner asked for this later): a shared flow field from each player for hostile mobs, path reuse while the target barely moves, updating far or idle entities less often, caching biome spawn tables and loot lookups. Measure first; the `cache` and `perf` commands and the stress test exist for that. Also a last RAM and performance pass at the end.
9. Older open items from the handoff: structure starts in chunk NBT, decoration order (0.9 % diff), 26.3 stage checks for the Nether and End.

## How to resume
```
cd ~/git/painite && git pull
cmake -S . -B build && cmake --build build -j8
tests/run_all.sh --serial          # expect only the two 26.1 failures
```
