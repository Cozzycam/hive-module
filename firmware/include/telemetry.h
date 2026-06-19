/* TelemetryLog — temporary per-conker mood/needs sampler for tuning.
 *
 * Every TELEMETRY_INTERVAL_MS, appends one CSV row per living conker to
 * /colony/telemetry/YYYY-MM-DD.csv on the LOCAL SD card. Each module logs its
 * own chamber, so COM3 (queen) and COM4 (satellite) each grow their own file.
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

class TelemetryLog {
public:
    void init();                                       // mkdir + read NVS flag
    void tick(const Chamber& ch, uint16_t module_id);  // call each loop; self-times
    void set_enabled(bool on);                         // persists to NVS
    bool enabled() const { return _enabled; }

    size_t dump_today();   // stream today's CSV to Serial (for telemetry_pull.py)
    void   print_status(); // file path + size + on/off

private:
    static constexpr uint32_t TELEMETRY_INTERVAL_MS = 10000;  // 10s sampling

    bool     _enabled = true;
    bool     _ready   = false;   // SD mounted + dir present
    uint32_t _last_ms = 0;

    void _today_path(char* buf, size_t buflen);
    void _sample(const Chamber& ch, uint16_t module_id);
};

extern TelemetryLog g_telemetry;
