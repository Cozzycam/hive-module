/* Host shim for <SD_MMC.h>. A phone module has no SD card: sd_card_init()
 * returns false (see host/stubs_hw.cpp), so persistence takes its in-RAM
 * "fresh colony" path and every SD_MMC/File call is guarded off. This shim
 * therefore only needs to COMPILE and LINK — all operations are inert and
 * return safe defaults. (Cross-reload persistence will later back this with
 * Emscripten IDBFS; for now the colony lives in RAM + the state blob.) */
#pragma once
#include <cstdint>
#include <cstddef>

// Arduino open-mode constants.
#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

class File {
public:
    File() {}
    explicit operator bool() const { return false; }   // always an invalid handle
    int   read() { return -1; }
    size_t read(uint8_t*, size_t) { return 0; }
    size_t readBytes(char*, size_t) { return 0; }
    size_t write(uint8_t) { return 0; }
    size_t write(const uint8_t*, size_t) { return 0; }
    size_t print(const char*) { return 0; }
    size_t println(const char*) { return 0; }
    size_t println() { return 0; }
    int   available() { return 0; }
    size_t size() { return 0; }
    size_t position() { return 0; }
    bool  seek(uint32_t) { return false; }
    void  flush() {}
    void  close() {}
    const char* name() { return ""; }
    const char* path() { return ""; }
    bool  isDirectory() { return false; }
    File  openNextFile() { return File(); }
};

class SDMMCFS {
public:
    bool begin(const char* = "/sdcard", bool = false) { return false; }
    bool begin(const char*, bool, bool, int, uint8_t) { return false; }
    void end() {}
    File open(const char*, const char* = FILE_READ) { return File(); }
    bool exists(const char*) { return false; }
    bool remove(const char*) { return false; }
    bool rename(const char*, const char*) { return false; }
    bool mkdir(const char*) { return false; }
    bool rmdir(const char*) { return false; }
    uint64_t totalBytes() { return 0; }
    uint64_t usedBytes() { return 0; }
    uint64_t cardSize() { return 0; }
    void setPins(int, int, int) {}
};

extern SDMMCFS SD_MMC;
