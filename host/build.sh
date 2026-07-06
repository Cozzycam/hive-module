#!/usr/bin/env bash
# Build the render de-risk harness to WASM (runs under Node) and produce a PNG.
# Usage: host/build.sh   (run from repo root)
set -euo pipefail

EMSDK="/c/claude/emsdk"
export PATH="$EMSDK/upstream/emscripten:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p host/out

# Real firmware logic reachable from Chamber::tick + Renderer::draw.
FW_SRC=(
  firmware/src/renderer.cpp
  firmware/src/chamber.cpp
  firmware/src/conker.cpp
  firmware/src/brood.cpp
  firmware/src/queen.cpp
  firmware/src/colony_state.cpp
  firmware/src/pheromone_grid.cpp
  firmware/src/bonds.cpp
  firmware/src/rng.cpp
)

echo "[build] compiling ${#FW_SRC[@]} firmware sources + host harness -> WASM"
emcc \
  -std=gnu++17 -O1 \
  -I host/shims -I firmware/include \
  host/host_main.cpp host/host_stubs.cpp "${FW_SRC[@]}" \
  -sENVIRONMENT=node -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 \
  -o host/out/frame.js

echo "[build] running -> host/out/frame.rgb565"
node host/out/frame.js

echo "[build] encoding PNG -> host/out/frame.png"
node host/dump_png.js host/out/frame.rgb565 host/out/frame.png 480 320

echo "[build] done: host/out/frame.png"
