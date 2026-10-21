#!/usr/bin/env bash
# First-time setup: builds Painite if needed, creates a ready-to-run server folder and (optionally) registers it in CreeperCrest.
#
#   ./setup.sh                                  server folder ~/painite-server, new normal world, offline mode off
#   ./setup.sh --dir ~/mc/painite --world ~/mc/paper/world --offline --gamemode creative
#   ./setup.sh --creepercrest ~/creepercrest    also add the server to that CreeperCrest (restart it afterwards)
#
# Options:
#   --dir DIR            server folder to create (default ~/painite-server)
#   --world PATH         copy this world folder in (a COPY: never point Painite at a world another server is using)
#   --name NAME          display name in CreeperCrest (default Painite)
#   --id ID              CreeperCrest server id (default painite)
#   --offline            allow cracked/offline clients (online-mode=false, and CreeperCrest will not force it back on)
#   --gamemode MODE      survival|creative|adventure (default survival)
#   --port N             Java port, Bedrock uses the same number over UDP (default 25565)
#   --java 26.2,26.3     Java versions to build protocol data for when data is missing (default 26.2,26.3)
#   --creepercrest DIR   CreeperCrest folder holding config.json (default ~/creepercrest when it exists)
#   --no-creepercrest    do not touch CreeperCrest
#   --no-build           fail instead of building when the binary or data are missing
set -euo pipefail
ROOT=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
DIR="$HOME/painite-server" WORLD="" NAME="Painite" ID="painite" OFFLINE=0 GM=survival PORT=25565 JAVA="26.2,26.3" CC="" NOCC=0 BUILD=1 LEVELTYPE=normal SEED=""
[ -f "$HOME/creepercrest/config.json" ] && CC="$HOME/creepercrest"
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) DIR="${2:?}"; shift ;;
    --world) WORLD="${2:?}"; shift ;;
    --name) NAME="${2:?}"; shift ;;
    --id) ID="${2:?}"; shift ;;
    --offline) OFFLINE=1 ;;
    --gamemode) GM="${2:?}"; shift ;;
    --port) PORT="${2:?}"; shift ;;
    --java) JAVA="${2:?}"; shift ;;
    --creepercrest) CC="${2:?}"; shift ;;
    --no-creepercrest) NOCC=1 ;;
    --no-build) BUILD=0 ;;
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option $1 (try --help)" >&2; exit 2 ;;
  esac
  shift
done
DIR="${DIR/#\~/$HOME}"; [ -n "$CC" ] && CC="${CC/#\~/$HOME}"
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

BIN="$ROOT/build/painite"
if [ ! -x "$BIN" ] || ! ls "$ROOT"/data/vanilla-*.bin >/dev/null 2>&1 || ! ls "$ROOT"/data/blocks-bedrock-*.bin >/dev/null 2>&1; then
  [ "$BUILD" = 1 ] || { echo "binary or protocol data missing; run ./build.sh --data --java $JAVA first" >&2; exit 1; }
  say "building Painite and generating protocol data (downloads Mojang's servers, a few minutes)"
  "$ROOT/build.sh" --data --java "$JAVA"
fi

say "creating the server folder $DIR"
mkdir -p "$DIR"
ln -sfn "$BIN" "$DIR/painite"          # CreeperCrest starts "<folder>/painite nogui"
ln -sfn "$ROOT/data" "$DIR/data"       # protocol data, found as ./data
[ -f "$DIR/painite.toml" ] || cp "$ROOT/painite.toml" "$DIR/painite.toml"
if [ ! -f "$DIR/server.properties" ]; then
  cat > "$DIR/server.properties" <<EOF
server-port=$PORT
motd=A Painite Server
max-players=20
online-mode=$([ "$OFFLINE" = 1 ] && echo false || echo true)
gamemode=$GM
level-name=world
view-distance=10
white-list=false
EOF
  echo "wrote server.properties (online-mode=$([ "$OFFLINE" = 1 ] && echo false || echo true))"
else
  echo "server.properties already exists, left alone"
fi

if [ -n "$WORLD" ]; then
  WORLD="${WORLD/#\~/$HOME}"
  [ -d "$WORLD" ] || { echo "world folder $WORLD does not exist" >&2; exit 1; }
  [ ! -e "$DIR/world" ] || { echo "$DIR/world already exists; move it away first" >&2; exit 1; }
  if command -v fuser >/dev/null 2>&1 && fuser "$WORLD/session.lock" >/dev/null 2>&1; then
    echo "note: $WORLD is open in a running server; the copy may be inconsistent. Stop that server (or use /save-off) for a clean copy."
  fi
  say "copying the world (this can take a while)"
  cp -a "$WORLD" "$DIR/world"
  rm -f "$DIR/world/session.lock"
  echo "world copied. Painite reads Paper/vanilla 26.x worlds; keep your original untouched."
fi

if [ "$NOCC" = 0 ] && [ -n "$CC" ] && [ -f "$CC/config.json" ]; then
  say "registering '$NAME' in $CC/config.json"
  python3 - "$CC/config.json" "$ID" "$NAME" "$DIR" "$OFFLINE" <<'PY'
import json, sys
path, sid, name, d, offline = sys.argv[1:6]
cfg = json.load(open(path))
srv = cfg.setdefault("servers", {})
if sid in srv:
    print(f"server id '{sid}' already exists in CreeperCrest, left alone (use --id to add another)")
else:
    srv[sid] = {"name": name, "directory": d, "jar": "painite", "runtime": "native", "memory_min_mb": 256, "memory_max_mb": 1024,
                "extra_args": "", "autostart": False, "allow_offline": offline == "1"}
    json.dump(cfg, open(path, "w"), indent=2)
    print(f"added '{sid}'")
PY
  echo "CreeperCrest keeps its config in memory: restart it (systemctl --user restart creepercrest, or restart creepercrest.py) to see the new server."
fi

PYTHONPATH="$ROOT/tools" python3 -c 'import sys, painite_servers as p; p.add(sys.argv[1], sys.argv[2], sys.argv[3])' "$ID" "$NAME" "$DIR"   # so ./run.sh lists it

say "done"
echo "server folder : $DIR"
echo "start by hand : cd $DIR && ./painite"
echo "or from CreeperCrest: start '$NAME' in the web UI"
echo "Java and Bedrock both connect to port $PORT (Bedrock over UDP)."
