/* VPS push — queen pushes colony state + events to remote server.
 * HTTPS POST with HMAC-SHA256 authentication.
 * Runs every 30s; buffers locally if VPS unreachable. */
#pragma once

class Coordinator;

// Initialize VPS push (reads secret from NVS, starts push timer)
void vps_push_init();

// Call periodically from main loop (handles timing internally)
void vps_push_tick(Coordinator& coord);

// Set the HMAC secret (stored in NVS, 32 bytes base64-encoded)
void vps_push_set_secret(const char* base64_secret);

// Set the VPS endpoint URL
void vps_push_set_endpoint(const char* url);
