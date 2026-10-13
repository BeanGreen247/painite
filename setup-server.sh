#!/usr/bin/env bash
# Interactive setup: asks for name, folder, port, MOTD, players, game mode, online mode, world to import, Bedrock, CPU settings and
# CreeperCrest registration; builds Painite first if needed. For a non-interactive version see setup.sh.
exec python3 "$(dirname "$(readlink -f "$0")")/tools/setup_server.py" "$@"
