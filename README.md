# Painite

A fast, low-resource Minecraft server for Linux, written in C++. Java and Bedrock clients
connect natively, with no Geyser or other addon.

Status: design stage. See `docs/DESIGN.md` and `painite.toml`.

Planned: region-sharded autoscaling workers under a total memory cap, core selection via config
file (CLI flags override), io_uring I/O, adaptive bandwidth for high-ping or proxied links,
built-in anti-xray, vanilla-style console commands.

Companion web manager: [CreeperCrest](https://github.com/BeanGreen247/creepercrest).

## License

Source-available, all rights reserved. Read, build and run for personal use; no forks,
redistribution or derivative works. See `LICENSE`.
