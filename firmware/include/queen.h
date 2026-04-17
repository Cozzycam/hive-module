/* Founding queen. Ported from sim/queen.py. */
#pragma once
#include "config.h"

class Chamber;  // forward declaration

struct Queen {
    int8_t   x, y;
    int      lay_cooldown;
    int      eggs_laid    = 0;
    float    hunger       = 0.0f;
    bool     alive        = true;
    float    reserves;

    void init(int8_t px, int8_t py);
    void tick(Chamber& chamber);

    bool needs_feeding() const { return hunger > 0.5f; }

    // ---- internal ----
    float _consume(Chamber& ch, float amount);
    void  _tend_founding_brood(Chamber& ch);
    bool  _can_lay(Chamber& ch);
    void  _lay(Chamber& ch);
};
