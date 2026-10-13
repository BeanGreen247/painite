#!/usr/bin/env bash
# Lists your Painite servers (setup-server.sh / setup.sh register them; native servers from CreeperCrest show up too), starts the one you pick
# and shows its log, status (TPS, players, memory, chunks ...), address and MOTD. Type console commands at the bottom.
exec python3 "$(dirname "$(readlink -f "$0")")/tools/painite_tui.py" "$@"
