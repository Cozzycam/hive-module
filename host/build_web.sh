#!/usr/bin/env bash
# Build the live browser module: host_web.cpp + firmware sim/renderer -> WASM.
# Output: host/web/hive.js + hive.wasm (loaded by host/web/index.html).
# Usage: host/build_web.sh   (run from repo root)
set -euo pipefail

EMSDK="/c/claude/emsdk"
export PATH="$EMSDK/upstream/emscripten:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p host/web

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
  firmware/src/sim.cpp
  firmware/src/coordinator.cpp
  firmware/src/persistence.cpp
  firmware/src/journal.cpp
  firmware/src/api_json.cpp
  firmware/src/hud.cpp
)

# ArduinoJson is header-only and portable — point at the PlatformIO checkout.
AJSON_INC="firmware/.pio/libdeps/esp32s3/ArduinoJson/src"

EXPORTS='_host_boot,_host_set_now,_host_step,_host_fb,_host_w,_host_h,_host_tap,_host_gather,_host_gather_end,_host_selected,_host_conkers,_host_snapshot,_host_snapshot_ptr,_malloc,_free'

echo "[build_web] compiling -> host/web/hive.js"
# ArduinoJson auto-enables Arduino String/Stream/Print integration when ARDUINO
# is defined; the firmware serializes to plain char buffers, so switch it to
# pure-C++ mode (avoids needing Print/Stream/__FlashStringHelper shims).
AJSON_CFG="-DARDUINOJSON_ENABLE_ARDUINO_STRING=0 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0"

emcc \
  -std=gnu++17 -O2 -DARDUINO=200 -DHOST_BUILD $AJSON_CFG -include stddef.h -Uunix -Ulinux \
  -I host/shims -I firmware/include -I "$AJSON_INC" \
  host/host_web.cpp host/host_stubs.cpp host/stubs_hw.cpp "${FW_SRC[@]}" \
  -sMODULARIZE=1 -sEXPORT_NAME=createHiveModule \
  -sENVIRONMENT=web \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8,HEAPU16,UTF8ToString \
  -sALLOW_MEMORY_GROWTH=1 \
  -o host/web/hive.js

echo "[build_web] done. Serve host/web/ over HTTP and open index.html"
