// sjis_table.h — 由 tools/gen_sjis.py 自动生成,勿手改
#pragma once
#include <cstdint>

namespace wa2 { namespace sjis {

extern const uint16_t kTable[];
constexpr int kTableCols = 189;
inline int TableIndex(int lead, int trail) {
    int row = (lead >= 0x81 && lead <= 0x9F) ? lead - 0x81
            : (lead >= 0xE0 && lead <= 0xEF) ? 0x1F + (lead - 0xE0) : -1;
    if (row < 0 || trail < 0x40 || trail > 0xFC) return -1;
    return row * kTableCols + (trail - 0x40);
}

}} // namespace wa2::sjis
