/* Host shim for <pgmspace.h> — on ESP32 PROGMEM lives in flash and needs
 * pgm_read_* accessors; on host it's ordinary memory, so these are plain
 * dereferences. */
#pragma once
#include <cstdint>
#include <cstring>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PGM_P
#define PGM_P const char*
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#ifndef F
#define F(s) (s)
#endif

static inline uint8_t  pgm_read_byte(const void* p)  { return *(const uint8_t*)p; }
static inline uint16_t pgm_read_word(const void* p)  { return *(const uint16_t*)p; }
static inline uint32_t pgm_read_dword(const void* p) { return *(const uint32_t*)p; }
static inline void*    pgm_read_ptr(const void* p)   { return *(void* const*)p; }
static inline float    pgm_read_float(const void* p) { return *(const float*)p; }

#define memcpy_P  memcpy
#define strcpy_P  strcpy
#define strncpy_P strncpy
#define strcmp_P  strcmp
#define strlen_P  strlen
