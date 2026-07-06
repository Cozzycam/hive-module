/* Host shim for <esp_random.h>. On device this is the hardware RNG used for
 * IDs / boot_id / name entropy. Here: a self-seeding LCG (the registry dedupes
 * names anyway, so within-session uniqueness holds). A real host build could
 * seed this from JS crypto for cross-session variety. */
#pragma once
#include <cstdint>

static inline uint32_t esp_random(void) {
    static uint32_t s = 0x9E3779B9u;
    s = s * 1664525u + 1013904223u;
    return s ^ (s >> 16);
}
