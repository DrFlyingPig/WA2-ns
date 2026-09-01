// sjis.h — Shift-JIS(CP932)→ UTF-8 转换
// 映射表由 tools/gen_sjis.py 生成到 sjis_table.cpp
#pragma once

#include "wa2.h"

namespace wa2 { namespace sjis {

// 整段转换
std::string ToUtf8(const uint8_t* data, size_t size);
inline std::string ToUtf8(const std::vector<uint8_t>& v) { return ToUtf8(v.data(), v.size()); }

// 单个字符转换:返回消耗的源字节数,输出追加到 out
size_t OneToUtf8(const uint8_t* src, size_t avail, std::string& out);

// 汉化补丁的双字节编码槽直接对应 fon.pak 字形。ASCII 保持原样，其余编码
// 放入补充私用区，渲染层据此查字形图集，避免误当成日文 Unicode。
constexpr uint32_t kPatchFontCodeBase = 0xF0000;
std::string ToPatchFontUtf8(const uint8_t* data, size_t size);
// wa2-godot font.map / 零售版 fnt/fon 图集的稳定槽位；找不到返回 -1。
int PatchFontSlot(uint16_t rawCode);

// UCS2 → UTF-8
void AppendUtf8(std::string& out, uint32_t cp);

}} // namespace wa2::sjis
