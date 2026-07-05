#include "chores.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// One work item. Fixed-size headers + owned heap payload keeps the queue
// item a plain pointer and the failure paths simple.
struct ChoreItem {
    uint8_t  type;
    uint32_t token;
    char     url[196];      // HTTP: full URL
    char     path[128];     // SD: destination path
    char     hmac[65];      // HTTP POST: X-HMAC-SHA256 value ('\0' = omit)
    char     enroll[65];    // HTTP POST: X-Enroll-Secret value ('\0' = omit)
    char*    payload;       // owned heap (POST body / file contents)
    size_t   payload_len;
};

static QueueHandle_t _work_q = nullptr;
static QueueHandle_t _result_q = nullptr;
static SemaphoreHandle_t _sd_mutex = nullptr;
// Queued + currently executing. Incremented on core 1, decremented on
// core 0 — must be atomic or chores_drain() can miscount and hang/underwait.
static std::atomic<int> _in_flight{0};

void chores_sd_lock() {
    if (_sd_mutex) xSemaphoreTakeRecursive(_sd_mutex, portMAX_DELAY);
}
void chores_sd_unlock() {
    if (_sd_mutex) xSemaphoreGiveRecursive(_sd_mutex);
}

// ---- Executors (worker task context — no sim state, no NVS) ----

static int _do_http(const ChoreItem& it, char** body_out) {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.begin(client, it.url);
    http.setTimeout(10000);

    int code;
    if (it.type == CHORE_HTTP_POST) {
        http.addHeader("Content-Type", "application/json");
        if (it.hmac[0])   http.addHeader("X-HMAC-SHA256", it.hmac);
        if (it.enroll[0]) http.addHeader("X-Enroll-Secret", it.enroll);
        code = http.POST((uint8_t*)it.payload, it.payload_len);
    } else {
        code = http.GET();
        if (code >= 200 && code < 300) {
            String s = http.getString();
            *body_out = (char*)malloc(s.length() + 1);
            if (*body_out) memcpy(*body_out, s.c_str(), s.length() + 1);
        }
    }
    http.end();
    return code;
}

static int _do_sd_write(const ChoreItem& it) {
    char tmp_path[140];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", it.path);

    chores_sd_lock();
    File f = SD_MMC.open(tmp_path, FILE_WRITE);
    if (!f) { chores_sd_unlock(); return 0; }
    size_t written = f.write((const uint8_t*)it.payload, it.payload_len);
    f.flush();
    f.close();
    if (written != it.payload_len) {
        SD_MMC.remove(tmp_path);
        chores_sd_unlock();
        return 0;
    }
    if (SD_MMC.exists(it.path)) SD_MMC.remove(it.path);
    bool ok = SD_MMC.rename(tmp_path, it.path);
    chores_sd_unlock();
    return ok ? 1 : 0;
}

static void _chores_task(void*) {
    ChoreItem* it;
    for (;;) {
        if (xQueueReceive(_work_q, &it, portMAX_DELAY) != pdTRUE) continue;

        ChoreResult res = {};
        res.type = it->type;
        res.token = it->token;
        res.body = nullptr;

        if (it->type == CHORE_SD_WRITE) {
            res.status = _do_sd_write(*it);
        } else {
            res.status = _do_http(*it, &res.body);
        }

        free(it->payload);
        free(it);

        // Result queue full = receiver has stopped draining (shouldn't
        // happen; pump runs every loop pass). Drop rather than block I/O.
        if (xQueueSend(_result_q, &res, 0) != pdTRUE) {
            free(res.body);
        }
        _in_flight--;
    }
}

// ---- Public API (main loop context) ----

void chores_begin() {
    if (_work_q) return;
    _sd_mutex = xSemaphoreCreateRecursiveMutex();
    _work_q = xQueueCreate(16, sizeof(ChoreItem*));   // task reads this — set first
    _result_q = xQueueCreate(16, sizeof(ChoreResult));
    // Core 0 = the WiFi core; it idles between stack work while core 1
    // runs the sim + render loop. Priority 1: above idle, below WiFi.
    if (!_work_q || !_result_q
            || xTaskCreatePinnedToCore(_chores_task, "chores", 12288, nullptr,
                                       1, nullptr, 0) != pdPASS) {
        // No worker — chores_ready() stays false and every caller falls
        // back to its old synchronous path. Degraded, not broken.
        Serial.println("[chores] WORKER FAILED TO START — synchronous I/O fallback");
        _work_q = nullptr;
        return;
    }
    Serial.println("[chores] worker up (core 0)");
}

bool chores_ready() { return _work_q != nullptr; }

static bool _submit(ChoreItem* it) {
    if (!_work_q) { free(it->payload); free(it); return false; }
    _in_flight++;
    if (xQueueSend(_work_q, &it, 0) != pdTRUE) {
        _in_flight--;
        free(it->payload);
        free(it);
        return false;
    }
    return true;
}

bool chores_submit_http(uint8_t type, uint32_t token, const char* url,
                        char* body, size_t body_len,
                        const char* hmac_hex, const char* enroll) {
    ChoreItem* it = (ChoreItem*)calloc(1, sizeof(ChoreItem));
    if (!it) { free(body); return false; }
    it->type = type;
    it->token = token;
    strlcpy(it->url, url, sizeof(it->url));
    if (hmac_hex) strlcpy(it->hmac, hmac_hex, sizeof(it->hmac));
    if (enroll)   strlcpy(it->enroll, enroll, sizeof(it->enroll));
    it->payload = body;
    it->payload_len = body_len;
    return _submit(it);
}

bool chores_submit_sd_write(uint32_t token, const char* path,
                            char* data, size_t len) {
    ChoreItem* it = (ChoreItem*)calloc(1, sizeof(ChoreItem));
    if (!it) { free(data); return false; }
    it->type = CHORE_SD_WRITE;
    it->token = token;
    strlcpy(it->path, path, sizeof(it->path));
    it->payload = data;
    it->payload_len = len;
    return _submit(it);
}

bool chores_poll_result(ChoreResult& out) {
    if (!_result_q) return false;
    return xQueueReceive(_result_q, &out, 0) == pdTRUE;
}

void chores_drain(uint32_t timeout_ms) {
    if (!_work_q) return;
    uint32_t start = millis();
    while (_in_flight > 0 && millis() - start < timeout_ms) {
        delay(5);
    }
}
