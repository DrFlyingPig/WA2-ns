// engine.h — 引擎宿主(Host 实现):状态机/渲染集成/输入/存档/UI
#pragma once

#include "wa2.h"
#include "funcs.h"
#include "res.h"
#include "gfx.h"
#include "audio.h"
#include "video.h"
#include "sav.h"
#include "character_state.h"
#include "special_mode.h"
#ifdef WA2_DIAG_TEXT_FIT
#include "text_layout.h"
#endif

namespace wa2 {

class Engine : public Host {
public:
    ~Engine();
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
    void RemoveChar(int id) override;
    void BgMove(int dx, int dy, int frames) override;
    void WaitBgMove() override;
    void StopBgMove() override;
    void ColorFade(int r, int g, int b, int frames) override;
    void ColorFadeFrom(int r, int g, int b, int frames) override;
    void SetFBColor(int r, int g, int b) override;
    void Shake(int type, int power, int frames) override;
    void ShowCalender(int y, int m, int d, int dow) override;
    void PlayMovie(int movieId, int flagIdx) override;
    int  TimeMode() const override;
    void SetTimeMode(int v) override;
    void SetEffectMode(const std::string& file) override;
    void SetWeather(int flag, int speedX, int speedY, int turbulence,
                    int count, int flag2, int index) override;
    void ChangeWeather(int speedX, int speedY, int count,
                       int turbulence, int index) override;
    void ResetWeather() override;
    void SetEroMode(bool v) override;
    int ReplayMode() const override { return replayMode_; }
    bool CanSkip() const override;
    bool Clicked() const override;

    void AddSelectItem(const std::string& text, int v1, int v2, int v3) override;
    void ShowSelect() override;

    void PlayBgm(int id, bool loop, int vol) override;
    void StopBgm(int fadeFrames) override;
    void SetVoiceLabel(int label) override;
    int  CurrentVoiceLabel() const override;
    void PlayVoice(int label, int id, int chr, int volume,
                   bool loop, int channel) override;
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
    void WaitUntilTimerMs(float targetMs) override;

    void LoadBmp(int id, const std::string& path, int z) override;
    void ReleaseBmp(int id) override;
    void SetBmpParam(int id, int mode, int alpha, int frames) override;

private:
    bool initialized_ = false;
    bool shutdown_ = false;
    bool suppressPersistence_ = false; // PC screenshot regression must be read/write isolated
    enum class State { Logo, Title, Game, Quit };

    // ---- 等待门(CheckScript 语义)----
    struct Wait {
        bool textBusy = false;      // 打字机未完成
        bool waitClick = false;     // 等点击
        bool timer = false;         // WaitMs/WaitVoice/WaitSe
        bool c4Timer = false;       // 仅 0xC4 时间轴等待，可由完成对白后的确认解除
        float timerUntil = 0;
        bool animBusy = false;      // 过渡/立绘/FB 动画进行中
        float animUntil = 0;
        bool selectVisible = false;
        bool calender = false;
        bool menu = false;          // 任意 UI 打开
        bool movie = false;         // ASF/WMV 影片播放中
        bool Blocking() const {
            return textBusy || waitClick || timer || animBusy ||
                   selectVisible || calender || menu || movie;
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
#ifdef WA2_DIAG_TEXT_FIT
        // 只在段落切换时重排，避免逐字显示期间换行跳动，也避免每帧重复扫描。
        std::string layoutText;
        DialogueTextLayout layout;
        bool layoutNovelMode = false;
#endif
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
        // 掩码溶解(B 命令 efc>=128):oldPix=过渡开始时的整屏像素,
        // mask 为 8-bit 灰度图 f0{id:03d}.bmp 的原尺寸数据。
        bool maskMode = false;
        std::vector<uint8_t> oldPix;
        std::vector<uint8_t> maskPix;
        int maskW = 0, maskH = 0;
    } trans_;
    // S 背景平移默认与脚本并行；只有 WSZ 才等待它。
    struct BgMoveAnim {
        bool active = false;
        float fromX = 0, fromY = 0, toX = 0, toY = 0;
        float t = 0, dur = 0;
    } bgMove_;
    using CharDraw = CharacterVisualState;
    CharDraw chars_[kMaxChars];
    struct BmpTex { std::string path; int z = 0; float alpha = 1, targetAlpha = 1; float fadePerSec = 0; };
    std::map<int, BmpTex> bmps_;
    // wa2-godot 的 fb 是以 0.5 为中性的全局色调，不是一个不透明色块。
    struct Fb {
        float r = 0.5f, g = 0.5f, b = 0.5f;
        float targetR = 0.5f, targetG = 0.5f, targetB = 0.5f;
        float remaining = 0.0f;
    } fb_;
    struct Weather {
        bool active = false;
        int flag = 0, speedX = 0, speedY = 0, turbulence = 0;
        int count = 0, flag2 = 0, index = 0;
        uint32_t startedMs = 0;
    } weather_;
    int shakeX_ = 0, shakeY_ = 0;
    float shakeUntil_ = 0;

    // ---- 引擎对象 ----
    Res res_;
    Gfx gfx_;
    Audio audio_;
    VideoPlayer video_;
    Config config_;
    SceneState scene_;
    std::vector<std::unique_ptr<Script>> stack_;
    std::vector<std::unique_ptr<Script>> graveyard_;
    std::vector<int> gameFlags_;
    std::vector<uint8_t> sysFlags_;
    State state_ = State::Logo;
    float logoUntil_ = 0;
    bool bootLogoLaunched_ = false;  // 是否已尝试播放启动公司 Logo 影片(mv000)
    float scriptAcc_ = 0;

    // ---- 选项 ----
    int selectCursor_ = 0;
    int selectBase_ = 0;

    // ---- 日历 ----
    int calY_ = 0, calM_ = 0, calD_ = 0, calDow_ = 0;
    float calUntil_ = 0;

    // ---- UI ----
    enum class UiMode { None, Title, TitleStart, TitleSpecial, TitleNovel,
                        TitleCg, TitleCgView, TitleScene, TitleBgm, TitleVoice,
                        Menu, Save, Load, Config, Backlog };
    UiMode ui_ = UiMode::None;
    int uiCursor_ = 0;
    int uiPage_ = 0;
    int uiScroll_ = 0;
    int backlogScroll_ = 0;
    float autoTimer_ = -1;
    bool autoMode_ = false;
    bool skipMode_ = false;
    bool skipDisable_ = false;
    uint32_t title_ = 0;
    bool titleBgmStarted_ = false;
    int cgViewSlot_ = -1;
    int cgViewVariant_ = 0;
    int replayMode_ = 0;
    int voiceMessagePlaying_ = -1;
    bool configResetConfirm_ = false;
    int configResetCursor_ = 0; // 0=是,1=否

    // ---- 路径/配置 ----
    std::string dataDir_, saveDir_;
    std::string startScript_ = "1001";
    std::string effectMode_;
    int timeMode_ = 0;
    int voiceLabel_ = 0;
    bool demoMode_ = false;
    bool clicked_ = false;
    bool cancelClicked_ = false;
    int navX_ = 0, navY_ = 0;       // 菜单/选项单次方向输入
    bool uiPagePrev_ = false, uiPageNext_ = false;
    bool pointerPressed_ = false, pointerHeld_ = false;
    int pointerX_ = -1, pointerY_ = -1;
    uint32_t pointerHeldSince_ = 0;
    bool holdSkip_ = false;
    bool movieSkippable_ = false;
    uint32_t timerStart_ = 0;

    // 原版 SYSTEM.sav 中 CG 收藏区、已读消息区与普通系统旗标彼此独立。
    std::unordered_set<int> unlockedCgs_;
    std::unordered_set<uint64_t> readMessages_;
    bool currentMessageWasRead_ = false;
    bool systemDirty_ = false;
    uint32_t lastSystemSaveMs_ = 0;

    // ---- 内部 ----
    bool LoadGameData();
    void LoadConfigFile();
    void SaveConfigFile();
    void LoadSystemFile();
    void SaveSystemFile();
    void ImportOriginalSystemFile();
    void MergeProgressFromSaves();
    void ApplyAudioConfig();
    void TickInput();
    void TickScript(float dt);
    void Render();
    void RenderWeather(bool frontLayer);
    void RenderAdvWindow();
    void RenderSelect();
    void RenderCalender();
    void RenderUi();
    void RenderTitleBackdrop();
    void RenderConfigUi();
    void RenderCgMode();
    void RenderCgViewer();
    void RenderSceneReplay();
    void RenderBgmMode();
    void RenderVoiceMessages();
    void DrawSpecialBackdrop(const std::string& title, const std::string& subtitle,
                             bool paged = true);
    void DrawSpecialGridCell(int index, int x, int y, int w, int h,
                             bool selected, bool unlocked, int imageId);
    void StartChapter(const std::string& scriptName);
    void StartReplay(int slot);
    void OpenSpecialMode(UiMode mode);
    void CloseSpecialMode();
    void UpdateAnims(float dt);
    bool NeedsContinuousRedraw() const;
    void ClickAdvance();          // 点击推进文本
    void SetupNewBg(const std::string& path, int frame, int x, int y, int offset, float sx, float sy, bool keepChar, int efc = 0);
    // 掩码溶解:在 Render 之后、Present 之前对新帧做 CPU 混合。
    void ApplyMaskTransition();
    bool ApplySav(const SaveData& sav);
    void BuildSav(SaveData* sav);
    bool SaveToSlotFile(int slot);
    bool LoadFromSlotFile(int slot);
    std::string SlotPath(int slot) const;

    Script* Active() { return stack_.empty() ? nullptr : stack_.back().get(); }
    const Script* Active() const { return stack_.empty() ? nullptr : stack_.back().get(); }
    void MarkAnim(float seconds);
    std::string SlotMeta(int slot);

    // 通用菜单输入:确认返回索引
    bool HandleMenuInputImpl(int count, bool allowCancel, int* outIdx, bool* canceled);
    void CancelUi();
    void BacklogInput();
    void ConfigAdjustInput(int count);
    bool PointerIn(int x, int y, int w, int h) const;
    void ConsumeGridInput(int count, int columns);
    bool IsCgUnlocked(int id) const;
    void UnlockCg(int id);
    uint64_t MessageReadKey(int msgIdx) const;

    template <typename Fn>
    void HandleMenuInput(int count, bool allowCancel, Fn onOk) {
        int idx = -1;
        bool canceled = false;
        if (HandleMenuInputImpl(count, allowCancel, &idx, &canceled) && idx >= 0)
            onOk(idx);
    }
};

} // namespace wa2
