/* HTTP API server — ESPAsyncWebServer serving Phase 6 endpoints. */
#include "http_server.h"
#include "api_json.h"
#include "coordinator.h"
#include "journal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

static AsyncWebServer* _server = nullptr;
static Coordinator* _coord = nullptr;
static bool _running = false;

void http_server_start(Coordinator* coord) {
    if (_running || !WiFi.isConnected()) return;
    _coord = coord;

    _server = new AsyncWebServer(80);

    // GET /api/v1/health
    _server->on("/api/v1/health", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[256];
        api_health_json(*_coord, buf, sizeof(buf));
        req->send(200, "application/json", buf);
    });

    // GET /api/v1/colony
    _server->on("/api/v1/colony", HTTP_GET, [](AsyncWebServerRequest* req) {
        char* buf = (char*)malloc(4096);
        if (!buf) { req->send(500); return; }
        size_t len = api_colony_json(*_coord, buf, 4096);
        req->send(200, "application/json", buf);
        free(buf);
    });

    // GET /api/v1/lilguys
    _server->on("/api/v1/lilguys", HTTP_GET, [](AsyncWebServerRequest* req) {
        int limit = 100, offset = 0;
        if (req->hasParam("limit")) limit = req->getParam("limit")->value().toInt();
        if (req->hasParam("offset")) offset = req->getParam("offset")->value().toInt();
        if (limit < 1) limit = 1;
        if (limit > 200) limit = 200;

        char* buf = (char*)malloc(8192);
        if (!buf) { req->send(500); return; }
        size_t len = api_lilguys_json(*_coord, buf, 8192, limit, offset);
        req->send(200, "application/json", buf);
        free(buf);
    });

    // GET /api/v1/lilguys/<id>
    _server->on("^\\/api\\/v1\\/lilguys\\/(\\d+)$", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            uint32_t id = req->pathArg(0).toInt();
            char* buf = (char*)malloc(2048);
            if (!buf) { req->send(500); return; }
            size_t len = api_lilguy_detail_json(*_coord, id, buf, 2048);
            if (len == 0) {
                req->send(404, "application/json", "{\"error\":\"not found\"}");
            } else {
                req->send(200, "application/json", buf);
            }
            free(buf);
        });

    // GET /api/v1/events
    _server->on("/api/v1/events", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Flush journal, then read today's events
        _coord->journal.flush();
        String response = "{\"schema\":1,\"results\":[";
        bool first = true;
        _coord->journal.read_day(g_tod.unix_time,
            [](const char* line, void* ctx) -> bool {
                String* resp = (String*)ctx;
                if (resp->length() > 16000) return false;  // cap response size
                if (resp->length() > 23) *resp += ",";  // not first
                *resp += line;
                return true;
            }, &response);
        response += "]}";
        req->send(200, "application/json", response);
    });

    // GET /api/v1/lilguys/<id>/events
    _server->on("^\\/api\\/v1\\/lilguys\\/(\\d+)\\/events$", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            uint32_t id = req->pathArg(0).toInt();
            uint32_t since = 0;
            if (req->hasParam("since")) since = req->getParam("since")->value().toInt();

            String response = "{\"schema\":1,\"results\":[";
            char id_pattern[24];
            snprintf(id_pattern, sizeof(id_pattern), "\"lilguy\":%lu", (unsigned long)id);

            _coord->journal.read_lilguy(id, since,
                [](const char* line, void* ctx) -> bool {
                    String* resp = (String*)ctx;
                    if (resp->length() > 16000) return false;
                    if (resp->length() > 23) *resp += ",";
                    *resp += line;
                    return true;
                }, &response);
            response += "]}";
            req->send(200, "application/json", response);
        });

    _server->begin();
    _running = true;
    Serial.printf("[http] server started on port 80 — http://%s/api/v1/colony\n",
                  WiFi.localIP().toString().c_str());
}

void http_server_stop() {
    if (!_running) return;
    _server->end();
    delete _server;
    _server = nullptr;
    _running = false;
    Serial.println("[http] server stopped");
}

bool http_server_running() { return _running; }
