#!/usr/bin/env bash
# Copies Mojang's world generation data (noise, density functions, biomes, features, structures, ...) out of the server jar into data/worldgen/<version>/.
# usage: tools/fetch_worldgen.sh [VERSION ...]     (default: every version that has data/vanilla-<version>.ids)
# The jars are cached in build/mojang/.
set -eu
cd "$(dirname "$(readlink -f "$0")")/.."
mkdir -p build/mojang data/worldgen
MANIFEST=https://piston-meta.mojang.com/mc/game/version_manifest_v2.json
versions=("$@")
if [ ${#versions[@]} -eq 0 ]; then for f in data/vanilla-*.ids; do v=${f#data/vanilla-}; versions+=("${v%.ids}"); done; fi
for v in "${versions[@]}"; do
  out=data/worldgen/$v
  if [ -f "$out/.done" ]; then echo "$v: already there"; continue; fi
  jar=build/mojang/server-$v.jar
  if [ ! -f "$jar" ]; then
    url=$(curl -sfL "$MANIFEST" | jq -r --arg v "$v" '.versions[] | select(.id==$v) | .url')
    [ -n "$url" ] || { echo "$v: not in the manifest" >&2; exit 1; }
    sj=$(curl -sfL "$url" | jq -r '.downloads.server.url')
    echo "$v: downloading the server jar"
    curl -sfL -o "$jar" "$sj"
  fi
  tmp=$(mktemp -d)
  unzip -q -o "$jar" 'META-INF/versions/*/server-*.jar' -d "$tmp" 2>/dev/null || true
  inner=$(ls "$tmp"/META-INF/versions/*/server-*.jar 2>/dev/null | head -1 || true)
  [ -n "$inner" ] || inner=$jar
  rm -rf "$out"
  mkdir -p "$out"
  unzip -q -o "$inner" 'data/minecraft/worldgen/*' 'data/minecraft/tags/worldgen/*' 'data/minecraft/dimension_type/*' 'data/minecraft/tags/block/*' 'data/minecraft/tags/fluid/*' 'data/minecraft/structure/*' -d "$out"
  touch "$out/.done"
  rm -rf "$tmp"
  echo "$v: $(find "$out" -name '*.json' | wc -l) files in $out"
done
