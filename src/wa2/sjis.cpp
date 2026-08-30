// sjis.cpp — Shift-JIS(CP932)→ UTF-8
#include "sjis.h"
#include "sjis_table.h"

namespace wa2 { namespace sjis {

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

size_t OneToUtf8(const uint8_t* src, size_t avail, std::string& out) {
    uint8_t b1 = src[0];
    // ASCII 直通
    if (b1 < 0x80) {
        out.push_back(char(b1));
        return 1;
    }
    // 半角假名 0xA1-0xDF
    if (b1 >= 0xA1 && b1 <= 0xDF) {
        AppendUtf8(out, 0xFF61u + (b1 - 0xA1));
        return 1;
    }
    // 双字节
    if ((b1 >= 0x81 && b1 <= 0x9F) || (b1 >= 0xE0 && b1 <= 0xFC)) {
        if (avail < 2) {
            out.push_back('?');
            return 1;
        }
        uint8_t b2 = src[1];
        int idx = TableIndex(b1, b2);
        uint32_t cp = idx >= 0 ? kTable[idx] : 0;
        if (cp == 0) {
            // 未定义码位:CP932 的 NEC/IBM 扩展区已含在表内;剩余按 '?' 处理
            out.push_back('?');
            return 2;
        }
        AppendUtf8(out, cp);
        return 2;
    }
    // 0x80 / 0xA0 / 0xFD-0xFF:非法
    out.push_back('?');
    return 1;
}

std::string ToUtf8(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(size * 3 / 2 + 16);
    size_t i = 0;
    while (i < size) {
        i += OneToUtf8(data + i, size - i, out);
    }
    return out;
}

}} // namespace wa2::sjis
