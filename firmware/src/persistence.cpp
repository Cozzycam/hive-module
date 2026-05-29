/* Persistence — SD-backed identity records and colony manifest. */
#include "persistence.h"
#include "sd_card.h"
#include "pin_config.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <esp_random.h>

static constexpr uint32_t FLUSH_INTERVAL_MS = 5000;  // 5s staggered flush (max 3 records per call)

// ---- Path helpers ----

void ConkerRegistry::_shard_path(char* buf, size_t buflen,
                                  const char* subdir, uint32_t id) {
    uint32_t shard = id / 256;
    snprintf(buf, buflen, "/colony/%s/%03lu", subdir, (unsigned long)shard);
}

void ConkerRegistry::_record_path(char* buf, size_t buflen,
                                   const char* subdir, uint32_t id) {
    uint32_t shard = id / 256;
    snprintf(buf, buflen, "/colony/%s/%03lu/%08lX.json",
             subdir, (unsigned long)shard, (unsigned long)id);
}

// ---- Directory setup ----

bool ConkerRegistry::_ensure_dirs() {
    const char* dirs[] = {
        "/colony",
        "/colony/lilguys",
        "/colony/lilguys/.corrupt",
        "/colony/brood",
        "/colony/brood/.corrupt",
    };
    for (auto d : dirs) {
        if (!SD_MMC.exists(d)) {
            if (!SD_MMC.mkdir(d)) {
                Serial.printf("[persist] failed to create dir: %s\r\n", d);
                return false;
            }
        }
    }
    return true;
}

// ---- Atomic write ----

bool ConkerRegistry::_atomic_write(const char* path, const char* json_buf, size_t len) {
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    File f = SD_MMC.open(tmp_path, FILE_WRITE);
    if (!f) {
        sd_card_write_failed();
        return false;
    }
    size_t written = f.write((const uint8_t*)json_buf, len);
    f.flush();
    f.close();

    if (written != len) {
        sd_card_write_failed();
        SD_MMC.remove(tmp_path);
        return false;
    }

    // Rename tmp over target (atomic on FAT32 — overwrites existing)
    if (SD_MMC.exists(path)) SD_MMC.remove(path);
    if (!SD_MMC.rename(tmp_path, path)) {
        sd_card_write_failed();
        return false;
    }

    sd_card_write_ok();
    return true;
}

// ---- Colony ID generation ----

// Three-word colony names: place + nature + nature (32 × 32 × 32 = 32768 combos)
static const char* const COLONY_W1[] = {
    "Barn","Glen","Moor","Dell","Holm","Brae","Dale","Fell",
    "Knoll","Lea","Mere","Copse","Glade","Briar","Holt","Cairn",
    "Cove","Dune","Ford","Heath","Lane","Mill","Nook","Pike",
    "Ridge","Shade","Tarn","Vale","Wold","Creek","Haven","Drift",
};
static const char* const COLONY_W2[] = {
    "Spring","Ember","Frost","Bloom","Stone","Thorn","Storm","Moss",
    "Wren","Fern","Brook","Ash","Birch","Dusk","Sage","Pine",
    "Flint","Rust","Dawn","Ivy","Hazel","Reed","Gorse","Slate",
    "Wheat","Lark","Thistle","Clover","Rowan","Swift","Otter","Hawk",
};
static const char* const COLONY_W3[] = {
    "Oak","Elm","Fox","Hare","Bee","Crow","Yew","Sedge",
    "Whin","Clay","Peat","Lime","Dock","Rue","Mace","Alder",
    "Sorrel","Vetch","Broom","Holly","Aspen","Finch","Rook","Kite",
    "Trout","Newt","Stoat","Heron","Shrew","Vole","Mink","Teal",
};

bool ConkerRegistry::_generate_colony_id() {
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    int w1 = r1 & 0x1F;         // 0-31
    int w2 = (r1 >> 5) & 0x1F;  // 0-31
    int w3 = (r2) & 0x1F;       // 0-31
    snprintf(_manifest.colony_id, sizeof(_manifest.colony_id),
             "%s%s%s", COLONY_W1[w1], COLONY_W2[w2], COLONY_W3[w3]);
    return true;
}

void ConkerRegistry::generate_colony_id() {
    _generate_colony_id();
}

// ---- Manifest I/O ----

bool ConkerRegistry::_load_manifest() {
    File f = SD_MMC.open("/colony/manifest.json", FILE_READ);
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[persist] manifest parse error: %s\r\n", err.c_str());
        return false;
    }

    strlcpy(_manifest.colony_id, doc["colony_id"] | "", sizeof(_manifest.colony_id));
    _manifest.schema         = doc["schema"] | 1;
    _manifest.founded_unix   = doc["founded_unix"] | 0;
    _manifest.next_lilguy_id = doc["next_lilguy_id"] | 1;
    _manifest.last_tick      = doc["last_tick"] | 0;
    _manifest.module_role    = doc["module_role"] | 0;

    // Colony stats
    JsonObject stats = doc["colony_stats"];
    _manifest.food_store         = stats["food_store"] | 0.0f;
    _manifest.food_total         = stats["food_total"] | 0.0f;
    _manifest.total_workers_born = stats["total_workers_born"] | 0;
    _manifest.total_workers_died = stats["total_workers_died"] | 0;
    _manifest.worker_census      = stats["worker_census"] | 0;

    // Queen state
    JsonObject qs = doc["queen_state"];
    _manifest.queen_state.reserves      = qs["reserves"] | 0.0f;
    _manifest.queen_state.hunger        = qs["hunger"] | 0.0f;
    _manifest.queen_state.founding_done = qs["founding_done"] | false;
    _manifest.queen_state.eggs_laid     = qs["eggs_laid"] | 0;
    _manifest.queen_state.egg_accum     = qs["egg_accum"] | 0.0f;
    _manifest.queen_state.x             = qs["x"] | (int8_t)Cfg::QUEEN_SPAWN_X;
    _manifest.queen_state.y             = qs["y"] | (int8_t)Cfg::QUEEN_SPAWN_Y;

    // Live positions
    _manifest.pos_count = 0;
    JsonArray pos = doc["positions"];
    for (size_t i = 0; i < pos.size() && _manifest.pos_count < ColonyManifest::MAX_POS; i++) {
        JsonObject p = pos[i];
        _manifest.positions[_manifest.pos_count++] = {
            p["id"] | (uint32_t)0,
            p["x"] | 0.0f,
            p["y"] | 0.0f
        };
    }

    return true;
}

bool ConkerRegistry::_save_manifest() {
    JsonDocument doc;
    doc["schema"]         = _manifest.schema;
    doc["colony_id"]      = _manifest.colony_id;
    doc["founded_unix"]   = _manifest.founded_unix;
    doc["next_lilguy_id"] = _manifest.next_lilguy_id;
    doc["last_tick"]      = _manifest.last_tick;
    doc["module_role"]    = _manifest.module_role;

    JsonObject stats = doc["colony_stats"].to<JsonObject>();
    stats["food_store"]         = _manifest.food_store;
    stats["food_total"]         = _manifest.food_total;
    stats["total_workers_born"] = _manifest.total_workers_born;
    stats["total_workers_died"] = _manifest.total_workers_died;
    stats["worker_census"]      = _manifest.worker_census;

    JsonObject qs = doc["queen_state"].to<JsonObject>();
    qs["reserves"]      = _manifest.queen_state.reserves;
    qs["hunger"]        = _manifest.queen_state.hunger;
    qs["founding_done"] = _manifest.queen_state.founding_done;
    qs["eggs_laid"]     = _manifest.queen_state.eggs_laid;
    qs["egg_accum"]     = _manifest.queen_state.egg_accum;
    qs["x"]             = _manifest.queen_state.x;
    qs["y"]             = _manifest.queen_state.y;

    // Live positions (avoids per-record file writes)
    JsonArray pos = doc["positions"].to<JsonArray>();
    for (int i = 0; i < _manifest.pos_count; i++) {
        JsonObject p = pos.add<JsonObject>();
        p["id"] = _manifest.positions[i].id;
        p["x"]  = _manifest.positions[i].x;
        p["y"]  = _manifest.positions[i].y;
    }

    // Use PSRAM-allocated buffer for larger manifest with positions
    size_t buf_size = 512 + _manifest.pos_count * 40;
    char* buf = (char*)malloc(buf_size);
    if (!buf) return false;
    size_t len = serializeJson(doc, buf, buf_size);
    bool ok = _atomic_write("/colony/manifest.json", buf, len);
    free(buf);
    return ok;
}

// ---- Record I/O ----

bool ConkerRegistry::_write_record(const IdentityRecord& rec) {
    char path[80];
    _record_path(path, sizeof(path), "lilguys", rec.id);

    // Ensure shard dir exists
    char shard[64];
    _shard_path(shard, sizeof(shard), "lilguys", rec.id);
    if (!SD_MMC.exists(shard)) SD_MMC.mkdir(shard);

    JsonDocument doc;
    doc["schema"]      = 1;
    doc["id"]          = rec.id;
    doc["name"]        = rec.name;
    doc["role"]        = rec.role;
    doc["is_pioneer"]  = rec.is_pioneer;
    doc["born_unix"]   = rec.born_unix;
    doc["died_unix"]   = rec.died_unix;
    doc["lifespan_ms"] = rec.lifespan_ms;
    doc["lived_ms"]    = rec.lived_ms;
    doc["scale_factor"] = rec.scale_factor;
    doc["tint_seed"]   = rec.tint_seed;
    doc["is_founder"]  = rec.is_founder;
    doc["tended_by"]   = rec.tended_by;
    doc["traits"]      = rec.traits;

    JsonArray pers = doc["personality"].to<JsonArray>();
    for (int i = 0; i < 8; i++) pers.add(rec.personality[i]);

    JsonObject pos = doc["last_pos"].to<JsonObject>();
    pos["chamber"] = rec.chamber_id;
    pos["x"]       = rec.last_x;
    pos["y"]       = rec.last_y;

    doc["last_state"] = rec.last_state;

    char buf[512];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    return _atomic_write(path, buf, len);
}

bool ConkerRegistry::_write_brood(const BroodRecord& rec) {
    char path[80];
    _record_path(path, sizeof(path), "brood", rec.id);

    char shard[64];
    _shard_path(shard, sizeof(shard), "brood", rec.id);
    if (!SD_MMC.exists(shard)) SD_MMC.mkdir(shard);

    JsonDocument doc;
    // Convert stage_start_ms (absolute millis) to elapsed for persistence
    uint32_t stage_elapsed = millis() - rec.stage_start_ms;

    doc["schema"]           = 1;
    doc["id"]               = rec.id;
    doc["stage"]            = rec.stage;
    doc["role"]             = rec.role;
    doc["x"]                = rec.x;
    doc["y"]                = rec.y;
    doc["hunger"]           = rec.hunger;
    doc["food_invested"]    = rec.food_invested;
    doc["stage_elapsed_ms"] = stage_elapsed;
    doc["total_duration_ms"] = (unsigned long)rec.total_duration_ms;
    doc["born_unix"]        = rec.born_unix;

    char buf[256];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    return _atomic_write(path, buf, len);
}

bool ConkerRegistry::_delete_brood_file(uint32_t id) {
    char path[80];
    _record_path(path, sizeof(path), "brood", id);
    if (SD_MMC.exists(path)) return SD_MMC.remove(path);
    return true;
}

// ---- Load living records at boot ----

bool ConkerRegistry::_load_living_records() {
    _alive_count = 0;
    // Walk shard directories
    File root = SD_MMC.open("/colony/lilguys");
    if (!root || !root.isDirectory()) return false;

    File shard_dir = root.openNextFile();
    while (shard_dir) {
        if (shard_dir.isDirectory() && shard_dir.name()[0] != '.') {
            File entry = shard_dir.openNextFile();
            while (entry) {
                if (!entry.isDirectory() && strstr(entry.name(), ".json")
                    && !strstr(entry.name(), ".tmp")) {
                    // Parse record
                    JsonDocument doc;
                    DeserializationError err = deserializeJson(doc, entry);
                    if (err) {
                        char corrupt_path[128];
                        strlcpy(corrupt_path, entry.path(), sizeof(corrupt_path));
                        Serial.printf("[persist] corrupt record: %s (%s)\r\n",
                                      corrupt_path, err.c_str());
                        entry.close();
                        _move_to_corrupt(corrupt_path, "lilguys");
                        entry = shard_dir.openNextFile();
                        continue;
                    }

                    uint32_t died = doc["died_unix"] | 0;
                    if (died == 0 && _alive_count < MAX_ALIVE) {
                        IdentityRecord& r = _alive[_alive_count];
                        r.id          = doc["id"] | 0;
                        strlcpy(r.name, doc["name"] | "", sizeof(r.name));
                        r.role        = doc["role"] | 0;
                        r.is_pioneer  = doc["is_pioneer"] | false;
                        r.born_unix   = doc["born_unix"] | 0;
                        r.died_unix   = 0;
                        r.lifespan_ms = doc["lifespan_ms"] | 0;
                        r.lived_ms    = doc["lived_ms"] | 0;
                        r.scale_factor = doc["scale_factor"] | 3.2f;
                        r.tint_seed   = doc["tint_seed"] | 0;
                        r.is_founder  = doc["is_founder"] | false;
                        r.tended_by   = doc["tended_by"] | 0;
                        r.traits      = doc["traits"] | 0;

                        JsonArray pers = doc["personality"];
                        for (int i = 0; i < 8 && i < (int)pers.size(); i++)
                            r.personality[i] = pers[i] | 0.0f;

                        JsonObject pos = doc["last_pos"];
                        r.chamber_id = pos["chamber"] | 0;
                        r.last_x     = pos["x"] | 0.0f;
                        r.last_y     = pos["y"] | 0.0f;
                        r.last_state = doc["last_state"] | 0;
                        r.dirty      = false;

                        _alive_count++;
                    }
                }
                entry.close();
                entry = shard_dir.openNextFile();
            }
        }
        shard_dir.close();
        shard_dir = root.openNextFile();
    }
    root.close();

    Serial.printf("[persist] loaded %d living records\r\n", _alive_count);
    return true;
}

bool ConkerRegistry::_load_brood_records() {
    _brood_count = 0;
    File root = SD_MMC.open("/colony/brood");
    if (!root || !root.isDirectory()) return true;  // no brood dir = fresh colony

    File shard_dir = root.openNextFile();
    while (shard_dir) {
        if (shard_dir.isDirectory() && shard_dir.name()[0] != '.') {
            File entry = shard_dir.openNextFile();
            while (entry) {
                if (!entry.isDirectory() && strstr(entry.name(), ".json")
                    && !strstr(entry.name(), ".tmp")) {
                    JsonDocument doc;
                    DeserializationError err = deserializeJson(doc, entry);
                    if (err) {
                        char corrupt_path[128];
                        strlcpy(corrupt_path, entry.path(), sizeof(corrupt_path));
                        entry.close();
                        _move_to_corrupt(corrupt_path, "brood");
                        entry = shard_dir.openNextFile();
                        continue;
                    }

                    if (_brood_count < MAX_BROOD_CACHE) {
                        BroodRecord& b = _brood[_brood_count];
                        b.id             = doc["id"] | 0;
                        b.stage          = doc["stage"] | 0;
                        b.role           = doc["role"] | 0;
                        b.x              = doc["x"] | 0;
                        b.y              = doc["y"] | 0;
                        b.hunger         = doc["hunger"] | 0.0f;
                        b.food_invested  = doc["food_invested"] | 0.0f;
                        b.total_duration_ms = doc["total_duration_ms"] | (uint32_t)0;
                        b.born_unix      = doc["born_unix"] | (uint32_t)0;
                        // Convert elapsed time back to absolute millis
                        uint32_t elapsed = doc["stage_elapsed_ms"] | 0;
                        b.stage_start_ms = millis() - elapsed;
                        _brood_count++;
                    }
                }
                entry.close();
                entry = shard_dir.openNextFile();
            }
        }
        shard_dir.close();
        shard_dir = root.openNextFile();
    }
    root.close();

    Serial.printf("[persist] loaded %d brood records\r\n", _brood_count);
    return true;
}

// ---- Corrupt file handling ----

void ConkerRegistry::_move_to_corrupt(const char* path, const char* subdir) {
    char dest[128];
    const char* filename = strrchr(path, '/');
    if (!filename) return;
    snprintf(dest, sizeof(dest), "/colony/%s/.corrupt%s", subdir, filename);
    SD_MMC.rename(path, dest);
    Serial.printf("[persist] moved corrupt file to %s\r\n", dest);
}

// ---- Clean stale .tmp files from previous interrupted writes ----

void ConkerRegistry::_clean_tmp_files() {
    // Walk lilguys shards
    File root = SD_MMC.open("/colony/lilguys");
    if (root && root.isDirectory()) {
        File shard = root.openNextFile();
        while (shard) {
            if (shard.isDirectory() && shard.name()[0] != '.') {
                File entry = shard.openNextFile();
                while (entry) {
                    if (strstr(entry.name(), ".tmp")) {
                        char p[128];
                        strlcpy(p, entry.path(), sizeof(p));
                        entry.close();
                        SD_MMC.remove(p);
                        Serial.printf("[persist] cleaned stale tmp: %s\r\n", p);
                    } else {
                        entry.close();
                    }
                    entry = shard.openNextFile();
                }
            }
            shard.close();
            shard = root.openNextFile();
        }
        root.close();
    }
}

// ---- Public API ----

PersistenceState ConkerRegistry::init() {
    if (sd_card_state() != SD_OK) {
        Serial.println("[persist] no SD card — degraded mode");
        _state = PERSIST_DEGRADED;
        return _state;
    }

    if (!_ensure_dirs()) {
        _state = PERSIST_DEGRADED;
        return _state;
    }

    _clean_tmp_files();

    // Try loading existing manifest
    if (_load_manifest()) {
        Serial.printf("[persist] manifest loaded — colony %s, next_id=%lu, tick=%lu\r\n",
                      _manifest.colony_id,
                      (unsigned long)_manifest.next_lilguy_id,
                      (unsigned long)_manifest.last_tick);
        _load_living_records();
        _load_brood_records();
        _state = PERSIST_OK;
    } else {
        // No manifest — will be created during migration (Case B)
        Serial.println("[persist] no manifest found — fresh colony (Case B)");
        _state = PERSIST_OK;
    }

    _last_flush_ms = millis();
    return _state;
}

uint32_t ConkerRegistry::allocate_id() {
    uint32_t id = _manifest.next_lilguy_id++;
    // Persist manifest immediately to avoid ID reuse on crash
    if (_state == PERSIST_OK) _save_manifest();
    return id;
}

bool ConkerRegistry::create(const IdentityRecord& rec) {
    if (_alive_count >= MAX_ALIVE) return false;
    // Guard against duplicate IDs (should never happen, but prevents data corruption)
    for (int i = 0; i < _alive_count; i++) {
        if (_alive[i].id == rec.id) {
            Serial.printf("[persist] WARNING: duplicate create for id=%lu, skipping\r\n",
                          (unsigned long)rec.id);
            return false;
        }
    }
    _alive[_alive_count] = rec;
    _alive[_alive_count].dirty = false;
    _alive_count++;

    if (_state == PERSIST_OK) return _write_record(rec);
    return true;
}

bool ConkerRegistry::update(uint32_t id, const IdentityRecord& rec) {
    for (int i = 0; i < _alive_count; i++) {
        if (_alive[i].id == id) {
            _alive[i] = rec;
            _alive[i].dirty = true;
            return true;
        }
    }
    return false;
}

IdentityRecord* ConkerRegistry::get(uint32_t id) {
    for (int i = 0; i < _alive_count; i++) {
        if (_alive[i].id == id) return &_alive[i];
    }
    return nullptr;
}

void ConkerRegistry::mark_dead(uint32_t id, uint32_t died_unix) {
    for (int i = 0; i < _alive_count; i++) {
        if (_alive[i].id == id) {
            _alive[i].died_unix = died_unix;
            _alive[i].dirty = false;  // about to flush
            if (_state == PERSIST_OK) _write_record(_alive[i]);

            // Remove from alive cache (swap-with-last)
            _alive[i] = _alive[_alive_count - 1];
            _alive_count--;
            return;
        }
    }
}

bool ConkerRegistry::create_brood(const BroodRecord& rec) {
    if (_brood_count >= MAX_BROOD_CACHE) return false;
    _brood[_brood_count] = rec;
    _brood_count++;

    if (_state == PERSIST_OK) return _write_brood(rec);
    return true;
}

BroodRecord* ConkerRegistry::get_brood(uint32_t id) {
    for (int i = 0; i < _brood_count; i++) {
        if (_brood[i].id == id) return &_brood[i];
    }
    return nullptr;
}

void ConkerRegistry::remove_brood(uint32_t id) {
    for (int i = 0; i < _brood_count; i++) {
        if (_brood[i].id == id) {
            if (_state == PERSIST_OK) _delete_brood_file(id);
            _brood[i] = _brood[_brood_count - 1];
            _brood_count--;
            return;
        }
    }
}

void ConkerRegistry::flush() {
    if (_state != PERSIST_OK) return;
    if (sd_card_state() != SD_OK) return;

    // Only write records with actual state changes (not position-only updates).
    // Position persistence is handled via the manifest's positions array.
    int flushed = 0;
    for (int i = 0; i < _alive_count; i++) {
        if (_alive[i].dirty) {
            if (_write_record(_alive[i])) {
                _alive[i].dirty = false;
                flushed++;
            }
        }
    }

    _last_flush_ms = millis();
    if (flushed > 0)
        Serial.printf("[persist] flushed %d records\r\n", flushed);
}

void ConkerRegistry::flush_manifest() {
    if (_state != PERSIST_OK) return;
    if (sd_card_state() != SD_OK) return;
    _save_manifest();
}
