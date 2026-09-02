// character_state.h — 立绘“期望列表 -> 画面槽位”提交与淡入淡出
//
// 脚本的 CW/CRW 只修改期望列表；C/CR/BC 才把整张列表一次性提交到
// 画面。这里保持纯数据逻辑，既可供 SDL 引擎使用，也可做无头回归测试。
#pragma once

#include "scene.h"

#include <algorithm>
#include <cmath>

namespace wa2 {

// 与参考项目的 Chars[pos] 一致：数组下标就是立绘位置，而不是分配顺序。
struct CharacterVisualState {
    bool show = false;
    int id = -1;
    int no = 0;
    int pos = 0;
    float alpha = 0.0f;
    float targetAlpha = 0.0f;
    float fadePerSec = 0.0f;
};

inline void ResetCharacterVisual(CharacterVisualState& visual, int pos) {
    visual = {};
    visual.pos = pos;
}

inline void ResetCharacterVisuals(CharacterVisualState (&visuals)[kMaxChars]) {
    for (int pos = 0; pos < kMaxChars; ++pos)
        ResetCharacterVisual(visuals[pos], pos);
}

// 把 SceneState 中排队完成的期望角色列表提交到固定位置槽。
// frames==0 与参考实现 SetCurTexture/Hide 一样立即完成；大于 0 时统一过渡。
inline void CommitCharacterVisuals(const SceneState& scene,
                                   CharacterVisualState (&visuals)[kMaxChars],
                                   int frames) {
    const int safeFrames = std::max(0, frames);
    const float seconds = safeFrames * kFrameTime;
    const float speed = seconds > 0.0f ? 1.0f / seconds : 0.0f;
    bool desiredPositions[kMaxChars] = {};

    for (const auto& desired : scene.chars) {
        if (!desired.show || desired.pos < 0 || desired.pos >= kMaxChars) continue;
        const int pos = desired.pos;
        desiredPositions[pos] = true;
        CharacterVisualState& visual = visuals[pos];
        const bool sameImage = visual.show && visual.id == desired.id &&
                               visual.no == desired.no;

        visual.show = true;
        visual.id = desired.id;
        visual.no = desired.no;
        visual.pos = pos;
        visual.targetAlpha = 1.0f;
        if (seconds <= 0.0f) {
            visual.alpha = 1.0f;
            visual.fadePerSec = 0.0f;
        } else {
            // 新图或同槽换差分从透明开始；同一张图只续接未完成的过渡。
            if (!sameImage) visual.alpha = 0.0f;
            visual.alpha = std::clamp(visual.alpha, 0.0f, 1.0f);
            visual.fadePerSec = std::abs(visual.alpha - visual.targetAlpha) > 0.0001f
                ? speed : 0.0f;
        }
    }

    for (int pos = 0; pos < kMaxChars; ++pos) {
        if (desiredPositions[pos]) continue;
        CharacterVisualState& visual = visuals[pos];
        if (!visual.show) continue;
        if (seconds <= 0.0f || visual.alpha <= 0.0001f) {
            ResetCharacterVisual(visual, pos);
        } else {
            visual.targetAlpha = 0.0f;
            visual.fadePerSec = speed;
        }
    }
}

inline void AdvanceCharacterVisual(CharacterVisualState& visual, float dt) {
    if (!visual.show || visual.fadePerSec <= 0.0f || dt <= 0.0f) return;
    if (visual.alpha < visual.targetAlpha) {
        visual.alpha = std::min(visual.targetAlpha,
                                visual.alpha + visual.fadePerSec * dt);
    } else if (visual.alpha > visual.targetAlpha) {
        visual.alpha = std::max(visual.targetAlpha,
                                visual.alpha - visual.fadePerSec * dt);
    }
    if (std::abs(visual.alpha - visual.targetAlpha) <= 0.0001f) {
        visual.alpha = visual.targetAlpha;
        visual.fadePerSec = 0.0f;
        if (visual.targetAlpha <= 0.0001f)
            ResetCharacterVisual(visual, visual.pos);
    }
}

inline void AdvanceCharacterVisuals(CharacterVisualState (&visuals)[kMaxChars],
                                    float dt) {
    for (auto& visual : visuals) AdvanceCharacterVisual(visual, dt);
}

} // namespace wa2
