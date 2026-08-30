// wa2.h — 公共类型与常量
//
// WA2-ns:《白色相簿2》PC 版数据格式的 Switch 原生引擎实现。
// 虚拟分辨率 1280x720,帧时间 1/60s,脚本节拍 1/30s。
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <unordered_map>
#include <functional>

namespace wa2 {

constexpr int   kVirtualW = 1280;
constexpr int   kVirtualH = 720;
constexpr float kFrameTime = 1.0f / 60.0f;      // 动画/渲染帧
constexpr float kScriptTick = 1.0f / 30.0f;     // 脚本节拍
constexpr int   kMaxChars = 10;                 // 立绘槽位(与原版 CharPos 一致)
constexpr int   kMaxGameFlags = 2048;           // 剧本旗标(GLOBAL_VAR 索引)
constexpr int   kMaxLocalVars = 26;             // LOCAL_VAR 整型/浮点槽
constexpr int   kSaveSlots = 6;

// 立绘槽位相对屏幕中心的 X 偏移(原版 Wa2Def.CharPos)
constexpr int kCharPos[kMaxChars] = {
    -288, 0, 288, -384, 384, -480, 480, -480, -160, 160
};
// 立绘绘制顺序(远→近,原版 Wa2Def.CharOrder,截去最后一项)
constexpr int kCharOrder[kMaxChars] = { 5, 7, 3, 0, 8, 1, 9, 2, 4, 6 };

// 立绘 id → 素材前缀(原版 Wa2Def.CharDict;demo 素材使用相同前缀)
const std::map<int, std::string>& CharDict();

} // namespace wa2
