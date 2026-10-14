#!/usr/bin/env bash
# The whole end-to-end suite: unit tests, then painite_tests (C++, every group starts its own server; groups run in parallel).
#   tests/run_all.sh [--filter text] [--verbose] [--serial]      other options go to painite_tests
# Needs: data/ (./build.sh --data), a Java runtime and network the first time (Mojang's server builds the reference world, cached in build/test-world;
# delete that folder to rebuild it, or set PAINITE_TEST_WORLD to use another folder holding world/ and vanilla_chunks.bin).
set -u
cd "$(dirname "$(readlink -f "$0")")/.."
ROOT=$PWD

cmake -S . -B build >/dev/null && cmake --build build --target painite painite_unit painite_tests -j"$(nproc)" 2>&1 | grep -E 'error|Error' && { echo "build failed"; exit 1; }
if ! build/painite_unit | grep -q '^OK'; then echo "FAIL  unit tests"; build/painite_unit | tail -5; exit 1; fi
echo "PASS  unit tests"

TW=${PAINITE_TEST_WORLD:-$ROOT/build/test-world}
STAMP=v2-$(cat data/vanilla-*.ids 2>/dev/null | cksum | cut -d' ' -f1)
if [ ! -d "$TW/world" ] || [ "$(cat "$TW/stamp" 2>/dev/null)" != "$STAMP" ]; then
  echo "building the reference world with Mojang's server (once) ..."
  mkdir -p "$TW.new"
  (cd "$TW.new" && python3 -I "$ROOT/tools/make_test_world.py" --out "$TW.new" --cmd "setblock -640 100 -480 minecraft:chest" --cmd "setblock -639 100 -480 minecraft:oak_sign" >/dev/null 2>&1) || { echo "could not generate a test world"; exit 1; }
  echo "$STAMP" > "$TW.new/stamp"
  mv "$TW" "$TW.old.$$" 2>/dev/null; mv "$TW.new" "$TW"; rm -rf "$TW.old.$$"
fi
[ -f data/steve-skin.json ] || curl -sfL -o data/steve-skin.json https://raw.githubusercontent.com/PrismarineJS/minecraft-data/master/data/bedrock/1.21.70/steve.json

exec build/painite_tests --world "$TW" --binary build/painite --data data "$@"
