/* EventJournal — append-only event log. */
#include "journal.h"
#include "sd_card.h"
#include "time_of_day.h"
#include "world_condition.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>

// Type name strings (must match JournalType enum order)
static const char* JEVT_NAMES[] = {
    "hatch", "death", "role_change", "food_tap",
    "food_discovered", "food_delivered", "chamber_crossing",
    "milestone", "colony_event", "tended_by_assigned",
    "bond_formed", "bond_broken", "challenge_start",
    "challenge_end", "trait_earned", "mourning", "play", "discovery"
};

// Keep in lockstep with CritterKind (chamber.h)
static const char* CRITTER_NAMES[] = { "beetle", "butterfly", "worm" };

static const char* CHALLENGE_NAMES[] = {
    "none", "heatwave", "cold_snap", "drought", "storm"
};

static const char* TRAIT_NAMES[] = {
    "pioneer", "elder", "bonded",
    "survived_heatwave", "survived_cold_snap", "survived_drought", "survived_storm"
};

static const char* DEATH_CAUSE[] = { "old_age", "starvation" };

static const char* MILESTONE_NAMES[] = { "workers_born", "first_major" };

// Keep in lockstep with ColonyEventKind (journal.h) — an OOB read here once
// shipped garbage event names
static const char* COLONY_EVT_NAMES[] = {
    "founded", "module_connected", "module_disconnected",
    "met_a_neighbouring_kingdom",
    "sent_a_care_package", "care_package_from_neighbours",
};

// ---- Path helpers ----

void EventJournal::_make_day_path(int day_num, char* buf, size_t buflen) {
    // Convert day number to YYYY-MM-DD
    uint32_t unix = (uint32_t)day_num * 86400UL + 43200UL;  // noon of that day
    struct tm t;
    time_t tt = (time_t)unix;
    gmtime_r(&tt, &t);
    snprintf(buf, buflen, "/colony/events/%04d-%02d-%02d.jsonl",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

// ---- Serialization ----

void EventJournal::_serialize_entry(const JournalEntry& e, char* buf, size_t buflen) {
    JsonDocument doc;
    doc["tick"] = e.tick;
    doc["unix"] = e.unix_time;
    doc["type"] = JEVT_NAMES[e.type];
    doc["lilguy"] = e.lilguy_id;
    if (e.who[0]) doc["name"] = e.who;

    JsonObject data = doc["data"].to<JsonObject>();

    switch (e.type) {
    case JEVT_HATCH:
        data["role"] = e.hatch.role;
        data["is_pioneer"] = e.hatch.is_pioneer;
        data["from_brood_id"] = e.hatch.from_brood_id;
        break;
    case JEVT_DEATH:
        data["cause"] = DEATH_CAUSE[e.death.cause & 1];
        break;
    case JEVT_ROLE_CHANGE:
        data["from"] = e.role_change.from_role;
        data["to"] = e.role_change.to_role;
        break;
    case JEVT_FOOD_TAP:
        data["x"] = e.food_tap.x;
        data["y"] = e.food_tap.y;
        data["amount"] = e.food_tap.amount;
        break;
    case JEVT_FOOD_DISCOVERED:
        data["x"] = e.food_discovered.x;
        data["y"] = e.food_discovered.y;
        break;
    case JEVT_FOOD_DELIVERED:
        data["amount"] = e.food_delivered.amount;
        break;
    case JEVT_CHAMBER_CROSSING:
        data["from_module"] = e.crossing.from_module;
        data["to_module"] = e.crossing.to_module;
        data["face"] = e.crossing.face;
        break;
    case JEVT_MILESTONE:
        data["kind"] = MILESTONE_NAMES[e.milestone.kind];
        data["value"] = e.milestone.value;
        break;
    case JEVT_COLONY_EVENT:
        data["kind"] = COLONY_EVT_NAMES[e.colony_event.kind];
        if (e.colony_event.module_id != 0)
            data["module_id"] = e.colony_event.module_id;
        break;
    case JEVT_TENDED_BY_ASSIGNED:
        data["carer_id"] = e.tended_by.carer_id;
        break;
    case JEVT_BOND_FORMED:
    case JEVT_BOND_BROKEN:
        data["target_id"] = e.bond.target_id;
        if (e.bond.target_name[0]) data["target_name"] = e.bond.target_name;
        break;
    case JEVT_CHALLENGE_START:
    case JEVT_CHALLENGE_END:
        if (e.challenge.challenge_type < CHALLENGE_COUNT)
            data["type"] = CHALLENGE_NAMES[e.challenge.challenge_type];
        data["severity"] = e.challenge.severity;
        break;
    case JEVT_TRAIT_EARNED: {
        // Find trait name from bit
        uint32_t bit = e.trait.trait_bit;
        const char* name = "unknown";
        for (int i = 0; i < 7; i++) {
            if (bit == (1u << i)) { name = TRAIT_NAMES[i]; break; }
        }
        data["trait"] = name;
        break;
    }
    case JEVT_MOURNING:
        data["dead_id"] = e.mourning.dead_id;
        break;
    case JEVT_PLAY:
        data["kind"] = e.play.kind == 0 ? "parade" : "play";
        data["participants"] = e.play.participants;
        break;
    case JEVT_DISCOVERY:
        data["critter"] = (e.discovery.critter < 3)
            ? CRITTER_NAMES[e.discovery.critter] : "critter";
        break;
    }

    size_t len = serializeJson(doc, buf, buflen - 1);
    buf[len] = '\n';
    buf[len + 1] = '\0';
}

// ---- Init ----

void EventJournal::init() {
    if (sd_card_state() != SD_OK) {
        _active = false;
        return;
    }

    if (!SD_MMC.exists("/colony/events")) {
        SD_MMC.mkdir("/colony/events");
    }

    _head = 0;
    _count = 0;
    _total_flushed = 0;
    _last_flush_ms = millis();
    _current_day = -1;
    _active = true;

    Serial.println("[journal] initialized");
}

// ---- Emit ----

void EventJournal::emit(const JournalEntry& entry) {
    if (!_active) return;

    int idx = (_head + _count) % BUF_CAPACITY;
    _buf[idx] = entry;

    if (_count < BUF_CAPACITY) {
        _count++;
    } else {
        // Buffer full — advance head (oldest event dropped)
        _head = (_head + 1) % BUF_CAPACITY;
    }

    // Flush immediately if buffer is 75% full
    if (_count >= BUF_CAPACITY * 3 / 4) {
        flush();
    }
}

// ---- Tick ----

void EventJournal::tick() {
    if (!_active) return;

    // Periodic flush
    uint32_t now = millis();
    if (_count > 0 && (now - _last_flush_ms >= FLUSH_INTERVAL_MS)) {
        flush();
    }
}

// ---- Flush ----

void EventJournal::flush() {
    if (!_active || _count == 0) return;
    if (sd_card_state() != SD_OK) return;

    _open_day_if_needed();

    // Build the day path
    char path[48];
    _make_day_path(_current_day, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) {
        sd_card_write_failed();
        return;
    }

    char line[384];
    int written = 0;
    while (_count > 0) {
        JournalEntry& e = _buf[_head];

        // Day rollover: if this entry is on a different day, close and reopen
        int entry_day = e.unix_time / 86400;
        if (entry_day != _current_day && entry_day > 0) {
            f.close();
            _current_day = entry_day;
            _make_day_path(_current_day, path, sizeof(path));
            f = SD_MMC.open(path, FILE_APPEND);
            if (!f) {
                sd_card_write_failed();
                return;
            }
        }

        _serialize_entry(e, line, sizeof(line));
        f.print(line);
        written++;

        _head = (_head + 1) % BUF_CAPACITY;
        _count--;
    }

    f.flush();
    f.close();
    sd_card_write_ok();

    _total_flushed += written;
    _last_flush_ms = millis();
}

void EventJournal::_open_day_if_needed() {
    if (g_tod.unix_time == 0) return;
    int today = g_tod.unix_time / 86400;
    if (today != _current_day) {
        _current_day = today;
    }
}

// ---- Read API ----

void EventJournal::read_day(uint32_t unix_time, EventCallback cb, void* ctx) {
    int day = unix_time / 86400;
    char path[48];
    _make_day_path(day, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return;

    char line[384];
    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char c = f.read();
            if (c == '\n') break;
            line[len++] = c;
        }
        line[len] = '\0';
        if (len > 0) {
            if (!cb(line, ctx)) break;
        }
    }
    f.close();
}

void EventJournal::read_lilguy(uint32_t id, uint32_t since_unix,
                                EventCallback cb, void* ctx) {
    // Scan from since_unix's day through today
    int start_day = since_unix / 86400;
    int today = g_tod.unix_time / 86400;

    char id_pattern[16];
    snprintf(id_pattern, sizeof(id_pattern), "\"lilguy\":%lu", (unsigned long)id);

    for (int day = start_day; day <= today; day++) {
        char path[48];
        _make_day_path(day, path, sizeof(path));

        File f = SD_MMC.open(path, FILE_READ);
        if (!f) continue;

        char line[384];
        while (f.available()) {
            int len = 0;
            while (f.available() && len < (int)sizeof(line) - 1) {
                char c = f.read();
                if (c == '\n') break;
                line[len++] = c;
            }
            line[len] = '\0';
            if (len > 0 && strstr(line, id_pattern)) {
                if (!cb(line, ctx)) { f.close(); return; }
            }
        }
        f.close();
    }
}
