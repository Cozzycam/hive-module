/* Host shim for Arduino.h — just enough of the Arduino/ESP32 runtime for the
 * sim + renderer to compile and run off-device (native or WASM via Emscripten).
 * NOT a faithful Arduino; only what the compiled translation units actually
 * touch. See host/README.md. */
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <esp_random.h>   // esp_random() — also needed before names.h (name_random uses it)

// ---- Time ----
// millis()/micros() read a host-driven clock. host_main advances g_host_millis
// so animation timing is deterministic and reproducible.
extern unsigned long g_host_millis;
static inline unsigned long millis() { return g_host_millis; }
static inline unsigned long micros() { return g_host_millis * 1000UL; }
static inline void delay(unsigned long) {}
static inline void delayMicroseconds(unsigned int) {}
static inline void yield() {}

// ---- Random (Arduino) ----
static inline long random(long hi) { return hi > 0 ? (long)(rand() % hi) : 0; }
static inline long random(long lo, long hi) {
    return hi > lo ? lo + (long)(rand() % (hi - lo)) : lo;
}
static inline void randomSeed(unsigned long s) { srand((unsigned)s); }

// ---- min/max/constrain (Arduino macros) ----
#ifndef _min
#define _min(a, b) ((a) < (b) ? (a) : (b))
#define _max(a, b) ((a) > (b) ? (a) : (b))
#endif
// Arduino provides min/max as macros; we use function templates so they don't
// clobber std::min/std::max in <algorithm> (which the renderer includes).
template <typename A, typename B>
static inline auto max(A a, B b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
template <typename A, typename B>
static inline auto min(A a, B b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template <typename T> static inline T constrain(T x, T lo, T hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// ---- PSRAM alloc → plain malloc on host ----
static inline void* ps_malloc(size_t n) { return malloc(n); }
static inline void* ps_calloc(size_t n, size_t s) { return calloc(n, s); }
static inline void* heap_caps_malloc(size_t n, uint32_t) { return malloc(n); }

// ---- Serial → stderr (keeps stdout free for data) ----
struct HostSerial {
    void begin(unsigned long) {}
    void println() { fputc('\n', stderr); }
    void println(const char* s) { fprintf(stderr, "%s\n", s ? s : ""); }
    void println(int v) { fprintf(stderr, "%d\n", v); }
    void print(const char* s) { fprintf(stderr, "%s", s ? s : ""); }
    void print(int v) { fprintf(stderr, "%d", v); }
    template <typename... A> int printf(const char* f, A... a) {
        return fprintf(stderr, f, a...);
    }
    void flush() {}
    explicit operator bool() const { return true; }
};
extern HostSerial Serial;

// ---- Minimal Arduino String (backed by std::string) ----
class String {
    std::string s;
public:
    String() {}
    String(const char* c) : s(c ? c : "") {}
    String(const std::string& o) : s(o) {}
    String(int v) : s(std::to_string(v)) {}
    String(unsigned v) : s(std::to_string(v)) {}
    String(long v) : s(std::to_string(v)) {}
    const char* c_str() const { return s.c_str(); }
    unsigned length() const { return (unsigned)s.length(); }
    char operator[](int i) const { return s[i]; }
    String operator+(const String& o) const { return String(s + o.s); }
    String& operator+=(const String& o) { s += o.s; return *this; }
    bool operator==(const String& o) const { return s == o.s; }
    bool equals(const String& o) const { return s == o.s; }
};

// ==================================================================
//  FreeRTOS shims — renderer pulls these transitively via Arduino.h on
//  ESP32. Here they only need to LINK: the async-flush task never spawns
//  (xTaskCreatePinnedToCore returns failure → renderer uses its synchronous
//  fallback), and host_main reads the canvas framebuffer directly rather
//  than flushing. So queue/semaphore ops are inert.
// ==================================================================
typedef void* TaskHandle_t;
typedef void* QueueHandle_t;
typedef void* SemaphoreHandle_t;
typedef int   BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffUL
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

static inline QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t) { return (QueueHandle_t)1; }
static inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) { return pdFALSE; }
static inline BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t) { return pdTRUE; }
static inline SemaphoreHandle_t xSemaphoreCreateBinary() { return (SemaphoreHandle_t)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
// Return failure so the renderer keeps _flushw_task null and flushes synchronously.
static inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t,
                                                 void*, UBaseType_t, TaskHandle_t*, BaseType_t) {
    return pdFAIL;
}
static inline void vTaskDelay(TickType_t) {}
