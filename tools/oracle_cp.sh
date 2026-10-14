#!/usr/bin/env bash
# Prepares build/mojang/cp-<version>.txt: the class path (inner server jar + bundled libraries) the oracle tools in tools/oracle/ run on.
# usage: tools/oracle_cp.sh VERSION      (needs build/mojang/server-<version>.jar, see tools/fetch_worldgen.sh)
set -eu
cd "$(dirname "$(readlink -f "$0")")/../build/mojang"
v=$1
[ -f "cp-$v.txt" ] && exit 0
tmp=$(mktemp -d)
unzip -q -o "server-$v.jar" 'META-INF/versions/*/server-*.jar' 'META-INF/libraries/*' -d "$tmp"
cp "$tmp"/META-INF/versions/*/server-*.jar "inner-$v.jar"
rm -rf "libs${v//./}"
mkdir -p "libs${v//./}"
mv "$tmp/META-INF" "libs${v//./}/META-INF"
rm -rf "$tmp"
cp_line="inner-$v.jar"
for j in $(find "libs${v//./}/META-INF/libraries" -name '*.jar' | sort); do cp_line="$cp_line:$j"; done
echo "$cp_line" > "cp-$v.txt"
