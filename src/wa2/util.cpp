// util.cpp — 基础工具实现
#include "util.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace wa2 {

uint16_t ReadU16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
uint32_t ReadU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
float ReadF32(const uint8_t* p) {
    uint32_t v = ReadU32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

std::vector<uint8_t> ReadFileAll(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        out.resize(size_t(size));
        size_t got = fread(out.data(), 1, out.size(), f);
        out.resize(got);
    }
    fclose(f);
    return out;
}

bool WriteFileAll(const std::string& path, const void* data, size_t size) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool FileExists(const std::string& path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

std::vector<std::string> ListDir(const std::string& dir, const std::string& suffix) {
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        if (suffix.empty() || (name.size() >= suffix.size() &&
            _stricmp(name.c_str() + name.size() - suffix.size(), suffix.c_str()) == 0))
            out.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string lower = ToLower(name);
        if (suffix.empty() || (lower.size() >= suffix.size() &&
            lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0))
            out.push_back(name);
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}

std::string PathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : char(c);
    });
    return r;
}

// ---------- 日志 ----------
static FILE* g_logFile = nullptr;

void LogSetFile(const std::string& path) {
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
    if (!path.empty()) g_logFile = fopen(path.c_str(), "wb");
}

void Log(LogLevel lv, const char* fmt, ...) {
    static const char* kTags[] = { "[D] ", "[I] ", "[W] ", "[E] " };
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stdout, "%s%s\n", kTags[int(lv)], buf);
    fflush(stdout);
    if (g_logFile) {
        fprintf(g_logFile, "%s%s\n", kTags[int(lv)], buf);
        fflush(g_logFile);
    }
}

void LogFlush() {
    fflush(stdout);
    if (g_logFile) fflush(g_logFile);
}

std::string Format(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace wa2
