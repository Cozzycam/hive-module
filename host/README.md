# host/ — running the firmware off-device (phone port de-risk)

The phone version of the queen module is **the firmware itself, compiled to
WebAssembly**, drawing into an HTML canvas instead of the LCD — one source of
truth, two build targets, so the phone can't drift from the hardware.

This directory is the **first de-risk experiment**: prove that the firmware's
`Renderer` can draw a real colony scene into an `Arduino_Canvas` framebuffer
that we lift out and display anywhere. It renders one frame to a PNG.

## Result

`host/out/frame.png` — the real firmware renderer drawing the real sim
(queen + brood + conkers + food on the day-lit chamber floor), produced with
**no ESP32**. Channel decode verified correct (warm-tan floor, not blue).

## How it works

`Chamber::tick()` + `Renderer::draw()` are self-contained: they don't need the
Coordinator's networking / SD / persistence layers. So the build compiles only
the real sim+render logic and replaces the hardware edges with thin shims:

| Real firmware compiled | Shimmed (`host/shims/`) | Skipped entirely |
|---|---|---|
| renderer, chamber, conker, brood, queen, colony_state, pheromone_grid, bonds, rng | `Arduino.h` (millis/random/Serial/String/FreeRTOS no-ops/ps_malloc), `Arduino_GFX_Library.h` (Arduino_Canvas over a uint16_t RGB565 buffer + ~25 primitives), `Preferences.h`, `pgmspace.h` | Coordinator, transport/ESP-NOW, persistence, journal, chores, topology, http_server, ota, vps_push |

Network-coupled globals the renderer only *reads* (`g_tod`, `g_weather`) are
defined with fixed benign values in `host_stubs.cpp` (full daylight, clear).

The async-flush FreeRTOS worker never spawns (`xTaskCreatePinnedToCore` returns
failure → renderer's synchronous fallback); `host_main` reads the canvas buffer
directly rather than flushing to a panel.

## Build

```
host/build.sh          # emcc -> host/out/frame.js -> frame.rgb565 -> frame.png
```
Requires the emsdk at `/c/claude/emsdk` (Emscripten 6.x) and Node.

## Known gaps (not yet done — this is a de-risk, not the port)

- **Text is a placeholder** — the GFX shim renders a filled cell per glyph, not
  the real CP437 5x7 font. Only firmware overlays (zoomies "z", bond hearts,
  banners) use text, none of which fire on a fresh-colony frame. Drop in the
  real `glcdfont` for parity.
- **Not yet compared against a real LCD photo** for exact pixel-parity.
- Interaction (touch→feed), animation loop (rAF driving tick+flush to a live
  canvas), persistence (IndexedDB), and VPS push (fetch) are the next steps.
