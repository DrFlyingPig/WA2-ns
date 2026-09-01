// sjis.cpp — Shift-JIS(CP932)→ UTF-8
#include "sjis.h"
#include "sjis_table.h"

namespace wa2 { namespace sjis {

struct CkgalMapEntry {
    uint16_t raw;
    uint32_t unicode;
};
#include "ckgal_map.inc"

static uint32_t CkgalUnicode(uint16_t raw) {
    size_t lo = 0, hi = sizeof(kCkgalMap) / sizeof(kCkgalMap[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (kCkgalMap[mid].raw < raw) lo = mid + 1;
        else hi = mid;
    }
    return lo < sizeof(kCkgalMap) / sizeof(kCkgalMap[0]) && kCkgalMap[lo].raw == raw
        ? kCkgalMap[lo].unicode : 0;
}

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

std::string ToPatchFontUtf8(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(size * 2 + 16);
    for (size_t i = 0; i < size;) {
        const uint8_t b1 = data[i];
        if (b1 < 0x80) {
            out.push_back((char)b1);
            ++i;
            continue;
        }
        uint32_t raw = b1;
        if (((b1 >= 0x81 && b1 <= 0x9F) || (b1 >= 0xE0 && b1 <= 0xFE)) && i + 1 < size) {
            raw = (uint32_t(b1) << 8) | data[i + 1];
            i += 2;
        } else {
            ++i;
        }
        uint32_t unicode = CkgalUnicode((uint16_t)raw);
        if (unicode) {
            AppendUtf8(out, unicode);
        } else if (raw >= 0xB1 && raw <= 0xDD) {
            // 汉化字库沿用原版半角假名/符号区。
            AppendUtf8(out, 0xFF61u + (raw - 0xA1));
        } else if (raw >= 0x8140 && raw < 0x889F) {
            // 标点、拉丁、假名、希腊/西里尔区未被汉化组重排，直接走 CP932。
            int index = TableIndex((raw >> 8) & 0xFF, raw & 0xFF);
            uint32_t standard = index >= 0 ? kTable[index] : 0;
            AppendUtf8(out, standard ? standard : kPatchFontCodeBase + raw);
        } else {
            // 罕见且 OCR 未形成共识的中文字绝不猜测，保留原字形图集回退。
            AppendUtf8(out, kPatchFontCodeBase + raw);
        }
    }
    return out;
}

int PatchFontSlot(uint16_t rawCode) {
    // 汉化文本使用的主体区。0x7f 等原本未定义的槽也必须保留，补丁在这些位置放了中文字形。
    static const uint16_t ranges[][2] = {
        {0x0020,0x007D}, {0x00B1,0x00DD},
        {0x8140,0x81AC}, {0x81B8,0x81BF}, {0x81C8,0x81CE}, {0x81DA,0x81FC},
        {0x824F,0x8258}, {0x8260,0x8279}, {0x8281,0x829A}, {0x829F,0x82F1},
        {0x8340,0x8396}, {0x839F,0x83B6}, {0x83BF,0x83D6},
        {0x8440,0x8460}, {0x8470,0x8491}, {0x849F,0x84BE},
        {0x8740,0x875D}, {0x875F,0x8775}, {0x877E,0x878F}, {0x889F,0x88FC},
        {0x8940,0x89FC}, {0x8A40,0x8AFC}, {0x8B40,0x8BFC}, {0x8C40,0x8CFC},
        {0x8D40,0x8DFC}, {0x8E40,0x8EFC}, {0x8F40,0x8FFC}, {0x9040,0x90FC},
        {0x9140,0x91FC}, {0x9240,0x92FC}, {0x9340,0x93FC}, {0x9440,0x94FC},
        {0x9540,0x95FC}, {0x9640,0x96FC}, {0x9740,0x97FC}, {0x9840,0x9872},
        {0x989F,0x98FC}, {0x9940,0x99FC}, {0x9A40,0x9AFC}, {0x9B40,0x9BFC},
        {0x9C40,0x9CFC}, {0x9D40,0x9DFC}, {0x9E40,0x9EFC}, {0x9F40,0x9FFC},
        {0xE040,0xE0FC}, {0xE140,0xE1FC}, {0xE240,0xE2FC}, {0xE340,0xE3FC},
        {0xE440,0xE4FC}, {0xE540,0xE5FC}, {0xE640,0xE6FC}, {0xE740,0xE7FC},
        {0xE840,0xE8FC}, {0xE940,0xE9FC}, {0xEA40,0xEAA4},
    };
    int slot = 0;
    for (const auto& range : ranges) {
        if (rawCode >= range[0] && rawCode <= range[1])
            return slot + int(rawCode - range[0]);
        slot += int(range[1] - range[0]) + 1;
    }
    return -1;
}

}} // namespace wa2::sjis
