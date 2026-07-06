/* Host shim for <SD_MMC.h> — backed by a REAL filesystem (Emscripten MEMFS by
 * default; IDBFS later for cross-reload persistence).
 *
 * Why real, not inert: the firmware only assigns conker identities (names,
 * personalities — the Characters roster) when the registry is NOT in
 * PERSIST_DEGRADED, and it degrades whenever sd_card_state() != SD_OK. So a
 * working (in-memory) card is what makes the phone colony's guys real. All the
 * firmware's own persistence runs unchanged on top of this. */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

// Value type with raw handles + explicit close (matches the firmware's usage:
// `File f = open(...); ...; f.close();`). No RAII close, so return-by-value
// copies don't double-close; unclosed handles are a minor within-session leak.
class File {
public:
    File() {}
    File(FILE* fp, const char* path) : _fp(fp), _path(path ? path : "") {}
    File(DIR* dir, const char* path) : _dir(dir), _isdir(true), _path(path ? path : "") {}

    explicit operator bool() const { return _fp != nullptr || _dir != nullptr; }
    int    read() { return _fp ? fgetc(_fp) : -1; }
    size_t read(uint8_t* b, size_t n) { return _fp ? fread(b, 1, n, _fp) : 0; }
    size_t readBytes(char* b, size_t n) { return _fp ? fread(b, 1, n, _fp) : 0; }
    size_t write(uint8_t c) { return _fp ? (fputc(c, _fp) != EOF ? 1 : 0) : 0; }
    size_t write(const uint8_t* b, size_t n) { return _fp ? fwrite(b, 1, n, _fp) : 0; }
    size_t print(const char* s) { return (_fp && s) ? fwrite(s, 1, strlen(s), _fp) : 0; }
    size_t println(const char* s) { size_t n = print(s); if (_fp) { fputc('\n', _fp); n++; } return n; }
    size_t println() { if (_fp) { fputc('\n', _fp); return 1; } return 0; }
    int    available() { if (!_fp) return 0; long c = ftell(_fp), e; fseek(_fp, 0, SEEK_END); e = ftell(_fp); fseek(_fp, c, SEEK_SET); return (int)(e - c); }
    size_t size() { if (!_fp) return 0; long c = ftell(_fp), e; fseek(_fp, 0, SEEK_END); e = ftell(_fp); fseek(_fp, c, SEEK_SET); return (size_t)e; }
    size_t position() { return _fp ? (size_t)ftell(_fp) : 0; }
    bool   seek(uint32_t p) { return _fp ? fseek(_fp, p, SEEK_SET) == 0 : false; }
    void   flush() { if (_fp) fflush(_fp); }
    void   close() { if (_fp) { fclose(_fp); _fp = nullptr; } if (_dir) { closedir(_dir); _dir = nullptr; } }
    const char* name() { const char* s = strrchr(_path.c_str(), '/'); return s ? s + 1 : _path.c_str(); }
    const char* path() { return _path.c_str(); }
    bool   isDirectory() { return _isdir; }
    File   openNextFile() {
        if (!_dir) return File();
        struct dirent* e;
        while ((e = readdir(_dir))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string p = _path + "/" + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) return File(opendir(p.c_str()), p.c_str());
            return File(fopen(p.c_str(), "rb"), p.c_str());
        }
        return File();
    }
private:
    FILE* _fp = nullptr;
    DIR*  _dir = nullptr;
    bool  _isdir = false;
    std::string _path;
};

class SDMMCFS {
public:
    bool begin(const char* = "/sdcard", bool = false) { return true; }
    bool begin(const char*, bool, bool, int, uint8_t) { return true; }
    void end() {}
    File open(const char* path, const char* mode = FILE_READ) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return File(opendir(path), path);
        const char* m = !strcmp(mode, FILE_WRITE) ? "wb" : !strcmp(mode, FILE_APPEND) ? "ab" : "rb";
        FILE* f = fopen(path, m);
        return f ? File(f, path) : File();
    }
    bool exists(const char* p) { struct stat st; return stat(p, &st) == 0; }
    bool remove(const char* p) { return ::remove(p) == 0; }
    bool rename(const char* a, const char* b) { return ::rename(a, b) == 0; }
    bool mkdir(const char* p) { return ::mkdir(p, 0777) == 0 || errno == EEXIST; }
    bool rmdir(const char* p) { return ::rmdir(p) == 0; }
    uint64_t totalBytes() { return 64ULL * 1024 * 1024; }
    uint64_t usedBytes() { return 1024 * 1024; }
    uint64_t cardSize() { return 64ULL * 1024 * 1024; }
    void setPins(int, int, int) {}
};

extern SDMMCFS SD_MMC;
