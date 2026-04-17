/* Fast xorshift32 PRNG for the sim. Seed from hardware RNG at boot. */
#pragma once
#include <cstdint>

struct Rng {
    uint32_t state;

    explicit Rng(uint32_t seed = 1) : state(seed ? seed : 1) {}

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // [0, 1)
    float rand_float() {
        return (next() & 0x7FFFFF) / static_cast<float>(0x800000);
    }

    // [lo, hi] inclusive
    int rand_int(int lo, int hi) {
        if (lo >= hi) return lo;
        uint32_t range = static_cast<uint32_t>(hi - lo + 1);
        return lo + static_cast<int>(next() % range);
    }

    // Pick -1 or 1
    int rand_sign() { return (next() & 1) ? 1 : -1; }

    // Pick from {-1, 0, 1}
    int rand_dir() { return static_cast<int>(next() % 3) - 1; }
};

// Global RNG instance
extern Rng g_rng;
