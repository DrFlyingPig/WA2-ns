// funcs.h — 游戏函数表(0x00-0xEE)与引擎宿主接口
//
// .bnr 字节码通过 4 号指令(函数号)驱动整个游戏:文本、画面、音频、选项、流程。
// Host 是引擎宿主的抽象接口:SDL 版 Engine 和无头测试 Harness 各自实现。
// 函数语义复刻自社区逆向资料(见 reference/),未实现的功能记录日志后放行,
// 不阻塞剧情推进。
#pragma once

#include "wa2.h"
#include "script.h"

namespace wa2 {

// 玩家主动推进定时对白时，第一次确认只完成打字机；只有文字已经完整、
// 正在等待下一次确认时，第二次确认才解除 0xC4 时间轴等待。
inline bool ShouldReleaseC4ForAdvance(bool clicked, bool c4Timer,
                                      bool textBusy, bool waitClick) {
    return clicked && c4Timer && !textBusy && waitClick;
}

// 参考实现会在最后一段文字的 WAIT_CLICK 收到确认后，于同一次输入调用中
// 继续解析脚本；仍有 \k 分段时则只切到下一段，不应越过该段的打字机。
inline bool ShouldSynchronouslyContinueAfterConfirm(bool clicked,
                                                     bool textBusy,
                                                     bool waitClick,
                                                     bool hasNextSegment) {
    return clicked && !textBusy && waitClick && !hasNextSegment;
}

class Host {
public:
    virtual ~Host() = default;

    // --- 脚本栈/流程 ---
    // 注意:SLoad/SCall 发生在 Tick 执行中,实现方不得同步销毁正在运行的脚本,
    // 应把旧脚本移入"墓地",在 ApplyPending()(每拍结束后由驱动层调用)再释放。
    virtual void SLoadScript(const std::string& name, int point) = 0;
    virtual void SCallScript(const std::string& name, int point) = 0;
    virtual void CallPoint(int point) = 0;
    virtual void GoTitle() = 0;
    virtual void ApplyPending() {}
    virtual void PushInt(int v) = 0;
    virtual void PushFloat(float v) = 0;

    // --- 旗标 ---
    virtual int  ReadSysFlag(int idx) = 0;
    virtual void WriteSysFlag(int idx, int v) = 0;
    virtual int  ReadGameFlag(int idx) = 0;
    virtual void WriteGameFlag(int idx, int v) = 0;

    // --- 文本 ---
    virtual void ShowMessage(const std::string& text, int msgIdx, int mode, bool append) = 0;
    virtual void EndMessage() = 0;
    virtual void SetName(const std::string& name) = 0;
    virtual void WaitClick() = 0;
    virtual void HideWindow(int fadeFrames) = 0;
    virtual void NovelHide(int frames) {}
    virtual void NovelShow(int frames) {}
    virtual void SetNovelMode(bool v) {}
    virtual void SetDemoMode(bool v) {}
    virtual void SetSkipDisable(bool v) {}
    virtual void StopSkip() {}

    // --- 画面 ---
    // 统一画面切换:id=-1 保持当前纹理;type 0=背景 1=CG 2=H;efc>=128 为掩码过渡
    virtual void RenderImage(int id, int efc, bool keepChar, int type,
                             int frame, int offset, int x, int y, float sx, float sy) = 0;
    virtual void AddChar(int id, int no, int pos) = 0;
    virtual void UpdateChar(int frames) = 0;
    virtual void RemoveChar(int id) = 0;
    virtual void BgMove(int dx, int dy, int frames) {}
    virtual void WaitBgMove() {}
    virtual void StopBgMove() {}
    virtual void ColorFade(int r, int g, int b, int frames) = 0;
    // F 与 FB 的动画方向不同：F 从指定色调回到中性，FB 从当前色调过渡到指定色调。
    virtual void ColorFadeFrom(int r, int g, int b, int frames) { ColorFade(r, g, b, frames); }
    virtual void SetFBColor(int r, int g, int b) {}
    virtual void Shake(int type, int power, int frames) {}
    virtual void ShowCalender(int y, int m, int d, int dow) = 0;
    virtual void PlayMovie(int movieId, int flagIdx) {}
    virtual int  TimeMode() const { return 0; }
    virtual void SetTimeMode(int v) {}
    virtual void SetEffectMode(const std::string& file) {}
    virtual void SetWeather(int flag, int speedX, int speedY, int turbulence,
                            int count, int flag2, int index) {}
    virtual void ChangeWeather(int speedX, int speedY, int count,
                               int turbulence, int index) {}
    virtual void ResetWeather() {}
    virtual bool EroMode() const { return false; }
    virtual void SetEroMode(bool v) {}
    // 0=正常剧情，1..24=特别模式中的场景编号。不能压缩成 bool，
    // 原版 9999 调度脚本依赖完整编号选择不同回放。
    virtual int ReplayMode() const { return 0; }
    virtual bool CanSkip() const { return false; }
    virtual bool Clicked() const { return false; }

    // --- 选项 ---
    virtual void AddSelectItem(const std::string& text, int v1, int v2, int v3) = 0;
    virtual void ShowSelect() = 0;   // 选中后宿主负责 PushInt

    // --- 音频 ---
    virtual void SetVoiceLabel(int label) {}         // VI
    virtual int  CurrentVoiceLabel() const { return 0; }
    virtual void PlayBgm(int id, bool loop, int vol) = 0;
    virtual void StopBgm(int fadeFrames) = 0;
    virtual void PlayVoice(int label, int id, int chr, int volume,
                           bool loop, int channel) = 0;
    virtual void WaitVoice(int ch) = 0;
    virtual void StopVoice(int fadeFrames, int ch) = 0;
    virtual void SetVoiceVolume(int ch, int vol, int frames) {}
    virtual void PlaySe(int ch, int id, bool loop, int fadeInFrames, int vol) = 0;
    virtual void StopSe(int ch, int fadeFrames) = 0;
    virtual void SetSeVolume(int ch, int vol, int frames) {}
    virtual void WaitSe(int ch) = 0;

    // --- 定时 ---
    virtual void WaitMs(float ms) = 0;
    virtual void StartTimer() = 0;
    virtual int  ElapsedTimerMs() = 0;
    // 0xC4 的参数是从最近一次 0xC3 起算的绝对时间点，不是新的相对延时。
    virtual void WaitUntilTimerMs(float targetMs) {
        const float remainingMs = targetMs - (float)ElapsedTimerMs();
        if (remainingMs > 0.0f) WaitMs(remainingMs);
    }

    // --- Bmp 自由图层(特效/控件用) ---
    virtual void LoadBmp(int id, const std::string& path, int z) {}
    virtual void ReleaseBmp(int id) {}
    virtual void SetBmpParam(int id, int mode, int alpha, int frames) {}
    virtual void SetBmpMove(int id, int x, int y) {}
    virtual void SetBmpZoom(int id, int x, int y, float sx, float sy) {}
};

class Funcs {
public:
    // 返回 true = 继续执行下一条指令;false = 让出本节拍
    static bool Call(Host& host, Script& sc, uint32_t idx, std::vector<Var>& args);
};

} // namespace wa2
