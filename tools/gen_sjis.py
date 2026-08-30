#!/usr/bin/env python3
"""gen_sjis.py — 生成 Shift-JIS(CP932)→ Unicode 映射表

输出 src/wa2/sjis_table.cpp(.h):
  表按 (首字节行, 尾字节) 线性排布:
    row = lead-0x81 (0x81..0x9F) 或 0x1F + lead-0xE0 (0xE0..0xEF),共 47 行
    col = trail-0x40 (0x40..0xFC),共 189 列(0x7F 列恒为 0)
    下标 = row*189 + col
  单字节半角假名区(0xA1-0xDF)不入表,由公式映射 0xFF61+ (b-0xA1)。
"""
import os

OUT_CPP = os.path.join(os.path.dirname(__file__), "..", "src", "wa2", "sjis_table.cpp")
OUT_H = os.path.join(os.path.dirname(__file__), "..", "src", "wa2", "sjis_table.h")

LEADS = list(range(0x81, 0xA0)) + list(range(0xE0, 0xF0))
TRAILS = list(range(0x40, 0xFD))
N_COLS = len(TRAILS)          # 189


def build():
    table = [0] * (len(LEADS) * N_COLS)
    for r, lead in enumerate(LEADS):
        for c, trail in enumerate(TRAILS):
            if trail == 0x7F:
                continue
            try:
                table[r * N_COLS + c] = ord(bytes([lead, trail]).decode("cp932")[0])
            except UnicodeDecodeError:
                pass
    return table


def main():
    table = build()
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("// sjis_table.h — 由 tools/gen_sjis.py 自动生成,勿手改\n")
        f.write("#pragma once\n#include <cstdint>\n\nnamespace wa2 { namespace sjis {\n\n")
        f.write("extern const uint16_t kTable[];\n")
        f.write(f"constexpr int kTableCols = {N_COLS};\n")
        f.write("inline int TableIndex(int lead, int trail) {\n")
        f.write("    int row = (lead >= 0x81 && lead <= 0x9F) ? lead - 0x81\n")
        f.write("            : (lead >= 0xE0 && lead <= 0xEF) ? 0x1F + (lead - 0xE0) : -1;\n")
        f.write("    if (row < 0 || trail < 0x40 || trail > 0xFC) return -1;\n")
        f.write("    return row * kTableCols + (trail - 0x40);\n")
        f.write("}\n\n}} // namespace wa2::sjis\n")
    with open(OUT_CPP, "w", encoding="utf-8") as f:
        f.write("// sjis_table.cpp — 由 tools/gen_sjis.py 自动生成,勿手改\n")
        f.write('#include "sjis_table.h"\n\nnamespace wa2 { namespace sjis {\n\n')
        f.write(f"const uint16_t kTable[{len(table)}] = {{\n")
        for i in range(0, len(table), 16):
            f.write("    " + ",".join(f"0x{v:04X}" for v in table[i:i+16]) + ",\n")
        f.write("};\n\n}} // namespace wa2::sjis\n")
    defined = sum(1 for v in table if v)
    print(f"written: {len(table)} entries ({defined} defined)")


if __name__ == "__main__":
    main()
