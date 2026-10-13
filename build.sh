#!/usr/bin/env bash
# Painite build script: installs build dependencies, then compiles tuned for THIS machine.
# Run it on the machine that will host the server; -march=native binaries are not portable to older CPUs.
#
#   ./build.sh                    install deps, build (Release, -O3, -march=native, LTO)
#   ./build.sh --pgo              also run profile-guided optimisation (extra ~3 min; fetches protocol data if missing)
#   ./build.sh --data             also generate the Java and Bedrock protocol data (downloads Mojang's servers)
#   ./build.sh --prefix /opt/painite   install the binary (bin/) and data (data/) there after building
#   ./build.sh --no-deps          skip the package installation step
#   ./build.sh --check            only report missing dependencies
#   ./build.sh --jobs N           parallel compile jobs (default: all cores)
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"
ROOT=$PWD
BUILD="$ROOT/build"
DEPS=1 PGO=0 DATA=0 CHECK=0 PREFIX="" JOBS="$(nproc 2>/dev/null || echo 2)"

while [ $# -gt 0 ]; do
  case "$1" in
    --pgo) PGO=1 ;;
    --data) DATA=1 ;;
    --no-deps) DEPS=0 ;;
    --check) CHECK=1 ;;
    --prefix) PREFIX="${2:?--prefix needs a path}"; shift ;;
    --jobs) JOBS="${2:?--jobs needs a number}"; shift ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---- package names per distribution family --------------------------------------------------------------
detect_pm() {
  if have apt-get; then echo apt
  elif have dnf; then echo dnf
  elif have yum; then echo yum
  elif have pacman; then echo pacman
  elif have zypper; then echo zypper
  elif have apk; then echo apk
  else echo none; fi
}
PM=$(detect_pm)
case "$PM" in
  apt)    PKGS="build-essential cmake pkg-config zlib1g-dev libssl-dev python3 python3-cryptography curl unzip ca-certificates git"
          INSTALL="apt-get install -y --no-install-recommends"; UPDATE="apt-get update" ;;
  dnf|yum) PKGS="gcc-c++ make cmake pkgconf-pkg-config zlib-devel openssl-devel python3 python3-cryptography curl unzip ca-certificates git"
          INSTALL="$PM install -y"; UPDATE="" ;;
  pacman) PKGS="base-devel cmake pkgconf zlib openssl python python-cryptography curl unzip ca-certificates git"
          INSTALL="pacman -S --needed --noconfirm"; UPDATE="pacman -Sy" ;;
  zypper) PKGS="gcc-c++ make cmake pkg-config zlib-devel libopenssl-devel python3 python3-cryptography curl unzip ca-certificates git"
          INSTALL="zypper --non-interactive install"; UPDATE="zypper --non-interactive refresh" ;;
  apk)    PKGS="build-base cmake pkgconf zlib-dev openssl-dev python3 py3-cryptography curl unzip ca-certificates git"
          INSTALL="apk add"; UPDATE="apk update" ;;
  *)      PKGS=""; INSTALL=""; UPDATE="" ;;
esac

missing=()
have g++ || missing+=("a C++ compiler (g++)")
have cmake || missing+=("cmake")
have make || missing+=("make")
have python3 || missing+=("python3")
have curl || missing+=("curl")
echo '#include <zlib.h>' | ${CXX:-g++} -E -x c++ - >/dev/null 2>&1 || missing+=("zlib development headers")
echo '#include <openssl/evp.h>' | ${CXX:-g++} -E -x c++ - >/dev/null 2>&1 || missing+=("OpenSSL development headers")
python3 -c 'import cryptography' >/dev/null 2>&1 || missing+=("python3 'cryptography' (only needed for tests and tools/bedrock_capture.py)")

if [ "$CHECK" = 1 ]; then
  if [ ${#missing[@]} -eq 0 ]; then echo "all dependencies present"; else printf 'missing: %s\n' "${missing[@]}"; fi
  exit 0
fi

if [ "$DEPS" = 1 ]; then
  if [ "$PM" = none ]; then
    say "no supported package manager found; install these yourself: a C++20 compiler, cmake, make, zlib and OpenSSL headers, python3"
  elif [ ${#missing[@]} -gt 0 ]; then
    say "installing build dependencies with $PM"
    SUDO=""
    if [ "$(id -u)" -ne 0 ]; then
      if have sudo; then SUDO=sudo; else echo "need root to install packages: run as root, or install them manually:"; echo "  $INSTALL $PKGS"; exit 1; fi
    fi
    [ -n "$UPDATE" ] && $SUDO $UPDATE
    $SUDO $INSTALL $PKGS
  else
    say "dependencies already installed"
  fi
fi

# the compiler must understand C++20
CXXBIN=${CXX:-g++}
ver=$($CXXBIN -dumpversion | cut -d. -f1)
if [ "$CXXBIN" = g++ ] && [ "$ver" -lt 11 ]; then
  echo "g++ $ver is too old (need 11 or newer). Install a newer one (for example gcc-12) and run: CXX=g++-12 ./build.sh --no-deps" >&2
  exit 1
fi

cmake_cfg() {  # $1 = PGO mode
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DPAINITE_NATIVE=ON -DPAINITE_LTO=ON -DPAINITE_PGO="$1" ${PREFIX:+-DCMAKE_INSTALL_PREFIX="$PREFIX"} >/dev/null
}

# ---- protocol data ---------------------------------------------------------------------------------------
need_data() {
  ls "$ROOT"/data/vanilla-*.bin >/dev/null 2>&1 && return 1 || return 0
}
if [ "$DATA" = 1 ] || { [ "$PGO" = 1 ] && need_data; }; then
  say "generating Java protocol data (downloads Mojang's server and, if needed, a JRE)"
  (cd /tmp && python3 -I "$ROOT/tools/fetch_vanilla_data.py" --out "$ROOT/data")
fi
if [ "$DATA" = 1 ]; then
  say "generating Bedrock data (downloads Mojang's Bedrock Dedicated Server, about 95 MB)"
  (cd /tmp && python3 -I "$ROOT/tools/fetch_bedrock_data.py" --out "$ROOT/data")
fi

# ---- build -----------------------------------------------------------------------------------------------
if [ "$PGO" = 1 ]; then
  say "PGO step 1/3: instrumented build"
  rm -rf "$BUILD"
  cmake_cfg generate
  cmake --build "$BUILD" -j"$JOBS" --target painite

  say "PGO step 2/3: training run (Java 26.x joins, Bedrock join if data and python3-cryptography exist)"
  TRAIN=$(mktemp -d)
  trap 'rm -rf "$TRAIN"; [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null || true' EXIT
  printf 'server-port=25999\nonline-mode=false\nview-distance=8\n' > "$TRAIN/server.properties"
  (cd "$TRAIN" && exec "$BUILD/painite" --no-bedrock >"$TRAIN/out.log" 2>&1) & SRV=$!
  sleep 1
  for ids in "$ROOT"/data/vanilla-*.ids; do
    proto=$(awk '$1=="protocol"{print $2}' "$ids")
    for _ in 1 2 3 4 5 6 7 8; do python3 -I "$ROOT/tests/smoke.py" --port 25999 --protocol "$proto" --ids "$ids" >/dev/null || true; done
  done
  kill "$SRV"; wait "$SRV" 2>/dev/null || true; SRV=""
  if ls "$ROOT"/data/bedrock-*.bin >/dev/null 2>&1 && python3 -c 'import cryptography' 2>/dev/null; then
    (cd "$TRAIN" && exec "$BUILD/painite" >"$TRAIN/out2.log" 2>&1) & SRV=$!
    sleep 1
    for _ in 1 2 3 4; do python3 "$ROOT/tests/bedrock_smoke.py" --port 25999 >/dev/null || true; done
    kill "$SRV"; wait "$SRV" 2>/dev/null || true; SRV=""
  fi
  profiles=$(find "$BUILD" -name '*.gcda' | wc -l)
  [ "$profiles" -gt 0 ] || { echo "training produced no profile data" >&2; exit 1; }

  say "PGO step 3/3: optimised rebuild using $profiles profile files"
  cmake_cfg use
  cmake --build "$BUILD" -j"$JOBS" --target painite
else
  say "building (Release, -O3, -march=native, LTO)"
  cmake_cfg off
  cmake --build "$BUILD" -j"$JOBS" --target painite
fi

if [ -n "$PREFIX" ]; then
  say "installing to $PREFIX"
  cmake --install "$BUILD" >/dev/null
  if ls "$ROOT"/data/* >/dev/null 2>&1; then mkdir -p "$PREFIX/data"; cp -f "$ROOT"/data/vanilla-* "$ROOT"/data/bedrock-* "$PREFIX/data/" 2>/dev/null || true; fi
  BIN="$PREFIX/bin/painite"
else
  BIN="$BUILD/painite"
fi

say "done: $BIN"
need_data && echo "note: no protocol data yet. Run ./build.sh --data (or tools/fetch_vanilla_data.py and tools/fetch_bedrock_data.py) before starting the server."
echo "start it from your server folder (the one with server.properties): $BIN"
