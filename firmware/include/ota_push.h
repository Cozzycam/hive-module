/* OTA cascade — queen pushes firmware to satellites over WiFi.
 *
 * Queen side (serial command "push"):
 *   1. Broadcasts OTA announce on ESP-NOW (channel 1)
 *   2. Connects to home WiFi, starts HTTP server
 *   3. Broadcasts server IP on ESP-NOW (now on WiFi channel)
 *   4. Serves firmware partition to connecting satellites
 *   5. Reboots
 *
 * Satellite side (triggered by OTA announce):
 *   1. Connects to home WiFi (same AP = same channel as queen)
 *   2. Waits for queen's server IP via ESP-NOW
 *   3. Downloads firmware over HTTP, flashes, reboots
 */
#pragma once

// Queen: push firmware to all connected satellites. Blocking, reboots on exit.
void ota_push();

// Satellite: download firmware from queen. Blocking, reboots on exit.
void ota_satellite_update();
