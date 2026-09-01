// archive.cpp — PACK / LAC 归档 + LZSS
#include "archive.h"
#include "util.h"

#include <cstring>

namespace wa2 {

std::unordered_map<std::string, ArchiveEntry>& Archive::Index() {
    static std::unordered_map<std::string, ArchiveEntry> idx;
    return idx;
}

bool Archive::Has(const std::string& lowerName) {
    return Index().count(lowerName) != 0;
}

std::vector<uint8_t> Archive::LoadFile(const std::string& lowerName) {
    auto& idx = Index();
    auto it = idx.find(lowerName);
    if (it == idx.end()) return {};
    Archive a;
    a.path_ = it->second.pkgPath;
    return a.Read(it->second);
}

// ---------------- LZSS ----------------
std::vector<uint8_t> LzssDecompress(const uint8_t* data, size_t size) {
    if (size < 8) return {};
    uint32_t inLim = ReadU32(data);
    uint32_t outLim = ReadU32(data + 4);
    if (inLim + 8 > size) inLim = uint32_t(size - 8);

    std::vector<uint8_t> out(outLim, 0);
    std::vector<uint8_t> ring(0x1000, 0x20);
    uint32_t ringW = 0xFEE;

    size_t ip = 8;          // 输入指针(跳过两个长度头)
    size_t op = 0;          // 输出指针
    while (ip < 8 + inLim && op < outLim) {
        uint8_t flags = data[ip++];
        for (int bit = 0; bit < 8 && ip < 8 + inLim && op < outLim; bit++) {
            if (flags & 1) {
                // 原文字节
                uint8_t b = data[ip++];
                ring[ringW++ & 0xFFF] = b;
                out[op++] = b;
            } else {
                // 回溯引用
                if (ip + 1 >= 8 + inLim) break;
                uint8_t b1 = data[ip++];
                uint8_t b2 = data[ip++];
                uint32_t ringR = uint32_t(b1) | (uint32_t(b2 & 0xF0) << 4);
                uint32_t len = uint32_t(b2 & 0x0F) + 3;
                while (len-- > 0 && op < outLim) {
                    uint8_t b = ring[ringR++ & 0xFFF];
                    ring[ringW++ & 0xFFF] = b;
                    out[op++] = b;
                }
            }
            flags >>= 1;
        }
    }
    out.resize(op);
    return out;
}

// ---------------- Archive ----------------
bool Archive::Open(const std::string& path) {
    std::vector<uint8_t> head = ReadFileRange(path, 0, 16);
    if (head.size() < 8) return false;

    uint32_t magic = ReadU32(head.data());
    size_t base = 0, entrySize = 0, count = 0;

    if (magic == 0x5041434B || magic == 0x4B434150) {
        // PACK:两种字节序的 magic 都接受
        if (head.size() < 16) return false;
        count = ReadU32(head.data() + 12);
        base = 16;
        entrySize = 44;
        if (count > (SIZE_MAX - base) / entrySize) return false;
        std::vector<uint8_t> buf = ReadFileRange(path, 0, base + count * entrySize);
        if (buf.size() != base + count * entrySize) return false;
        for (size_t i = 0; i < count; i++) {
            const uint8_t* p = buf.data() + base + i * entrySize;
            ArchiveEntry e;
            e.compressed = ReadU32(p) != 0;
            // name: 24 字节 Shift-JIS,补零截断(仅 ASCII 名,直接取字节)
            char name[25];
            memcpy(name, p + 4, 24);
            name[24] = 0;
            e.name = ToLower(std::string(name));
            e.offset = ReadU32(p + 36);
            e.size = ReadU32(p + 40);
            e.pkgPath = path;
            if (!e.name.empty() && e.size > 0)
                Index()[e.name] = e;
        }
        Log(LogLevel::Info, "archive: PACK %s -> %u entries", path.c_str(), uint32_t(count));
        return true;
    }

    if (magic == 0x0043414C) {
        // LAC:"LAC\0",名字逐字节取反
        count = ReadU32(head.data() + 4);
        base = 8;
        entrySize = 40;
        if (count > (SIZE_MAX - base) / entrySize) return false;
        std::vector<uint8_t> buf = ReadFileRange(path, 0, base + count * entrySize);
        if (buf.size() != base + count * entrySize) return false;
        for (size_t i = 0; i < count; i++) {
            const uint8_t* p = buf.data() + base + i * entrySize;
            char name[33];
            for (int j = 0; j < 32; j++) {
                uint8_t c = p[j];
                name[j] = c ? char(uint8_t(~c)) : 0;
            }
            name[32] = 0;
            ArchiveEntry e;
            e.name = ToLower(std::string(name));
            e.size = ReadU32(p + 32);
            e.offset = ReadU32(p + 36);
            e.compressed = false;
            e.pkgPath = path;
            if (!e.name.empty() && e.size > 0)
                Index()[e.name] = e;
        }
        Log(LogLevel::Info, "archive: LAC %s -> %u entries", path.c_str(), uint32_t(count));
        return true;
    }

    return false;
}

std::vector<uint8_t> Archive::Read(const ArchiveEntry& e) {
    std::vector<uint8_t> raw = ReadFileRange(e.pkgPath, e.offset, e.size);
    if (raw.size() != e.size) {
        Log(LogLevel::Error, "archive: entry %s out of range in %s", e.name.c_str(), e.pkgPath.c_str());
        return {};
    }
    if (!e.compressed)
        return raw;
    return LzssDecompress(raw.data(), raw.size());
}

} // namespace wa2
