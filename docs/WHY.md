# Why a native rewrite

What limits a Minecraft server, and what Painite does about it. The numbers are from this repository's own tests on an 8 core dev box
(`build/painite_load`); run the same test against Paper on your hardware before drawing conclusions, it prints CPU and memory for any server pid.

| Limit | Painite | Measured |
|---|---|---|
| One 50 ms tick for everything | Block updates sharded per 512x512 region and ticked in parallel; the tick never waits for the network | 60 bots moving every tick: tick avg well under 1 ms, 0.3-0.7% of the 50 ms budget with 20 bots |
| Network handling inside the game thread | One epoll loop per core, pinned, started and retired with load; chunk packets encoded once and shared | 20 bots: the single worker was 6% busy |
| JVM heap and GC pauses | No managed heap, per connection and per chunk buffers with a hard memory cap | idle: 39 MB RSS and about 0.25% of a core; 60 bots roaming: about 220 MB |
| Geyser/Floodgate as a second process | Java and Bedrock in one process, one world, one entity model | `tests/cpp/t_bedrock.cpp` |
| Slow start | Seconds | `Done (0.2s)` on the test worlds |

The dashboard (`./run.sh`) shows these live: tick avg/p99/max against the 50 ms budget, each worker's busy %, KB/s in and out, the time each task
(tick phases, chunk load, light, encode, event delivery) takes, and CPU and RSS per player. F2 shows this page with the current numbers.

What Painite does not have: plugins, mobs, terrain generation, a survival inventory, fluids that flow. It is a base to build those on, not a drop-in Paper replacement.
