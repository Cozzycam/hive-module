/* Host shim for <esp_random.h>. On device this is the hardware RNG used for
 * IDs / boot_id / name entropy. Here: an LCG whose state is a SINGLE shared
 * global (not a per-TU function static), so one call to esp_random_seed()
 * reseeds every consumer at once — names (name_random), boot_id, and the
 * conker-registry naming path. host_seed() (host_web.cpp) drives it from the
 * install's unique colony_id so each phone founds its OWN colony instead of
 * the byte-identical one the fixed seed used to produce. */
#pragma once
#include <cstdint>

extern uint32_t g_esp_rng_state;   // defined once in host_stubs.cpp

static inline uint32_t esp_random(void) {
    g_esp_rng_state = g_esp_rng_state * 1664525u + 1013904223u;
    return g_esp_rng_state ^ (g_esp_rng_state >> 16);
}

// Reseed all esp_random consumers. Zero is coerced to the old default so a
// missing/zero seed still yields a running (if shared) sequence.
static inline void esp_random_seed(uint32_t s) {
    g_esp_rng_state = s ? s : 0x9E3779B9u;
}
