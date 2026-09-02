// util.h — 基础工具:字节读取、文件系统、日志、编码
#pragma once

#include "wa2.h"
#include <cstring>

namespace wa2 {

// ---------- 小端读取 ----------
uint16_t ReadU16(const uint8_t* p);
uint32_t ReadU32(const uint8_t* p);
float    ReadF32(const uint8_t* p);

// ---------- 文件系统 ----------
// 以二进制读入整个文件;失败返回空 vector。跨平台(sdmc:/romfs:/盘符路径均可,由 stdio 处理)
std::vector<uint8_t> ReadFileAll(const std::string& path);
// 只读取文件的一段，避免从数百 MB/GB 的资源包取单个条目时把整包载入内存。
std::vector<uint8_t> ReadFileRange(const std::string& path, uint64_t offset, size_t size);
bool WriteFileAll(const std::string& path, const void* data, size_t size);
bool FileExists(const std::string& path);
// 列出目录下匹配后缀的文件名(仅文件名,不含路径);后缀为空则返回全部
std::vector<std::string> ListDir(const std::string& dir, const std::string& suffix = "");
// 路径拼接(处理斜杠)
std::string PathJoin(const std::string& a, const std::string& b);
std::string ToLower(const std::string& s);

// ---------- 日志 ----------
enum class LogLevel { Debug, Info, Warn, Error };
void LogSetFile(const std::string& path);   // 传空则只写 stdout
void Log(LogLevel lv, const char* fmt, ...);
void LogFlush();

// ---------- 字符串 ----------
std::string Format(const char* fmt, ...);
std::vector<std::string> Split(const std::string& s, char sep);
std::string Trim(const std::string& s);

// 保存数据序列化用的简单追加式 buffer
class ByteBuf {
public:
    void U8(uint8_t v)   { b_.push_back(v); }
    void U32(uint32_t v) { for (int i = 0; i < 4; i++) b_.push_back(uint8_t(v >> (i * 8))); }
    void I32(int32_t v)  { U32(uint32_t(v)); }
    void F32(float v)    { uint32_t bits = 0; std::memcpy(&bits, &v, sizeof(bits)); U32(bits); }
    void Str(const std::string& s) { U32(uint32_t(s.size())); b_.insert(b_.end(), s.begin(), s.end()); }
    void Bytes(const void* p, size_t n) {
        U32(uint32_t(n));
        if (n) b_.insert(b_.end(), (const uint8_t*)p, (const uint8_t*)p + n);
    }
    const std::vector<uint8_t>& data() const { return b_; }
private:
    std::vector<uint8_t> b_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}
    bool   Ok() const { return ok_; }
    size_t Pos() const { return pos_; }
    size_t Remaining() const { return pos_ <= n_ ? n_ - pos_ : 0; }
    void   Seek(size_t pos) { pos_ = pos; ok_ = pos_ <= n_; }
    uint8_t  U8()  { if (!Need(1)) return 0; return p_[pos_++]; }
    uint32_t U32() { if (!Need(4)) return 0; uint32_t v = ReadU32(p_ + pos_); pos_ += 4; return v; }
    int32_t  I32() { return (int32_t)U32(); }
    float    F32() { if (!Need(4)) return 0; float v = ReadF32(p_ + pos_); pos_ += 4; return v; }
    std::string Str() { uint32_t n = U32(); if (!Need(n)) return {}; std::string s((const char*)p_ + pos_, n); pos_ += n; return s; }
    std::vector<uint8_t> Bytes() { uint32_t n = U32(); if (!Need(n)) return {}; std::vector<uint8_t> v(p_ + pos_, p_ + pos_ + n); pos_ += n; return v; }
private:
    bool Need(size_t k) {
        if (pos_ > n_ || k > n_ - pos_) { ok_ = false; return false; }
        return true;
    }
    const uint8_t* p_; size_t n_; size_t pos_ = 0; bool ok_ = true;
};

} // namespace wa2
