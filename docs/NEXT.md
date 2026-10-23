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
- `roundtrip/mojang-reads-painite-world` failed once in a full serial run ("Mojang's server started on the saved world") and passes alone.
- `survival/bow-and-snowball` failed once inside a full serial run and passes alone (not reproduced).
- The two-bots tests fail when the whole suite runs in parallel (`tests/run_all.sh` without `--serial`); they pass alone and serially. Older binaries fail the same way, the cause was not found (probably interference between test groups).
- The 26.1 tests fail because `data/vanilla-26.1.ids` is not built here; harmless.
- Light is estimated (open sky by day, light sources within 3 blocks): there is no lighting engine.
- Tree growth uses simple shapes of my own, not Mojang's tree features; dark oak, mangrove and azalea saplings do nothing yet.
- Mob walking speed is derived from Mojang's formulas and has not been compared with a real server.
- Players are trusted on mining time (no dig-speed check) and on movement.

## To assess tomorrow, ranked by my guess of value (the owner decides the order)
1. Fluids are done except: dispensers with buckets (wait for containers), milk buckets (no effects system yet), flowing water entering waterlogging blocks, seagrass growth. Built (`src/fluids.cpp`): FlowingFluid tick, spread and slope search for water and lava (Nether rates), infinite water, lava meeting water, buckets (water, lava, powder snow), cauldrons, waterlogging on place and dig, bubble columns (blocks and entity push), flow pushing items and mobs, mob drowning (`air_supply`), kelp growth, lava starting fires. Tests: `redstone/fluids`, `redstone/fluids-more`, flow and drowning checks in `tests/sim/simtest.cpp`. The region ticker keeps the earliest due time per position, so a neighbour change can wake a block sooner than its queued tick.
2. Containers and crafting, remaining: chest lid animation (block event), grindstone, stonecutter, smithing table, loom, cartography table, villager trades, crafting recipe types transmute/dye/special (armor dye, fireworks, banners, maps), crafter block, composter, jukebox, lectern, chiseled bookshelf, barrel/chest open sounds. Built: `src/sim/recipe.*` (shaped, shapeless, cooking recipes from the data pack, fuel values), inventory 2x2 and crafting table windows, `src/containers.cpp` (chest/double chest/barrel/shulker/dispenser/dropper/hopper windows, items in the chunk's block entity list, loot-table chests roll on first open, drops when broken, furnaces/blast furnaces/smokers with fuel and progress data, hoppers moving items and sucking item entities, dropper and dispenser behaviours incl. buckets, fire, TNT, projectiles). Also built (2026-10-23): player experience (`Vitals.xp_*`, `set_experience`, saved as XpLevel/XpP/XpTotal, lost on death without orbs), furnaces store the experience of what they smelt and pay it out when the result is taken, ender chests (per player, `EnderItems`), the enchanting table (`src/sim/enchant.*`, `src/sim/workstation.*`: offers from the vanilla formulas with the player's XpSeed, bookshelf count, lapis and levels, `container_button_click`), the anvil (repair with material, combine, enchant merge, rename via `rename_item`, level cost, 12 % chip), brewing stands (`src/sim/brewing.*`: bottles, ingredient, blaze fuel, vanilla recipes, splash/lingering, has_bottle properties). Menu data slots are now sent after the content. Tests: `survival/crafting-table`, `survival/containers` (furnace XP, ender chest), `survival/workstations`, `survival/brewing`, `survival/hoppers-droppers`, brew and anvil unit checks in `tests/sim/simtest.cpp`. Not done here: grindstone, stonecutter, smithing, loom, cartography, villager trades, XP orbs (xp goes straight to the player), mending, hoppers feeding brewing stands, potion effects on drinking.
3. Remaining redstone parts: sculk sensors, lightning rods, rails and minecarts, command blocks, copper bulbs, crafter, quasi-connectivity for pistons and doors (only dispensers and droppers use it), comparator side-input edge cases, observer state after a restart. Built: tripwires and hooks (`Game::tripwire_tick`, `Redstone::calc_hook`; entities powering them, shears disarm), piston animation (Java clients animate from `block_event`, the server keeps `moving_piston` blocks for three ticks and settles them in `Redstone::tick_moving`; Bedrock gets the final blocks at once; entities are not carried yet), observer, comparator (compare/subtract, containers, cauldrons, cake...), daylight sensor (+invert), target block (arrows), note block (sounds via `Game::play_sound`), weighted pressure plates, torch burnout, slime and honey blocks with pistons.
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
