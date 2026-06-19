/* TelemetryLog — temporary mood/needs + bond sampler for tuning.
 *
 * Two parallel CSV streams on the LOCAL SD card (each module logs its own state,
 * so COM3 and COM4 each grow their own files):
 *   - /colony/telemetry/YYYY-MM-DD.csv        one row per living conker, every
 *                                             TELEMETRY_INTERVAL_MS (10s)
 *   - /colony/telemetry/bonds-YYYY-MM-DD.csv  one row per directed bond (incl.
 *                                             sub-threshold), every BONDS_INTERVAL_MS (30s)
 *
 * Plain append, no RAM ring buffer: every sample is committed independently, so
 * a reset can at worst tear the final line — no risk of losing a buffered batch
 * (cf. the brood/identity write-window losses). Telemetry rows are disposable.
 *
 * Disposable by design: `telemetry off` to stop (state persisted in NVS so it
 * stays off across reboots), or delete /colony/telemetry to reclaim the space.
 * Default ON — this exists for an active tuning session.
 */
#pragma once
#include <cstdint>
#include <cstddef>

struct Chamber;
class BondStore;
class ConkerRegistry;

class TelemetryLog {
public:
    void init();                                       // mkdir + read NVS flag
    void tick(const Chamber& ch, uint16_t module_id);  // call each loop; self-times (10s)
    void tick_bonds(const BondStore& bs, ConkerRegistry& reg,
                    uint16_t module_id);               // call each loop; self-times (30s)
    void set_enabled(bool on);                         // persists to NVS
    bool enabled() const { return _enabled; }

    size_t dump_today();        // stream today's needs CSV to Serial
    size_t dump_bonds_today();  // stream today's bonds CSV to Serial
    void   print_status();      // file paths + sizes + on/off

private:
    static constexpr uint32_t TELEMETRY_INTERVAL_MS = 10000;  // 10s needs sampling
    static constexpr uint32_t BONDS_INTERVAL_MS     = 30000;  // 30s bond sampling

    bool     _enabled       = true;
    bool     _ready         = false;   // SD mounted + dir present
    uint32_t _last_ms       = 0;
    uint32_t _last_bonds_ms = 0;

    void _dated_path(const char* prefix, char* buf, size_t buflen);
    void _sample(const Chamber& ch, uint16_t module_id);
    void _sample_bonds(const BondStore& bs, ConkerRegistry& reg, uint16_t module_id);
    size_t _dump(const char* prefix);
};

extern TelemetryLog g_telemetry;
