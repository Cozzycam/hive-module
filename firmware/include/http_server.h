/* HTTP API server — serves Phase 6 JSON endpoints on LAN.
 * Uses ESPAsyncWebServer (runs on its own FreeRTOS task).
 * Only starts if WiFi is connected. */
#pragma once

class Coordinator;

void http_server_start(Coordinator* coord);
void http_server_stop();
bool http_server_running();
