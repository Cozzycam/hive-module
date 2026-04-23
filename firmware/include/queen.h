/* Founding queen — real-time lifecycle. */
#pragma once
#include "config.h"

class Chamber;

struct Queen {
    int8_t x, y;
    float  reserves    = 0.0f;
    float  hunger      = 0.0f;
    int    eggs_laid    = 0;
    bool   alive        = true;
    bool   founding_done = false;   // one-way: true after founding eggs laid
    float  egg_accum    = 0.0f;     // fractional egg accumulator

    bool needs_feeding() const { return hunger > 0.3f; }

    void init(int8_t px, int8_t py);
    void tick(Chamber& ch, float dt);

private:
    float _consume(Chamber& ch, float amount);
    void  _tend_founding_brood(Chamber& ch, float dt);
    bool  _can_lay(Chamber& ch);
    void  _lay_founding(Chamber& ch, float dt);
    void  _lay_established(Chamber& ch, float dt);
};
