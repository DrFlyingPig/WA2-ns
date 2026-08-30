// archive.h — PC 版 WA2 归档读取(PACK / LAC)+ LZSS 解压
//
// 格式要点(逆向自社区资料,详见 docs/formats.md):
//   PACK: 头 16 字节 [magic u32][8 字节保留][nentry u32],
//         条目表位于 16 + i*44:[crypted u32][name 24B Shift-JIS][保留 8B][offset u32][size u32]
//   LAC : 头 8 字节 [magic "LAC\0"][nentry u32],
//         条目位于 8 + i*40:[name 32B 按位取反混淆, Shift-JIS][size u32][offset u32]
//   crypted != 0 的条目 = LZSS 压缩:头 [压缩后 u32][解压后 u32] + 数据,
//         0x1000 环形缓冲初始填 0x20,写指针 0xFEE,标志字节 LSB 在前,
//         标志位 1 = 原文字节;0 = 回溯引用 [pos低8位|(高4位<<4), len=(低4位)+3]
#pragma once

#include "wa2.h"

namespace wa2 {

struct ArchiveEntry {
    std::string name;    // 小写文件名
    uint32_t    offset = 0;
    uint32_t    size = 0;
    bool        compressed = false;
    std::string pkgPath; // 所属归档文件路径
};

class Archive {
public:
    // 打开归档并把所有条目登记进 index;失败返回 false(可能只是格式不符)
    bool Open(const std::string& path);

    // 读取条目数据(自动解压);找不到返回空
    std::vector<uint8_t> Read(const ArchiveEntry& e);

    // 归档登记表(全局共用):key = 小写文件名
    static std::unordered_map<std::string, ArchiveEntry>& Index();
    // 从登记表读文件(自动解压);找不到返回空 vector
    static std::vector<uint8_t> LoadFile(const std::string& lowerName);
    // 登记表是否包含某文件
    static bool Has(const std::string& lowerName);

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

// LZSS 解压(PC 版引擎使用的经典变体)
std::vector<uint8_t> LzssDecompress(const uint8_t* data, size_t size);

} // namespace wa2
