/* API JSON serialization — produces wire-protocol payloads.
 *
 * Each function writes to a caller-provided buffer (heap-allocated).
 * Returns the number of bytes written, or 0 on failure.
 * Phase 7 will serve these over HTTP; Phase 6 exposes via serial.
 */
#pragma once
#include <cstdint>
#include <cstddef>

class Coordinator;

// GET /api/v1/colony
size_t api_colony_json(Coordinator& coord, char* buf, size_t buflen);

// GET /api/v1/lilguys?living=true&limit=N&offset=O
size_t api_lilguys_json(Coordinator& coord, char* buf, size_t buflen,
                        int limit, int offset);

// GET /api/v1/lilguys/<id>
size_t api_lilguy_detail_json(Coordinator& coord, uint32_t id,
                              char* buf, size_t buflen);

// GET /api/v1/health
size_t api_health_json(Coordinator& coord, char* buf, size_t buflen);
