/* Typed event bus — sim emits, renderer subscribes.
 *
 * Events are ephemeral and advisory. Missing one skips an animation,
 * never breaks state. Also the future seam for inter-module network
 * transport.
 *
 * Tagged union: each Event has a type tag and a payload union.
 * Ring buffer overwrites oldest when full. Single-threaded, no locks.
 */
#pragma once

#include <cstdint>

// ---- Event type tags ----
enum EventType : uint8_t {
    EVT_INTERACTION_STARTED = 0,
    EVT_INTERACTION_ENDED   = 1,
    EVT_FOOD_DELIVERED      = 2,
    EVT_FOOD_TAPPED         = 3,
    EVT_PILE_DISCOVERED     = 4,
    EVT_QUEEN_LAID_EGG      = 5,
    EVT_YOUNG_HATCHED       = 6,
    EVT_YOUNG_DIED          = 7,
    EVT_LIL_GUY_DIED        = 8,
    EVT_HANDOFF_INCOMING    = 9,
    EVT_HANDOFF_OUTGOING    = 10,
};

// ---- Interaction subtypes ----
enum InteractionKind : uint8_t {
    INTERACT_GREETING      = 0,
    INTERACT_FOOD_SHARING  = 1,
    INTERACT_TENDING_YOUNG = 2,
    INTERACT_TENDING_QUEEN = 3,
};

// ---- Event payloads ----

struct InteractionStartedData {
    uint16_t        pair_id;
    InteractionKind kind;
    uint8_t         duration_hint;
};

struct InteractionEndedData {
    uint16_t pair_id;
};

struct FoodDeliveredData {
    int8_t x, y;
    float  amount;
};

struct FoodTappedData {
    int8_t x, y;
};

struct YoungHatchedData {
    uint8_t stage_from;  // BroodStage value
    uint8_t stage_to;    // BroodStage value or 0xFF for 'worker'
};

// ---- Event struct (tagged union) ----

struct Event {
    EventType type;
    uint32_t  tick;
    union {
        InteractionStartedData interaction_started;
        InteractionEndedData   interaction_ended;
        FoodDeliveredData      food_delivered;
        FoodTappedData         food_tapped;
        YoungHatchedData       young_hatched;
        // queen_laid_egg, young_died, lil_guy_died have no payload
    };
};

// ---- Ring buffer ----

constexpr int EVENT_BUS_CAPACITY = 256;

class EventBus {
public:
    void init() {
        _write = 0;
        _count = 0;
        _next_pair_id = 0;
    }

    void emit(const Event& ev) {
        _buf[_write] = ev;
        _write = (_write + 1) % EVENT_BUS_CAPACITY;
        if (_count < EVENT_BUS_CAPACITY)
            _count++;
    }

    /* Drain: copies up to max_out events into out[], returns count.
     * Clears the buffer. Called once per frame for serial logging. */
    int drain(Event* out, int max_out) {
        if (_count == 0) return 0;
        int start = (_write - _count + EVENT_BUS_CAPACITY) % EVENT_BUS_CAPACITY;
        int n = (_count < max_out) ? _count : max_out;
        for (int i = 0; i < n; i++) {
            int idx = (start + i) % EVENT_BUS_CAPACITY;
            out[i] = _buf[idx];
        }
        _count = 0;
        return n;
    }

    int count() const { return _count; }

    uint16_t next_pair_id() {
        return _next_pair_id++;
    }

private:
    Event    _buf[EVENT_BUS_CAPACITY];
    int      _write = 0;
    int      _count = 0;
    uint16_t _next_pair_id = 0;
};
