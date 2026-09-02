// sav.h — 存档与配置
#pragma once

#include "wa2.h"
#include "util.h"
#include "scene.h"
#include <array>

namespace wa2 {

constexpr int kSysFlagCount = 4096;

// 用户设置
struct Config {
    int textSpeed = 2;        // 0-3,越大越快
    int autoSpeed = 2;        // 旧 config.bin 兼容字段
    int autoDelayFrames = 137;// 原版 auto_max,60-600 帧
    int masterVolume = 195;   // 原版 all_vol,0-255(原值 256 在混音层饱和)
    int bgmVolume = 134;      // 0-255
    int seVolume = 195;
    int voiceVolume = 255;
    bool skipUnread = true;   // 原版 msg_cut_optin=1:允许跳过全部文本
    bool fastWait = false;    // 原版 wait:确认键快速结束演出等待
    bool confirmDefaultYes = true; // 原版 yes_no:确认框默认选择“是”
    bool pageVoice = false;   // 翻页/结束本页时停止当前语音
    bool eroVoice = false;    // H 场景只保留主要角色语音
    bool longPressSkip = false;
    int windowAlpha = 207;    // 普通对话框
    int cgWindowAlpha = 134;  // CG 画面上的对话框
    int novelWindowAlpha = 134;
    std::array<uint8_t, 10> charVoice{{1,1,1,1,1,1,1,1,1,1}};

    void Save(ByteBuf& out) const;
    bool Load(ByteReader& in);
    void SetDefaults() { *this = Config{}; }
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
