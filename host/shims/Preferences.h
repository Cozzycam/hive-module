/* Host shim for <Preferences.h> (ESP32 NVS). Backed by a real in-process
 * key-value store (namespace:key -> value) so code that stages state across
 * a boot within one process works — e.g. the Gateway summon staging
 * (host_stage_summon -> _summon_staged_apply at founding). Non-persistent
 * across processes; getters fall back to the supplied default like NVS. */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>

#include "Arduino.h"   // String

class Preferences {
    static std::map<std::string, std::string>& _store() {
        static std::map<std::string, std::string> s;
        return s;
    }
    std::string _ns;
    std::string _k(const char* key) const { return _ns + ":" + (key ? key : ""); }
    bool _has(const char* key) const { return _store().count(_k(key)) > 0; }
    const std::string& _raw(const char* key) const { return _store()[_k(key)]; }
    template <typename T> T _get(const char* key, T def) const {
        if (!_has(key)) return def;
        const std::string& v = _store()[_k(key)];
        if (v.size() != sizeof(T)) return def;
        T out; memcpy(&out, v.data(), sizeof(T)); return out;
    }
    template <typename T> size_t _put(const char* key, T v) {
        _store()[_k(key)] = std::string((const char*)&v, sizeof(T));
        return sizeof(T);
    }

public:
    bool begin(const char* ns, bool = false) { _ns = ns ? ns : ""; return true; }
    void end() {}
    bool clear() {
        auto& s = _store();
        for (auto it = s.begin(); it != s.end();)
            it = (it->first.compare(0, _ns.size() + 1, _ns + ":") == 0) ? s.erase(it) : ++it;
        return true;
    }
    bool remove(const char* key) { return _store().erase(_k(key)) > 0; }
    bool isKey(const char* key) { return _has(key); }

    size_t putString(const char* key, const char* v) {
        _store()[_k(key)] = v ? v : "";
        return v ? strlen(v) : 0;
    }
    size_t putString(const char* key, const String& v) { return putString(key, v.c_str()); }
    size_t getString(const char* key, char* out, size_t n) {
        if (!n || !out) return 0;
        if (!_has(key)) { out[0] = '\0'; return 0; }
        const std::string& v = _raw(key);
        size_t len = v.size() < n - 1 ? v.size() : n - 1;
        memcpy(out, v.data(), len);
        out[len] = '\0';
        return len;
    }
    String getString(const char* key, String def = String()) {
        return _has(key) ? String(_raw(key)) : def;
    }

    uint32_t putUInt(const char* k, uint32_t v) { return (uint32_t)_put(k, v); }
    uint32_t getUInt(const char* k, uint32_t def = 0) { return _get(k, def); }
    uint64_t putULong64(const char* k, uint64_t v) { return (uint64_t)_put(k, v); }
    uint64_t getULong64(const char* k, uint64_t def = 0) { return _get(k, def); }
    uint64_t putULong(const char* k, uint64_t v) { return (uint64_t)_put(k, v); }
    uint64_t getULong(const char* k, uint64_t def = 0) { return _get(k, def); }
    int64_t  putLong64(const char* k, int64_t v) { return (int64_t)_put(k, v); }
    int64_t  getLong64(const char* k, int64_t def = 0) { return _get(k, def); }
    uint32_t putLong(const char* k, uint32_t v) { return (uint32_t)_put(k, v); }
    uint32_t getLong(const char* k, uint32_t def = 0) { return _get(k, def); }
    uint16_t putUShort(const char* k, uint16_t v) { return (uint16_t)_put(k, v); }
    uint16_t getUShort(const char* k, uint16_t def = 0) { return _get(k, def); }
    int32_t  putInt(const char* k, int32_t v) { return (int32_t)_put(k, v); }
    int32_t  getInt(const char* k, int32_t def = 0) { return _get(k, def); }
    uint8_t  putUChar(const char* k, uint8_t v) { return (uint8_t)_put(k, v); }
    uint8_t  getUChar(const char* k, uint8_t def = 0) { return _get(k, def); }
    bool     putBool(const char* k, bool v) { _put(k, v); return v; }
    bool     getBool(const char* k, bool def = false) { return _get(k, def); }
    float    putFloat(const char* k, float v) { _put(k, v); return v; }
    float    getFloat(const char* k, float def = 0) { return _get(k, def); }
    size_t   putBytes(const char* k, const void* v, size_t n) {
        _store()[_k(k)] = std::string((const char*)v, n);
        return n;
    }
    size_t   getBytes(const char* k, void* out, size_t n) {
        if (!_has(k)) return 0;
        const std::string& v = _raw(k);
        size_t len = v.size() < n ? v.size() : n;
        memcpy(out, v.data(), len);
        return len;
    }
    size_t   getBytesLength(const char* k) { return _has(k) ? _raw(k).size() : 0; }
};
