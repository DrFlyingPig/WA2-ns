// sav.h — 存档与配置
#pragma once

#include "wa2.h"
#include "util.h"
#include "scene.h"

namespace wa2 {

constexpr int kSysFlagCount = 4096;

// 用户设置
struct Config {
    int textSpeed = 2;        // 0-3,越大越快
    int autoSpeed = 2;        // 0-3
    int bgmVolume = 200;      // 0-255
    int seVolume = 220;
    int voiceVolume = 255;
    bool skipUnread = false;  // 强制跳过未读

    void Save(ByteBuf& out) const;
    bool Load(ByteReader& in);
};

// 完整存档:脚本栈由引擎序列化,这里存放公共部分
struct SaveMeta {
    uint64_t timestamp = 0;
    std::string chapter;      // 脚本名
    std::string preview;      // 最近一句文本
};

struct SaveData {
    SaveMeta meta;
    std::vector<int> gameFlags;
    std::vector<uint8_t> sysFlags;   // 0/1 数组,kSysFlagCount
    // 脚本栈的字节块由引擎追加(引擎私有格式,直接透传)
    std::vector<uint8_t> engineBlock;

    void Reset();
    void Save(ByteBuf& out) const;
    bool Load(ByteReader& in);
};

} // namespace wa2
