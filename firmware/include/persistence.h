/* Persistence — SD-backed identity records and colony manifest.
 *
 * Storage layout on FAT32:
 *   /colony/manifest.json
 *   /colony/lilguys/<shard>/<id>.json    (shard = id/256, 3-digit zero-padded)
 *   /colony/brood/<shard>/<id>.json
 *   /colony/lilguys/.corrupt/            (moved here on parse failure)
 *   /colony/brood/.corrupt/
 *
 * Atomic writes: serialize to <file>.tmp, flush, close, rename over <file>.
 * Power loss leaves old file intact and a stale .tmp to clean on next boot.
 */
#pragma once

#include "config.h"
#include "sd_card.h"
#include <cstdint>

// ---- Persistence state machine ----

enum PersistenceState : uint8_t {
    PERSIST_UNINIT   = 0,
    PERSIST_OK       = 1,
    PERSIST_DEGRADED = 2,   // no SD card — running without persistence
    PERSIST_ERROR    = 3,   // SD failures — writes suspended, will retry
};

// ---- Identity record (per Conker, on disk + RAM cache) ----

struct IdentityRecord {
    uint32_t id           = 0;
    char     name[20]     = {};       // "Conker_<id>" placeholder (Phase 4: wordlist)
    uint8_t  role         = 0;        // Role enum
    bool     is_pioneer   = false;
    uint32_t born_unix    = 0;        // wall-clock birth time
    uint32_t died_unix    = 0;        // 0 = alive
    uint32_t tended_by    = 0;        // Phase 4: lineage
    float    personality[8] = {};     // Phase 3: 0.0-1.0 per dimension
    uint32_t traits       = 0;        // Phase 5: bitmask (zero-stub)
    uint32_t lifespan_ms  = 0;        // predetermined death-of-old-age timer
    uint32_t lived_ms     = 0;        // cumulative sim-running time (ageing source of truth)
    float    scale_factor = 3.2f;     // per-conker render scale
    uint8_t  tint_seed    = 0;        // per-conker colour variation (1-255)
    uint8_t  catches      = 0;        // cumulative critters caught (feeds the colony-wide Bug Hunter title, 5-step bar)
    uint8_t  accessory    = 0;        // worn hat (1=petal hat, 2=seed cap, 3=grass hat)
    uint8_t  accessory_tint = 0;      // maker's tint_seed (hat wears the maker's colour)
    uint32_t accessory_from = 0;      // maker's id (friendship-loss / memorial checks)
    bool     accessory_memorial = false;  // maker died a friend — worn for life
    bool     is_founder   = false;    // first FOUNDER_COHORT_SIZE hatches
    uint16_t chamber_id   = 0;        // module that owns this worker
    float    last_x       = 0;
    float    last_y       = 0;
    uint8_t  last_state   = 0;        // AntState enum
    // v151: needs survive reboot (mood is re-derived from these on the first tick).
    float    last_boredom    = 0;
    float    last_tiredness  = 0;
    float    last_loneliness = 0;
    float    last_hunger     = 0;
    // Personality drift: what grief took (positive magnitudes). A NEW best
    // friend restores half of the credit and clears it.
    float    grief_social = 0;
    float    grief_play   = 0;
    // Incubation/princess (Gateway): closeness to the keeper survives reloads —
    // the PWA reloads constantly, so a RAM-only bond read as "impossible to
    // build" (feedback #51). Always 0 outside incubation mode.
    float    keeper_bond  = 0;
    bool     dirty        = false;    // RAM-only: needs flush to disk
};

// ---- Brood record (per brood unit, on disk + RAM cache) ----

struct BroodRecord {
    uint32_t id           = 0;
    uint8_t  stage        = 0;        // BroodStage enum
    uint8_t  role         = 0;        // Role assigned at egg
    int8_t   x            = 0;
    int8_t   y            = 0;
    float    hunger       = 0;
    float    food_invested = 0;
    uint32_t stage_start_ms = 0;      // millis at current stage entry
    uint32_t total_duration_ms = 0;   // founder bootstrap: total egg+seed time
    uint32_t born_unix    = 0;
};

// ---- Queen state (in manifest, not a separate record) ----

struct QueenPersistState {
    float    reserves      = 0;
    float    hunger        = 0;
    bool     founding_done = false;
    int      eggs_laid     = 0;
    float    egg_accum     = 0;
    int8_t   x             = 0;
    int8_t   y             = 0;
};

// ---- Colony manifest ----

struct ColonyManifest {
    uint8_t  schema          = 1;
    char     colony_id[25]   = {};    // 12-byte hex string
    char     queen_name[16]  = {};    // random plant name, assigned at founding
    char     title[25]       = {};    // keeper's chosen name for the colony (feedback
                                      // #40). EMPTY = fall back to colony_id, which
                                      // stays the immutable API/identity key — this is
                                      // a display name only, never an address.
    uint32_t founded_unix    = 0;
    uint32_t next_lilguy_id  = 1;     // monotonic, never decremented
    uint32_t last_tick       = 0;     // sim tick_count at last flush

    // Colony-level mutable state
    float    food_store      = 0;
    float    food_total      = 0;
    uint16_t bugs            = 0;     // shop purse (see ColonyState::bugs)
    uint32_t owned_items     = 0;     // shop items already paid for
    uint32_t last_interact[3] = {0,0,0};  // keeper-interaction cooldown clocks
    bool     bond_peak_seen  = false;     // 100%-bond celebration already shown
    uint16_t total_workers_born = 0;
    uint16_t total_workers_died = 0;
    uint16_t worker_census   = 0;

    // Queen state (singleton)
    QueenPersistState queen_state;

    // Module role
    uint8_t  module_role     = 0;     // ModuleRole enum

    // Verdant chamber (Gateway coronation): the module has an identity and a
    // living chamber but NO colony yet — it sits "awaiting opportunity" until
    // a summon_queen crowns a raised princess (or a reset refounds normally).
    bool     awaiting        = false;

    // Gateway coronation: a queen summoned from the app (a raised princess).
    // All-zero on ordinary colonies — set once at founding from the staged
    // handoff (vps_push summon_queen), then immutable. q_pers is the
    // inheritance source for untended (founding) hatches, so the colony
    // takes after the character she grew on the phone.
    bool     queen_imported  = false;
    float    q_pers[8]       = {};
    uint8_t  q_tint          = 0;     // her body tint (queen render tint, future)
    float    q_bond          = 0;     // keeper-bond at coronation ("beloved" seed)
    uint32_t q_born_unix     = 0;     // when she hatched on the phone
    uint32_t q_traits        = 0;     // traits she earned solo
    uint8_t  q_catches       = 0;     // her critter tally (Bug Hunter history)
    char     q_from[25]      = {};    // the app colony she grew up in (prehistory)

    // Live positions (written with manifest, avoids per-record file writes)
    static constexpr int MAX_POS = 48;   // tracks MAX_CONKERS; JSON load clamps
    struct PosEntry { uint32_t id; float x, y; };
    PosEntry positions[MAX_POS];
    int      pos_count = 0;
};

// ---- Registry (owns RAM cache + disk I/O) ----

class ConkerRegistry {
public:
    // Initialize — mount SD, load manifest, populate RAM cache.
    // Returns the PersistenceState after init.
    PersistenceState init();

    PersistenceState state() const { return _state; }

    // ID allocation
    uint32_t allocate_id();

    // Naming — a random plant name no known record (or the queen) already
    // bears. Plain name_random() gave the colony two Marigolds: esp_random's
    // early-boot state can repeat across reboots. Uniqueness keeps the
    // historian legible ("mourned by Marigold" must mean one Marigold).
    void pick_unique_name(char* buf, size_t buflen);

    // Record lifecycle
    bool create(const IdentityRecord& rec);
    bool update(uint32_t id, const IdentityRecord& rec);
    IdentityRecord* get(uint32_t id);
    void mark_dead(uint32_t id, uint32_t died_unix);

    // Forensics + recovery (serial `who <id>` / `revive <id>`)
    bool dump_record(uint32_t id, char* out, size_t out_size);  // raw JSON from disk
    bool revive(uint32_t id);  // clear died_unix on a dead record, reload to cache

    // Brood lifecycle
    bool create_brood(const BroodRecord& rec);
    BroodRecord* get_brood(uint32_t id);
    bool update_brood(const BroodRecord& rec);  // re-persist developmental state (stage, timing, feeding)
    void remove_brood(uint32_t id);  // hatched or died — remove from cache + disk

    // Flush dirty records to disk.  Called periodically (30s) and on lifecycle events.
    void flush();

    // Flush manifest only (for tick_count updates).
    void flush_manifest();

    // Chores-worker SD write result (routed from the main loop's pump).
    // Failures re-dirty the record so the next flush retries.
    void handle_sd_result(uint32_t token, bool ok);

    // Iteration
    int living_count() const { return _alive_count; }
    IdentityRecord* living_records() { return _alive; }
    int brood_count() const { return _brood_count; }
    BroodRecord* brood_records() { return _brood; }

    // Manifest access (for sim to read/write colony state)
    ColonyManifest& manifest() { return _manifest; }

    // Generate a fresh colony_id (called during migration)
    void generate_colony_id();

private:
    PersistenceState _state = PERSIST_UNINIT;
    ColonyManifest   _manifest;

    // RAM caches — allocated on PSRAM
    // Living records only — the dead are filtered out at load and swap-removed
    // at death, so this tracks the pop cap (20), not MAX_CONKERS. Load clamps.
    static constexpr int MAX_ALIVE = 64;
    static constexpr int MAX_BROOD_CACHE = 60;
    IdentityRecord  _alive[MAX_ALIVE];
    int             _alive_count = 0;
    BroodRecord     _brood[MAX_BROOD_CACHE];
    int             _brood_count = 0;

    uint32_t _last_flush_ms = 0;

    // Disk helpers
    bool _ensure_dirs();
    bool _load_manifest();
    bool _save_manifest();
    bool _generate_colony_id();
    bool _load_living_records();
    bool _load_brood_records();
    bool _write_record(const IdentityRecord& rec, bool async = false,
                       uint32_t token = 0);
    bool _write_brood(const BroodRecord& rec);
    bool _delete_brood_file(uint32_t id);
    bool _atomic_write(const char* path, const char* json_buf, size_t len);
    bool _atomic_write_async(const char* path, const char* json_buf,
                             size_t len, uint32_t token);
    void _shard_path(char* buf, size_t buflen, const char* subdir, uint32_t id);
    void _record_path(char* buf, size_t buflen, const char* subdir, uint32_t id);
    void _move_to_corrupt(const char* path, const char* subdir);
    void _clean_tmp_files();
};

// ---- Full colony wipe (fresh start / satellite conversion) ----
// Removes the whole /colony tree from SD and clears the founding + VPS
// journal cursors from NVS. Caller reboots afterwards. Shared by the
// "reset colony" serial command and the reset_to_satellite app command.
void colony_reset_wipe();
