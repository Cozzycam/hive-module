/* EventJournal — append-only event log on SD card.
 *
 * Per-day rolling JSONL files: /colony/events/YYYY-MM-DD.jsonl
 * Buffered writes: RAM ring buffer flushed every 5s or when nearly full.
 * Events are never modified or deleted — past is immutable.
 */
#pragma once
#include <cstdint>
#include <cstddef>

// ---- Journal event types ----

enum JournalType : uint8_t {
    JEVT_HATCH = 0,
    JEVT_DEATH,
    JEVT_ROLE_CHANGE,
    JEVT_FOOD_TAP,
    JEVT_FOOD_DISCOVERED,
    JEVT_FOOD_DELIVERED,
    JEVT_CHAMBER_CROSSING,
    JEVT_MILESTONE,
    JEVT_COLONY_EVENT,
    JEVT_COUNT
};

// ---- Journal entry (compact, serialized to JSON at flush time) ----

struct JournalEntry {
    uint32_t tick;
    uint32_t unix_time;
    uint32_t lilguy_id;   // 0 for colony-level events
    uint8_t  type;        // JournalType

    union {
        struct { uint8_t role; bool is_pioneer; uint32_t from_brood_id; } hatch;
        struct { uint8_t cause; } death;  // 0=old_age, 1=starved
        struct { uint8_t from_role; uint8_t to_role; } role_change;
        struct { int8_t x; int8_t y; float amount; } food_tap;
        struct { int8_t x; int8_t y; } food_discovered;
        struct { float amount; } food_delivered;
        struct { uint16_t from_module; uint16_t to_module; uint8_t face; } crossing;
        struct { uint8_t kind; uint32_t value; } milestone;  // kind enum below
        struct { uint8_t kind; uint16_t module_id; } colony_event;  // kind enum below
    };
};

// Milestone kinds
enum MilestoneKind : uint8_t {
    MILE_WORKERS_BORN = 0,   // value = total count
    MILE_FIRST_MAJOR  = 1,
};

// Colony event kinds
enum ColonyEventKind : uint8_t {
    COLONY_FOUNDED            = 0,
    COLONY_MODULE_CONNECTED   = 1,
    COLONY_MODULE_DISCONNECTED = 2,
};

// ---- EventJournal ----

class EventJournal {
public:
    // Initialize — create events dir, open today's file.
    // Call after SD card is mounted and g_tod is valid.
    void init();

    // Push an event to the RAM buffer. Non-blocking.
    void emit(const JournalEntry& entry);

    // Called every tick from coordinator. Handles flush timing and day rollover.
    void tick();

    // Force flush (e.g. before shutdown or on lifecycle events).
    void flush();

    // Stats
    int pending_count() const { return _count; }
    int total_flushed() const { return _total_flushed; }

    // ---- Read API (naive linear scan) ----

    // Callback for reading events. Return false to stop iteration.
    using EventCallback = bool(*)(const char* json_line, void* ctx);

    // Read all events for a UTC day (days since epoch = unix / 86400).
    void read_day(uint32_t unix_time, EventCallback cb, void* ctx);

    // Read events involving a specific LilGuy since a given time.
    void read_lilguy(uint32_t id, uint32_t since_unix,
                     EventCallback cb, void* ctx);

private:
    static constexpr int BUF_CAPACITY = 256;
    static constexpr uint32_t FLUSH_INTERVAL_MS = 5000;

    JournalEntry _buf[BUF_CAPACITY];
    int          _head = 0;
    int          _count = 0;
    int          _total_flushed = 0;
    uint32_t     _last_flush_ms = 0;
    int          _current_day = -1;  // UTC day number (unix / 86400)
    bool         _active = false;

    void _serialize_entry(const JournalEntry& e, char* buf, size_t buflen);
    void _make_day_path(int day_num, char* buf, size_t buflen);
    void _open_day_if_needed();
};
