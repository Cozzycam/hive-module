/* The chores worker — a core-0 task that does the queen's slow I/O
 * (SD record writes, VPS HTTP) so the main loop never blocks between
 * frames. The 30s "jarring pause" (issue #56) was never the QSPI flush:
 * it was 6 record writes + 3 HTTP round trips running ON the loop.
 *
 * Contract: the main loop serializes all payloads (no sim state crosses
 * the boundary — only heap strings), submits them, and polls results on
 * every pass. Submitted heap buffers are OWNED by the worker (it frees
 * them); result bodies are owned by the receiver (free after handling).
 * All SD writes for a given path funnel through the one FIFO queue, so
 * write order is preserved (a queued needs-refresh can never land after
 * the death write that supersedes it). */
#pragma once
#include <cstdint>
#include <cstddef>

enum ChoreType : uint8_t {
    CHORE_HTTP_POST = 1,
    CHORE_HTTP_GET  = 2,
    CHORE_SD_WRITE  = 3,   // atomic: write <path>.tmp, then rename over <path>
};

struct ChoreResult {
    uint8_t  type;
    uint32_t token;    // caller's correlation id (0 = fire-and-forget)
    int      status;   // HTTP code; SD write: 1 ok / 0 failed
    char*    body;     // heap; GET response body (else nullptr). Receiver frees.
};

// Spawn the worker task (idempotent; safe before WiFi is up).
void chores_begin();
bool chores_ready();

// Submit HTTP work. `url` is the full prebuilt URL. `body` is a heap buffer
// the worker takes ownership of (nullptr for GET). `hmac_hex`/`enroll` are
// optional header values (copied; may be nullptr).
bool chores_submit_http(uint8_t type, uint32_t token, const char* url,
                        char* body, size_t body_len,
                        const char* hmac_hex, const char* enroll);

// Submit an atomic SD file write. `data` is heap; worker takes ownership.
bool chores_submit_sd_write(uint32_t token, const char* path,
                            char* data, size_t len);

// Non-blocking result poll; true if `out` was filled.
bool chores_poll_result(ChoreResult& out);

// Block (with timeout) until all queued work has been executed. For paths
// that read/wipe files the queue may still be writing (revive, colony wipe,
// pre-reboot).
void chores_drain(uint32_t timeout_ms = 5000);

// SD access guard, shared by the worker and every main-loop SD user
// (journal appends/reads, brood writes, boot loads happen before the
// worker exists). Recursive — nesting is fine.
void chores_sd_lock();
void chores_sd_unlock();
