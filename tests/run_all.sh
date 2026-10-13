#!/usr/bin/env bash
# Runs the whole test suite against a freshly started Painite built in ./build.
#   tests/run_all.sh [WORLD_DIR_WITH_chunks]    WORLD_DIR holds world/ and vanilla_chunks.bin from tools/make_test_world.py;
#                                               it is generated (downloads Mojang's server) when not given.
# Needs: build/painite (./build.sh), data/ (./build.sh --data), python3 with the 'cryptography' package.
set -u
cd "$(dirname "$(readlink -f "$0")")/.."
ROOT=$PWD
WORK=$(mktemp -d)
trap 'kill $SRV 2>/dev/null; rm -rf "$WORK"' EXIT
fail=0
run() { printf '== %s\n' "$*"; if ! "$@" 2>&1 | tail -3; then :; fi; }
check() { local name=$1; shift; if "$@" >"$WORK/out.txt" 2>&1; then printf 'PASS  %s\n' "$name"; else printf 'FAIL  %s\n' "$name"; tail -15 "$WORK/out.txt"; fail=1; fi; }

check "unit tests" bash -c "cmake --build build --target painite_unit >/dev/null && build/painite_unit | grep -q '^OK'"

TW=${1:-}
if [ -z "$TW" ]; then
  TW=$WORK/world-src
  mkdir -p "$TW"
  (cd "$TW" && python3 -I "$ROOT/tools/make_test_world.py" --out "$TW" --cmd "setblock -640 100 -480 minecraft:chest" --cmd "setblock -639 100 -480 minecraft:oak_sign" >/dev/null 2>&1) || { echo "could not generate a test world"; exit 1; }
fi
mkdir -p "$WORK/srv"
cp -r "$TW/world" "$WORK/srv/world"
PORT=28300
printf 'server-port=%s\nonline-mode=false\nlevel-name=world\ngamemode=creative\nmax-players=200\n' $PORT > "$WORK/srv/server.properties"
python3 - <<PY
import hashlib, json
def uuid(n):
    h = bytearray(hashlib.md5(("OfflinePlayer:" + n).encode()).digest()); h[6] = (h[6] & 15) | 0x30; h[8] = (h[8] & 0x3f) | 0x80
    u = h.hex(); return f"{u[:8]}-{u[8:12]}-{u[12:16]}-{u[16:20]}-{u[20:]}"
json.dump([{"uuid": uuid(n), "name": n, "level": 4, "bypassesPlayerLimit": False} for n in ("Bobby", "Alice")], open("$WORK/srv/ops.json", "w"))
PY
(cd "$WORK/srv" && exec "$ROOT/build/painite" > "$WORK/srv/out.log" 2>&1) & SRV=$!
for _ in $(seq 100); do (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.2; done

check "chunks equal Mojang's (blocks, biomes, light layout)"   python3 -I tests/world_compare.py --port $PORT --chunks "$TW/vanilla_chunks.bin"
check "Java smoke 26.3"                                         python3 -I tests/smoke.py --port $PORT
[ -f data/vanilla-26.2.ids ] && check "Java smoke 26.2"         python3 -I tests/smoke.py --port $PORT --protocol 776 --ids data/vanilla-26.2.ids
check "block entities"                                          python3 -I tests/block_entity_test.py --port $PORT --expect 2
check "Bedrock smoke"                                           python3 -I tests/bedrock_smoke.py --port $PORT --name BedSmoke
check "two Java bots: entities, blocks, commands (26.3)"        python3 -I tests/game_test.py --port $PORT
[ -f data/vanilla-26.2.ids ] && check "two Java bots (26.2)"    python3 -I tests/game_test.py --port $PORT --protocol 776 --ids data/vanilla-26.2.ids
check "Java and Bedrock together"                               python3 -I tests/crossplay_test.py --port $PORT
check "bans, whitelist, server icon"                            python3 -I tests/lists_test.py --port $PORT --dir "$WORK/srv"
check "Java online mode (fake session server)"                  python3 -I tests/online_test.py
check "Bedrock Xbox login (fake authorization service)"         python3 -I tests/xbox_test.py
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
check "Mojang's server loads what Painite saved"                python3 -I tests/vanilla_roundtrip.py --world "$TW/world"
[ $fail = 0 ] && echo "all tests passed" || echo "SOME TESTS FAILED"
exit $fail
