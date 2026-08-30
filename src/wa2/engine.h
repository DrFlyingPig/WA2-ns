// engine.h — 引擎宿主(Host 实现):状态机/渲染集成/输入/存档/UI
#pragma once

#include "wa2.h"
#include "funcs.h"
#include "res.h"
#include "gfx.h"
#include "audio.h"
#include "sav.h"

namespace wa2 {

class Engine : public Host {
public:
    bool Init(const std::string& dataDirOverride);
    void Run();
    void Shutdown();

    // ---- Host 实现 ----
    void SLoadScript(const std::string& name, int point) override;
    void SCallScript(const std::string& name, int point) override;
    void CallPoint(int point) override;
    void GoTitle() override;
    void ApplyPending() override;
    void PushInt(int v) override;
    void PushFloat(float v) override;

    int  ReadSysFlag(int idx) override;
    void WriteSysFlag(int idx, int v) override;
    int  ReadGameFlag(int idx) override;
    void WriteGameFlag(int idx, int v) override;

    void ShowMessage(const std::string& text, int msgIdx, int mode, bool append) override;
    void EndMessage() override;
    void SetName(const std::string& name) override;
    void WaitClick() override;
    void HideWindow(int fadeFrames) override;
    void SetNovelMode(bool v) override;
    void SetDemoMode(bool v) override;
    void SetSkipDisable(bool v) override;
    void StopSkip() override;

    void RenderImage(int id, int efc, bool keepChar, int type, int frame,
                     int offset, int x, int y, float sx, float sy) override;
    void AddChar(int id, int no, int pos) override;
    void UpdateChar(int frames) override;
    void RemoveChar(int pos) override;
    void BgMove(int dx, int dy, int frames) override;
    void ColorFade(int r, int g, int b, int frames) override;
    void SetFBColor(int r, int g, int b) override;
    void Shake(int type, int power, int frames) override;
    void ShowCalender(int y, int m, int d, int dow) override;
    void PlayMovie(int movieId, int flagIdx) override;
    int  TimeMode() const override;
    void SetTimeMode(int v) override;
    void SetEffectMode(const std::string& file) override;
    void SetEroMode(bool v) override;
    bool ReplayMode() const override { return false; }
    bool CanSkip() const override;
    bool Clicked() const override;

    void AddSelectItem(const std::string& text, int v1, int v2, int v3) override;
    void ShowSelect() override;

    void PlayBgm(int id, bool loop, int vol) override;
    void StopBgm(int fadeFrames) override;
    void SetVoiceLabel(int label) override;
    int  CurrentVoiceLabel() const override;
    void PlayVoice(int label, int id, int ch, bool loop, int track) override;
    void WaitVoice(int ch) override;
    void StopVoice(int fadeMs, int ch) override;
    void SetVoiceVolume(int ch, int vol, int frames) override;
    void PlaySe(int ch, int id, bool loop, int fadeInMs, int vol) override;
    void StopSe(int ch, int fadeMs) override;
    void SetSeVolume(int ch, int vol, int frames) override;
    void WaitSe(int ch) override;

    void WaitMs(float ms) override;
    void StartTimer() override;
    int  ElapsedTimerMs() override;

    void LoadBmp(int id, const std::string& path, int z) override;
    void ReleaseBmp(int id) override;
    void SetBmpParam(int id, int mode, int alpha, int frames) override;

private:
    enum class State { Logo, Title, Game, Quit };

    // ---- 等待门(CheckScript 语义)----
    struct Wait {
        bool textBusy = false;      // 打字机未完成
        bool waitClick = false;     // 等点击
        bool timer = false;         // WaitMs/WaitVoice/WaitSe
        float timerUntil = 0;
        bool animBusy = false;      // 过渡/立绘/FB 动画进行中
        float animUntil = 0;
        bool selectVisible = false;
        bool calender = false;
        bool menu = false;          // 任意 UI 打开
        bool Blocking() const {
            return textBusy || waitClick || timer || animBusy ||
                   selectVisible || calender || menu;
        }
        void Clear() { *this = Wait(); }
    } wait_;

    // ---- 文本窗 ----
    struct Adv {
        bool visible = false;
        bool hide = false;          // WR 隐藏
        std::string name, text;     // text 已按 \k 切段
        std::vector<std::string> segments;
        int seg = 0;                // 当前段
        int shown = 0;              // 当前段已显示字符数
        uint32_t lastCharMs = 0;
        bool WaitForClick() const { return shown >= (int)segments[seg].size(); }
    } adv_;

    // ---- 画面状态 ----
    struct BgDraw {
        std::string path;    // 当前背景
        int id = -1;
        float x = 0, y = 0;
        float sx = 1, sy = 1;
    } bg_;
    struct Transition {
        bool active = false;
        float t = 0, dur = 0;      // 秒
        SDL_Texture* snap = nullptr;
    } trans_;
    struct CharDraw {
        bool show = false;
        int id = -1, no = 0, pos = 0;
        float alpha = 1, targetAlpha = 1;
        float fadePerSec = 0;
    } chars_[kMaxChars];
    struct BmpTex { std::string path; int z = 0; float alpha = 1, targetAlpha = 1; float fadePerSec = 0; };
    std::map<int, BmpTex> bmps_;
    struct Fb { uint8_t r = 0, g = 0, b = 0; float alpha = 0, targetAlpha = 0, fadePerSec = 0; } fb_;
    int shakeX_ = 0, shakeY_ = 0;
    float shakeUntil_ = 0;

    // ---- 引擎对象 ----
    Res res_;
    Gfx gfx_;
    Audio audio_;
    Config config_;
    SceneState scene_;
    std::vector<std::unique_ptr<Script>> stack_;
    std::vector<std::unique_ptr<Script>> graveyard_;
    std::vector<int> gameFlags_;
    std::vector<uint8_t> sysFlags_;
    State state_ = State::Logo;
    float logoUntil_ = 0;
    float scriptAcc_ = 0;

    // ---- 选项 ----
    int selectCursor_ = 0;
    int selectBase_ = 0;

    // ---- 日历 ----
    int calY_ = 0, calM_ = 0, calD_ = 0, calDow_ = 0;
    float calUntil_ = 0;

    // ---- UI ----
    enum class UiMode { None, Title, Menu, Save, Load, Config, Backlog };
    UiMode ui_ = UiMode::None;
    int uiCursor_ = 0;
    int backlogScroll_ = 0;
    float autoTimer_ = -1;
    bool autoMode_ = false;
    bool skipMode_ = false;
    bool skipDisable_ = false;
    uint32_t title_ = 0;

    // ---- 路径/配置 ----
    std::string dataDir_, saveDir_;
    std::string startScript_ = "1001";
    std::string effectMode_;
    int timeMode_ = 0;
    int voiceLabel_ = 0;
    bool demoMode_ = false;
    bool clicked_ = false;
    uint32_t timerStart_ = 0;

    // ---- 内部 ----
    bool LoadGameData();
    void LoadConfigFile();
    void SaveConfigFile();
    void TickInput();
    void TickScript(float dt);
    void Render();
    void RenderAdvWindow();
    void RenderSelect();
    void RenderCalender();
    void RenderUi();
    void UpdateAnims(float dt);
    void ClickAdvance();          // 点击推进文本
    void SetupNewBg(const std::string& path, int frame, int x, int y, int offset, float sx, float sy, bool keepChar);
    void ApplySav(const SaveData& sav);
    void BuildSav(SaveData* sav);
    bool SaveToSlotFile(int slot);
    bool LoadFromSlotFile(int slot);
    std::string SlotPath(int slot) const;

    Script* Active() { return stack_.empty() ? nullptr : stack_.back().get(); }
    void MarkAnim(float seconds);
    std::string SlotMeta(int slot);

    // 通用菜单输入:确认返回索引
    bool HandleMenuInputImpl(int count, bool allowCancel, int* outIdx, bool* canceled);
    void BacklogInput();
    void ConfigAdjustInput(int count);

    template <typename Fn>
    void HandleMenuInput(int count, bool allowCancel, Fn onOk) {
        int idx = -1;
        bool canceled = false;
        if (HandleMenuInputImpl(count, allowCancel, &idx, &canceled) && idx >= 0)
            onOk(idx);
    }
};

} // namespace wa2
