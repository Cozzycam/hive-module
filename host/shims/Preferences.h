/* Host shim for <Preferences.h> (ESP32 NVS). In-memory, non-persistent:
 * getters return the supplied default, so e.g. renderer_load_floor_tint()
 * reads no stored tint and falls back to the default sand floor. Enough for
 * the render de-risk; a real host build would back this with localStorage /
 * IndexedDB. */
#pragma once
#include <cstdint>
#include <cstddef>

class Preferences {
public:
    bool begin(const char*, bool = false) { return true; }
    void end() {}
    bool clear() { return true; }
    bool remove(const char*) { return true; }
    bool isKey(const char*) { return false; }

    size_t   putString(const char*, const char*) { return 0; }
    size_t   getString(const char*, char* out, size_t n) { if (n && out) out[0] = '\0'; return 0; }
    uint32_t putUInt(const char*, uint32_t v) { return sizeof(v); }
    uint32_t getUInt(const char*, uint32_t def = 0) { return def; }
    uint64_t putULong64(const char*, uint64_t v) { return sizeof(v); }
    uint64_t getULong64(const char*, uint64_t def = 0) { return def; }
    uint64_t putULong(const char*, uint64_t v) { return sizeof(v); }
    uint64_t getULong(const char*, uint64_t def = 0) { return def; }
    int64_t  putLong64(const char*, int64_t v) { return sizeof(v); }
    int64_t  getLong64(const char*, int64_t def = 0) { return def; }
    uint32_t putLong(const char*, uint32_t v) { return sizeof(v); }
    uint32_t getLong(const char*, uint32_t def = 0) { return def; }
    uint16_t putUShort(const char*, uint16_t v) { return sizeof(v); }
    uint16_t getUShort(const char*, uint16_t def = 0) { return def; }
    int32_t  putInt(const char*, int32_t v) { return sizeof(v); }
    int32_t  getInt(const char*, int32_t def = 0) { return def; }
    uint8_t  putUChar(const char*, uint8_t v) { return sizeof(v); }
    uint8_t  getUChar(const char*, uint8_t def = 0) { return def; }
    bool     putBool(const char*, bool v) { return v; }
    bool     getBool(const char*, bool def = false) { return def; }
    float    putFloat(const char*, float v) { return v; }
    float    getFloat(const char*, float def = 0) { return def; }
    size_t   putBytes(const char*, const void*, size_t n) { return n; }
    size_t   getBytes(const char*, void*, size_t) { return 0; }
    size_t   getBytesLength(const char*) { return 0; }
};
