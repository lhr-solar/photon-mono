#!/usr/bin/env bash
# Linux counterpart of photon.cmd for `build` and `station`. Other use cases
# (ui daq, ui pcan, ...) are Windows-only for now.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UI="$ROOT/photon-next"
SHIP="$ROOT/build"
EMSDK="$UI/.external/emsdk"
JOBS=$(nproc)
GEN=(-G "Unix Makefiles")
if command -v ninja >/dev/null; then
    GEN=(-G Ninja)
fi

usage() {
    cat <<'EOF'
  scripts/photon.sh build
      Build photon-server, photon-backend, photon-index, and the page into build/.
      Needs clang, cmake, git, python3, and Go 1.26.4. Installs emsdk into
      photon-next/.external the first time.

  scripts/photon.sh station [flags]
      Server and page on this machine: http://127.0.0.1:8080/photon.html.
      /mcp and Google sign-in go to database-mcp (photon-server/gamma) on :9100.
      Flags are passed to photon-server (-host 127.0.0.1 keeps it on loopback).

PHOTON_PASSWORD defaults to laptop. PHOTON_DATA_DIR defaults to <repo>/data.
EOF
}

build() {
    if ! command -v go >/dev/null; then
        echo "go is missing. Install Go 1.26.4 from https://go.dev/dl/" >&2
        exit 1
    fi
    mkdir -p "$SHIP/ui"

    # The desktop photon-ui is Windows-only; the page is the UI here.
    cmake -S "$UI" -B "$UI/.artifacts" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DPHOTON_NATIVE_UI=OFF -DBUILD_TESTING=OFF -DPHOTON_SHIP_DIR="$SHIP"
    cmake --build "$UI/.artifacts" --target photon-backend photon-index -j "$JOBS"

    if [ ! -f "$EMSDK/emsdk_env.sh" ]; then
        git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK"
        "$EMSDK/emsdk" install latest
        "$EMSDK/emsdk" activate latest
    fi
    # shellcheck disable=SC1091
    source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
    emcmake cmake -S "$UI" -B "$UI/.artifacts-web" "${GEN[@]}" \
        -DPHOTON_FRONTEND=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build "$UI/.artifacts-web" -j "$JOBS"
    # Same guard web/scripts/build.ps1 patches into the Emscripten glue.
    python3 - "$UI/.artifacts-web/photon.js" <<'EOF'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
s = p.read_text(encoding="utf-8")
old = "var args=readEmAsmArgs(sigPtr,argbuf);return ASM_CONSTS[emAsmAddr](...args)}"
new = 'var args=readEmAsmArgs(sigPtr,argbuf);var fn=ASM_CONSTS[emAsmAddr];if(typeof fn!=="function"){return 0}return fn(...args)}'
p.write_text(s.replace(old, new), encoding="utf-8")
EOF
    cp "$UI/.artifacts-web"/photon.{html,js,wasm,data} "$SHIP/ui/"
    if [ -d "$UI/.artifacts-web/models" ]; then
        cp -r "$UI/.artifacts-web/models" "$SHIP/ui/"
    fi

    (cd "$ROOT/photon-server" && go build -o "$SHIP/photon-server" ./server)
    echo "built $SHIP"
}

station() {
    if [ ! -x "$SHIP/photon-server" ] || [ ! -x "$SHIP/photon-backend" ]; then
        echo "build/ is incomplete. Run: scripts/photon.sh build" >&2
        exit 1
    fi
    local password=${PHOTON_PASSWORD:-laptop}
    local data=${PHOTON_DATA_DIR:-$ROOT/data}
    mkdir -p "$data"
    echo "password $password  data $data  page http://127.0.0.1:8080/photon.html"
    cd "$SHIP"
    exec ./photon-server -data-dir "$data" -password "$password" "$@"
}

case "${1:-}" in
    build) build ;;
    station) shift; station "$@" ;;
    *) usage ;;
esac
