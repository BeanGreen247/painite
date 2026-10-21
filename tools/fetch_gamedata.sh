#!/usr/bin/env bash
# Copies the gameplay data (loot tables, recipes, tags, enchantments, damage types, item components, block and entity facts) out of Mojang's server jar
# into data/game/<version>/. Needs tools/fetch_worldgen.sh to have downloaded the jar, and JDK 25 on PATH (see docs/HANDOFF.md).
# usage: tools/fetch_gamedata.sh [VERSION]      (default 26.3)
set -eu
cd "$(dirname "$(readlink -f "$0")")/.."
v=${1:-26.3}
out=data/game/$v
[ -f "$out/.done" ] && { echo "$v: already there"; exit 0; }
jar=build/mojang/server-$v.jar
[ -f "$jar" ] || { echo "$v: run tools/fetch_worldgen.sh $v first" >&2; exit 1; }
tools/oracle_cp.sh "$v"
rm -rf "$out"; mkdir -p "$out"
unzip -q -o build/mojang/inner-$v.jar 'data/minecraft/loot_table/*' 'data/minecraft/recipe/*' 'data/minecraft/tags/*' 'data/minecraft/enchantment/*' 'data/minecraft/damage_type/*' \
  'data/minecraft/enchantment_provider/*' 'data/minecraft/villager_trade/*' 'data/minecraft/trade_set/*' 'data/minecraft/advancement/*' 'data/minecraft/banner_pattern/*' \
  'data/minecraft/jukebox_song/*' 'data/minecraft/instrument/*' 'data/minecraft/painting_variant/*' 'data/minecraft/trial_spawner/*' -d "$out"
tmp=$(mktemp -d)
(cd "$tmp" && java -DbundlerMainClass=net.minecraft.data.Main -jar "$OLDPWD/$jar" --reports --output gen >/dev/null 2>&1)
cp -r "$tmp/gen/reports" "$out/reports"
rm -rf "$tmp"
(cd build/mojang && java -Xshare:auto -XX:TieredStopAtLevel=1 -cp "$(cat "cp-$v.txt")" ../../tools/oracle/DumpGameplay.java "../../$out/blockprops.txt" "../../$out/entityprops.txt" >/dev/null 2>&1) \
  || echo "$v: could not dump the block and entity facts" >&2
(cd build/mojang && java -Xshare:auto -XX:TieredStopAtLevel=1 -cp "$(cat "cp-$v.txt")" ../../tools/oracle/DumpShapes.java "../../$out/blockshapes_states.txt" "../../$out/blockshapes.txt" >/dev/null 2>&1) \
  || echo "$v: could not dump the collision shapes" >&2
touch "$out/.done"
echo "$v: $(find "$out" -type f | wc -l) files in $out"
