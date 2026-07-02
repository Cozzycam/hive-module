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
    EVT_CONKER_DIED        = 8,
    EVT_HANDOFF_INCOMING    = 9,
    EVT_HANDOFF_OUTGOING    = 10,
    EVT_PARADE_STARTED      = 11,
    EVT_DISCOVERY           = 12,   // a conker found a visiting critter
    EVT_BOND_FORMED         = 13,   // one-way bond crossed the formed threshold
    EVT_BOND_MUTUAL         = 14,   // pair became best friends
    EVT_TRAIT_EARNED        = 15,   // a conker earned a trait/title
    EVT_MOURNING            = 16,   // a conker set off to stand vigil
    EVT_CHALLENGE_STARTED   = 17,   // weather challenge began (banner)
    EVT_CHALLENGE_ENDED     = 18,   // weather challenge passed (banner)
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
    int8_t x, y;         // position of the brood
};

struct PositionData {
    int8_t x, y;
};

struct HandoffData {
    uint8_t  worker_idx;   // slot index in conkers[]
    uint8_t  face;         // Face enum value
    uint16_t neighbour_id; // module_id of the neighbour
};

struct ParadeData {
    uint8_t leader_idx;    // slot index in conkers[]
    uint8_t participants;  // total conkers in the line, leader included
};

struct DiscoveryData {
    uint8_t finder_idx;    // slot index in conkers[]
    uint8_t kind;          // CritterKind value
};

/* Story-beat payloads carry ids AND names: the renderer resolves ids to
 * local chamber slots for animations (skipping if the conker isn't here),
 * while names keep the HUD banner meaningful even for non-local conkers. */

struct BondEventData {     // bond_formed, bond_mutual
    uint32_t a_id, b_id;
    char     a_name[16], b_name[16];
};

struct TraitEarnedData {
    uint32_t conker_id;
    uint32_t trait_bit;    // TraitBit mask (single bit)
    char     who[16];
};

struct MourningData {
    uint32_t mourner_id;
    char     mourner_name[16];
    char     dead_name[16];
};

struct ChallengeEventData {
    uint8_t challenge_type;   // ChallengeType value
    float   severity;
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
        PositionData           position;  // queen_laid_egg, young_died, conker_died
        HandoffData            handoff;   // handoff_incoming, handoff_outgoing
        ParadeData             parade;    // parade_started
        DiscoveryData          discovery; // discovery
        BondEventData          bond;      // bond_formed, bond_mutual
        TraitEarnedData        trait;     // trait_earned
        MourningData           mourning;  // mourning
        ChallengeEventData     challenge; // challenge_started, challenge_ended
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
