// engine.cpp — 引擎宿主实现
#include "engine.h"
#include "util.h"
#include "funcs.h"

#include <SDL2/SDL.h>
#ifdef __SWITCH__
#include <switch.h>   // appletMainLoop: 正确的 Switch 生命周期
#endif
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>   // getenv(WA2_REAL / WA2_REAL_START)

namespace wa2 {

// 文本窗布局：对应 wa2-godot/AdvMain.tscn 的原版 1280x720 坐标。
static const int kWinX = 16, kWinY = 505, kWinW = 1248, kWinH = 208;
static const int kTextX = 278, kTextY = 562, kNameX = 277, kNameY = 524;
static const int kNameSize = 28, kTextSize = 32;
#ifdef WA2_DIAG_TEXT_FIT
#ifdef WA2_RELEASE_BUILD
// 正式版：用户在 F5 实机范围上确认可再增加一个全角字格。
// 正文从 x=278 到 x=978，共 25*28=700px。
static const int kAdvTextWidth = 25 * 28;
#elif defined(WA2_DIAG_TEXT_SNOW_SAFE)
// F5：原始 UI 贴图在正文高度内的第一枚高可见度右侧雪花约从屏幕
// x=972 开始。正文止于 x=950（24*28），连 2px 字形阴影也保留约 20px 间隔。
static const int kAdvTextWidth = 24 * 28;
#elif defined(WA2_DIAG_TEXT_SAFE_WIDTH)
// F4：参考 Wa2AdvMain/Wa2Label 的正文安全区是 x=278 起、28 个 28px
// 全角字格，即右边界 x=1062。F3 扩到 x=1198 后进入了 UI 右侧雪花装饰区。
static const int kAdvTextWidth = 28 * 28;
#elif defined(WA2_DIAG_TEXT_REFLOW)
// F3 历史诊断值；保留以便复现已被实机否决的横向过宽版本。
static const int kAdvTextWidth = 920;
#else
// F2 参考 Wa2Label 的固定 28px * 28 字布局。
static const int kAdvTextWidth = 28 * 28;
#endif
// 底部额外留 8px，避免字形阴影压住窗口边框。
static const int kAdvTextSafeBottom = kWinY + kWinH - 8;
static const int kAdvTextHeight = kAdvTextSafeBottom - kTextY - 2;
static const int kNovelTextX = 80, kNovelTextY = 40;
static const int kNovelTextWidth = 39 * 28;
static const int kNovelTextSafeBottom = kVirtualH - 40;
static const int kNovelTextHeight = kNovelTextSafeBottom - kNovelTextY - 2;
#endif

// 0.1.8 及更早的自定义存档没有写入背景路径。为这些已经存在的存档
// 只重放脚本的轻量状态指令（不解码图片/声音），恢复目标位置之前最后
// 一张背景；新存档直接使用 RST2 状态，不走这条兼容路径。
class LegacySceneProbe final : public Host {
public:
    LegacySceneProbe(Res& res, const std::vector<int>& gameFlags,
                     const std::vector<uint8_t>& sysFlags)
        : res_(res), gameFlags_(gameFlags), sysFlags_(sysFlags) {}

    bool Recover(const std::string& name, uint32_t targetPos, BgInfo* out) {
        if (!out || name.empty()) return false;
        auto first = std::make_unique<Script>();
        first->SetGameFlags(&gameFlags_);
        if (!first->Load(res_, name, 0)) return false;
        stack_.push_back(std::move(first));
        for (int ticks = 0; ticks < 250000 && !stack_.empty() && !failed_; ++ticks) {
            Script* active = stack_.back().get();
            if (active->name() == name && active->pos() >= targetPos) {
                if (!scene_.bg.path.empty()) { *out = scene_.bg; return true; }
                return false;
            }
            const TickResult result = active->Tick(*this);
            graveyard_.clear();
            if (result == TickResult::End && !stack_.empty()) stack_.pop_back();
        }
        return false;
    }

    void SLoadScript(const std::string& name, int point) override {
        for (auto& s : stack_) graveyard_.push_back(std::move(s));
        stack_.clear();
        PushScript(name, point);
        scene_.ClearChars();
    }
    void SCallScript(const std::string& name, int point) override { PushScript(name, point); }
    void CallPoint(int point) override {
        if (!stack_.empty()) PushScript(stack_.back()->name(), point);
    }
    void GoTitle() override { failed_ = true; }
    void PushInt(int v) override { if (!stack_.empty()) stack_.back()->PushInt(5, 3, v); }
    void PushFloat(float v) override { if (!stack_.empty()) stack_.back()->PushFloat(5, 4, v); }

    int ReadSysFlag(int idx) override {
        return idx >= 0 && idx < (int)sysFlags_.size() ? sysFlags_[(size_t)idx] : 0;
    }
    void WriteSysFlag(int idx, int v) override {
        if (idx >= 0 && idx < (int)sysFlags_.size()) sysFlags_[(size_t)idx] = (uint8_t)v;
    }
    int ReadGameFlag(int idx) override {
        return idx >= 0 && idx < (int)gameFlags_.size() ? gameFlags_[(size_t)idx] : 0;
    }
    void WriteGameFlag(int idx, int v) override {
        if (idx >= 0 && idx < (int)gameFlags_.size()) gameFlags_[(size_t)idx] = v;
    }

    void ShowMessage(const std::string&, int, int, bool) override {}
    void EndMessage() override {}
    void SetName(const std::string&) override {}
    void WaitClick() override {}
    void HideWindow(int) override {}
    void SetNovelMode(bool v) override { scene_.novelMode = v; }
    void SetDemoMode(bool v) override { scene_.demoMode = v; }

    void RenderImage(int id, int, bool keepChar, int type, int, int offset,
                     int x, int y, float sx, float sy) override {
        if (id >= 0) {
            scene_.bg.id = id;
            scene_.bg.type = type;
            scene_.bg.path = type == 1 ? Res::CgName(id)
                : type == 2 ? Res::HName(id) : Res::BgName(id, timeMode_);
        }
        scene_.bg.x = x;
        scene_.bg.y = y;
        scene_.bg.offset = offset;
        scene_.bg.sx = sx > 0 ? sx : 1.0f;
        scene_.bg.sy = sy > 0 ? sy : 1.0f;
        if (!keepChar) scene_.ClearChars();
    }
    void AddChar(int id, int no, int pos) override { scene_.AddOrUpdateChar(id, no, pos); }
    void UpdateChar(int) override {}
    void RemoveChar(int id) override { scene_.RemoveCharById(id); }
    void BgMove(int x, int y, int) override { scene_.bg.x = x; scene_.bg.y = y; scene_.bg.offset = 0; }
    void ColorFade(int, int, int, int) override {}
    void ShowCalender(int, int, int, int) override {}
    int TimeMode() const override { return timeMode_; }
    void SetTimeMode(int v) override { timeMode_ = v; scene_.timeMode = v; }
    void SetEffectMode(const std::string& v) override { scene_.effectMode = v; }
    void SetEroMode(bool v) override { scene_.eroMode = v; }

    void AddSelectItem(const std::string& text, int v1, int v2, int v3) override {
        scene_.selectItems.push_back({text, v1, v2, v3});
    }
    void ShowSelect() override {
        if (!stack_.empty() && !stack_.back()->args.empty()) {
            Val choice;
            choice.kind = Val::Int;
            choice.i = 0;
            stack_.back()->SetVar(stack_.back()->args.back(), choice);
        }
        scene_.selectItems.clear();
    }

    void PlayBgm(int, bool, int) override {}
    void StopBgm(int) override {}
    void PlayVoice(int, int, int, int, bool, int) override {}
    void WaitVoice(int) override {}
    void StopVoice(int, int) override {}
    void PlaySe(int, int, bool, int, int) override {}
    void StopSe(int, int) override {}
    void WaitSe(int) override {}
    void WaitMs(float) override {}
    void StartTimer() override {}
    int ElapsedTimerMs() override { return INT_MAX; }

private:
    void PushScript(const std::string& name, int point) {
        auto script = std::make_unique<Script>();
        script->SetGameFlags(&gameFlags_);
        if (!script->Load(res_, name, point)) { failed_ = true; return; }
        stack_.push_back(std::move(script));
    }

    Res& res_;
    std::vector<int> gameFlags_;
    std::vector<uint8_t> sysFlags_;
    SceneState scene_;
    std::vector<std::unique_ptr<Script>> stack_;
    std::vector<std::unique_ptr<Script>> graveyard_;
    int timeMode_ = 0;
    bool failed_ = false;
};

Engine::~Engine() {
    Shutdown();
}

bool Engine::Init(const std::string& dataDirOverride) {
    initialized_ = false;
    shutdown_ = false;
    suppressPersistence_ = false;
    gameFlags_.assign(kMaxGameFlags, 0);
    sysFlags_.assign(kSysFlagCount, 0);

    // 数据目录:Switch 正式包只读取 sdmc:/wa2；PC 保留开发目录回退。
    if (!dataDirOverride.empty()) dataDir_ = dataDirOverride;
    else {
#ifdef __SWITCH__
#ifdef WA2_WITH_DEMO
        if (FileExists("sdmc:/wa2/game.ini")) dataDir_ = "sdmc:/wa2";
        else dataDir_ = "romfs:/demo";
#else
        dataDir_ = "sdmc:/wa2";
#endif
#else
        if (FileExists("Wa2Res/game.ini")) dataDir_ = "Wa2Res";
        else if (FileExists("out/Wa2Res/game.ini")) dataDir_ = "out/Wa2Res";
        else dataDir_ = "demo";
#endif
    }
    saveDir_ =
#ifdef __SWITCH__
        "sdmc:/wa2";
#else
        ""; // SDL 初始化后改为系统用户配置目录，不向原游戏资源目录写文件。
#endif
    Log(LogLevel::Info, "engine: data dir = %s", dataDir_.c_str());

    if (!LoadGameData()) {
        Log(LogLevel::Error, "engine: game data not found in %s", dataDir_.c_str());
        return false;
    }

    // 字体:数据目录下 font.ttf
    std::string fontPath = PathJoin(dataDir_, "font.ttf");
    if (!FileExists(fontPath)) {
        Log(LogLevel::Error, "engine: font missing: %s (把任意 TTF 字体放到数据目录)",
            fontPath.c_str());
        return false;
    }
    if (!gfx_.Init(fontPath)) return false;
#ifndef __SWITCH__
    if (char* pref = SDL_GetPrefPath("WA2-ns", "WA2-ns")) {
        saveDir_ = pref;
        SDL_free(pref);
    } else {
        saveDir_ = ".";
    }
#endif
    LoadConfigFile();
    LoadSystemFile();
    ImportOriginalSystemFile();
    MergeProgressFromSaves();
    Log(LogLevel::Info, "engine: save dir = %s", saveDir_.c_str());
    if (res_.UsesPatchFont() && !gfx_.EnablePatchFont(res_)) return false;
    if (!audio_.Init()) return false;
    if (!video_.Init(gfx_.renderer())) return false;
    ApplyAudioConfig();

    // 原版标题场景自己负责 Logo/背景动画；启动阶段只保留极短黑屏。
    logoUntil_ = 0.15f;
    title_ = SDL_GetTicks();
    initialized_ = true;
    return true;
}

bool Engine::LoadGameData() {
    res_.SetDataDir(dataDir_);
    res_.ScanArchives();
    // game.ini: title=... / start=...
    std::vector<uint8_t> ini = ReadFileAll(PathJoin(dataDir_, "game.ini"));
    for (const auto& line : Split(std::string(ini.begin(), ini.end()), '\n')) {
        auto kv = Split(Trim(line), '=');
        if (kv.size() == 2) {
            if (kv[0] == "start") startScript_ = Trim(kv[1]);
        }
    }
    return true;
}

void Engine::Run() {
#ifdef WA2_REAL
    // 真实数据冒烟:强制进入 game 并加载 argv 指定的启动脚本号(默认1001),
    // 自动每秒点几次推进,跑真实 White Album 2 数据(经 Res ScanArchives 读 .pak)。
    Log(LogLevel::Info, "REAL: auto-run real data, forcing game start");
    LogFlush();
    {
        const char* e = std::getenv("WA2_REAL_START");
        std::string sn = e && *e ? e : "1001";
        state_ = State::Game; ui_ = UiMode::None;
        SLoadScript(sn, 0);
        Log(LogLevel::Info, "REAL: started script %s", sn.c_str());
        LogFlush();
    }
    {
        uint32_t clickEveryMs = 260;
        if (const char* clickEnv = std::getenv("WA2_REAL_CLICK_MS")) {
            unsigned long parsed = std::strtoul(clickEnv, nullptr, 10);
            if (parsed >= 50 && parsed <= 60000) clickEveryMs = (uint32_t)parsed;
        }
        Log(LogLevel::Info, "REAL: auto click every %u ms", clickEveryMs);
        uint32_t last = SDL_GetTicks(), lastClick = 0;
        uint64_t frames = 0;
        while (state_ != State::Quit) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) { if (ev.type == SDL_QUIT) state_ = State::Quit; }
            uint32_t now = SDL_GetTicks();
            float dt = (now - last) / 1000.0f; if (dt > 0.1f) dt = 0.1f; last = now;
            if (now - lastClick >= clickEveryMs) { lastClick = now; clicked_ = true; }
            TickInput();
            audio_.Update();
            if (systemDirty_ && now - lastSystemSaveMs_ >= 2000u) SaveSystemFile();
            video_.Update();
            if (wait_.movie && !video_.Playing()) wait_.movie = false;
            UpdateAnims(dt);
            if (state_ == State::Game) {
                scriptAcc_ += dt;
                while (scriptAcc_ >= kScriptTick) { scriptAcc_ -= kScriptTick; TickScript(kScriptTick); }
            }
            Render();
            if (frames == 360) {
                if (const char* shot = std::getenv("WA2_REAL_SCREENSHOT")) {
                    if (*shot) gfx_.SaveScreenshot(shot);
                }
            }
            gfx_.Present();
            frames++;
            SDL_Delay(16);
            if ((frames % 120) == 0) {
                std::string t = adv_.visible && !adv_.segments.empty()
                    ? adv_.segments[adv_.seg] : std::string();
                if (t.size() > 24) t.resize(24);
                Log(LogLevel::Info, "REAL: frame=%llu text=%s show=%d hide=%d shown=%d seg=%d wait=(txt:%d click:%d timer:%d/%.2fs anim:%d/%.2fs sel:%d cal:%d movie:%d) fb=(%.3f,%.3f,%.3f)",
                    (unsigned long long)frames, t.c_str(), (int)adv_.visible,
                    (int)adv_.hide, adv_.shown, (int)adv_.seg,
                    (int)wait_.textBusy, (int)wait_.waitClick, (int)wait_.timer,
                    wait_.timerUntil - SDL_GetTicks() / 1000.0f,
                    (int)wait_.animBusy, wait_.animUntil - SDL_GetTicks() / 1000.0f,
                    (int)wait_.selectVisible, (int)wait_.calender, (int)wait_.movie,
                    fb_.r, fb_.g, fb_.b);
                LogFlush();
            }
        }
    }
    return;
#endif
    uint32_t last = SDL_GetTicks();
#ifdef WA2_DIAG_STATIC_REDRAW
    uint32_t lastPresentedMs = 0;
    uint32_t perfStartedMs = last;
    uint32_t redrawDebt = 2;
    uint64_t perfLoops = 0, perfDraws = 0, perfSkips = 0;
    uint64_t perfDrawTicks = 0, perfMaxDrawTicks = 0;
    const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
#endif
    // 无窗口焦点依赖的标题菜单回归入口；普通运行未设置环境变量时完全不启用。
    int titleTestTarget = -1, titleTestMoves = 0;
    if (const char* test = std::getenv("WA2_TEST_CHAPTER")) {
        int v = std::atoi(test);
        if (v >= 0 && v <= 2) titleTestTarget = v;
    }
#ifndef __SWITCH__
    // 无人值守 UI 截图入口，只在 PC 回归中启用；正式运行没有环境变量时零影响。
    const char* uiScreenshotEnv = std::getenv("WA2_TEST_UI_SCREENSHOT");
    const std::string uiScreenshot = uiScreenshotEnv ? uiScreenshotEnv : "";
    int uiScreenshotFrames = 0;
    int uiTestClickX = -1, uiTestClickY = -1;
    if (!uiScreenshot.empty()) {
        suppressPersistence_ = true;
        config_.SetDefaults();
        ApplyAudioConfig();
        sysFlags_.assign(kSysFlagCount, 0);
        unlockedCgs_.clear();
        readMessages_.clear();
        systemDirty_ = false;
        const std::string mode = std::getenv("WA2_TEST_UI_MODE")
            ? std::getenv("WA2_TEST_UI_MODE") : "config";
        state_ = State::Title;
        titleBgmStarted_ = true;
        uiCursor_ = uiPage_ = uiScroll_ = 0;
        if (mode == "cg") ui_ = UiMode::TitleCg;
        else if (mode == "scene") ui_ = UiMode::TitleScene;
        else if (mode == "bgm") ui_ = UiMode::TitleBgm;
        else if (mode == "voice") ui_ = UiMode::TitleVoice;
        else if (mode == "special") ui_ = UiMode::TitleSpecial;
        else if (mode == "title") ui_ = UiMode::Title;
        else ui_ = UiMode::Config;
        if (const char* click = std::getenv("WA2_TEST_UI_CLICK"))
            if (std::sscanf(click, "%d,%d", &uiTestClickX, &uiTestClickY) != 2)
                uiTestClickX = uiTestClickY = -1;
        if (const char* page = std::getenv("WA2_TEST_UI_PAGE")) {
            const int requested = std::atoi(page);
            if (ui_ == UiMode::Config) uiPage_ = std::clamp(requested, 0, 2);
            else if (ui_ == UiMode::TitleCg) uiPage_ = std::clamp(requested, 0, 13);
            else if (ui_ == UiMode::TitleScene || ui_ == UiMode::TitleBgm)
                uiPage_ = std::clamp(requested, 0, 1);
        }
        if (ui_ == UiMode::Config) {
            if (const char* cursor = std::getenv("WA2_TEST_UI_CURSOR"))
                uiCursor_ = std::clamp(std::atoi(cursor), 0,
                    uiPage_ == 0 ? 7 : uiPage_ == 1 ? 13 : 3);
            if (std::getenv("WA2_TEST_UI_CONFIRM")) {
                configResetConfirm_ = true;
                configResetCursor_ = config_.confirmDefaultYes ? 0 : 1;
            }
        }

        // Give the screenshot-only route a small unlocked sample.  This is
        // deliberately written straight into the in-memory collections so a
        // visual regression run can exercise both states without touching the
        // user's persistent system data.
        if (mode == "cg") {
            const auto& slots = CgSlots();
            const int base = uiPage_ * 12;
            for (int i = 0; i < 4; ++i)
                if (!slots[(size_t)(base + i)].empty())
                    unlockedCgs_.insert(slots[(size_t)(base + i)].front());
        } else if (mode == "scene") {
            const auto& slots = SceneReplaySlots();
            const int base = uiPage_ * 12;
            for (int i = 0; i < 4; ++i)
                sysFlags_[(size_t)slots[(size_t)(base + i)].unlockFlag] = 1;
        } else if (mode == "bgm") {
            const auto& tracks = BgmSlots();
            const int base = uiPage_ == 0 ? 0 : 31;
            for (int i = 0; i < 8; ++i)
                sysFlags_[(size_t)(100 + tracks[(size_t)(base + i)])] = 1;
        }
    }
#endif
#ifdef __SWITCH__
    bool appletAlive = true;
    while (state_ != State::Quit && (appletAlive = appletMainLoop())) {
#else
    while (state_ != State::Quit) {
#endif
        uint32_t frameStart = SDL_GetTicks();
        uint32_t now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;

#ifdef WA2_DIAG_STATIC_REDRAW
        const UiMode uiBeforeInput = ui_;
        const State stateBeforeInput = state_;
        const bool autoBeforeInput = autoMode_;
        const bool skipBeforeInput = skipMode_;
        const bool movieBeforeInput = video_.Playing();
#endif
        TickInput();
#ifndef __SWITCH__
        if (!uiScreenshot.empty() && uiScreenshotFrames == 0 &&
            uiTestClickX >= 0 && uiTestClickY >= 0) {
            pointerX_ = uiTestClickX;
            pointerY_ = uiTestClickY;
            pointerPressed_ = clicked_ = true;
        }
#endif
        audio_.Update();
        if (systemDirty_ && now - lastSystemSaveMs_ >= 2000u) SaveSystemFile();
        if (titleTestTarget >= 0) {
            if (state_ == State::Title && ui_ == UiMode::Title) {
                clicked_ = true;
            } else if (state_ == State::Title && ui_ == UiMode::TitleStart) {
                if (titleTestMoves < titleTestTarget) {
                    navY_ = 1;
                    ++titleTestMoves;
                } else {
                    clicked_ = true;
                    titleTestTarget = -1;
                }
            }
        }
#ifdef WA2_DIAG_STATIC_REDRAW
        const bool inputActivity = clicked_ || cancelClicked_ || navX_ || navY_ ||
            ui_ != uiBeforeInput || state_ != stateBeforeInput ||
            autoMode_ != autoBeforeInput || skipMode_ != skipBeforeInput ||
            video_.Playing() != movieBeforeInput;
        const bool wasContinuouslyChanging = NeedsContinuousRedraw();
        const bool movieWasPlaying = video_.Playing();
        bool visualChanged = inputActivity;
#endif
        video_.Update();
        if (wait_.movie && !video_.Playing()) wait_.movie = false;
#ifdef WA2_DIAG_STATIC_REDRAW
        visualChanged = visualChanged || (movieWasPlaying != video_.Playing());
#endif
        UpdateAnims(dt);

        if (state_ == State::Logo && now / 1000.0f >= logoUntil_) {
            state_ = State::Title;
            ui_ = UiMode::Title;
            uiCursor_ = 0;
#ifdef WA2_DIAG_STATIC_REDRAW
            visualChanged = true;
#endif
        }
        if (state_ == State::Title && !titleBgmStarted_) {
            if (audio_.PlayBgm(31, true, 255, res_)) WriteSysFlag(100 + 31, 1);
            titleBgmStarted_ = true;
        }

        // 脚本节拍 30Hz
        if (state_ == State::Game) {
            scriptAcc_ += dt;
            while (scriptAcc_ >= kScriptTick) {
                scriptAcc_ -= kScriptTick;
#ifdef WA2_DIAG_STATIC_REDRAW
                // 非阻塞脚本 tick 可以修改任意场景状态；阻塞状态只在真实
                // 输入/自动推进时可能改变。先记脏，再让既有 D5 输入路径处理。
                if (!wait_.Blocking() || inputActivity || skipMode_ || autoMode_)
                    visualChanged = true;
#endif
                TickScript(kScriptTick);
            }
        }

#ifdef WA2_DIAG_STATIC_REDRAW
        const bool continuouslyChanging = NeedsContinuousRedraw();
        if (wasContinuouslyChanging && !continuouslyChanging) visualChanged = true;
        if (visualChanged) redrawDebt = 2;
        const bool heartbeat = frameStart - lastPresentedMs >= 1000u;
        const bool shouldDraw = redrawDebt || continuouslyChanging || heartbeat;
        ++perfLoops;
        if (shouldDraw) {
            const uint64_t drawStart = SDL_GetPerformanceCounter();
            Render();
            gfx_.Present();
            const uint64_t drawTicks = SDL_GetPerformanceCounter() - drawStart;
            perfDrawTicks += drawTicks;
            perfMaxDrawTicks = std::max(perfMaxDrawTicks, drawTicks);
            ++perfDraws;
            lastPresentedMs = SDL_GetTicks();
            if (redrawDebt) --redrawDebt;
        } else {
            ++perfSkips;
        }

        const uint32_t perfNow = SDL_GetTicks();
        if (perfNow - perfStartedMs >= 10000u) {
            size_t visibleChars = 0;
            for (const auto& c : chars_)
                if (c.show && c.alpha > 0.001f) ++visibleChars;
            const double avgMs = perfDraws && perfFrequency
                ? (double)perfDrawTicks * 1000.0 /
                    ((double)perfFrequency * (double)perfDraws) : 0.0;
            const double maxMs = perfFrequency
                ? (double)perfMaxDrawTicks * 1000.0 / (double)perfFrequency : 0.0;
            Log(LogLevel::Info,
                "perf: loops=%llu draw=%llu skip=%llu draw-rate=%.1f%% avg=%.2fms max=%.2fms continuous=%d chars=%zu bmps=%zu cache=%.1f/%.1fMiB",
                (unsigned long long)perfLoops,
                (unsigned long long)perfDraws,
                (unsigned long long)perfSkips,
                perfLoops ? (double)perfDraws * 100.0 / (double)perfLoops : 0.0,
                avgMs, maxMs, continuouslyChanging ? 1 : 0,
                visibleChars, bmps_.size(),
                (double)gfx_.CachedTextureBytes() / (1024.0 * 1024.0),
                (double)gfx_.TextureCacheBudget() / (1024.0 * 1024.0));
            perfStartedMs = perfNow;
            perfLoops = perfDraws = perfSkips = 0;
            perfDrawTicks = perfMaxDrawTicks = 0;
        }
#else
        Render();
        gfx_.Present();
#endif

#ifndef __SWITCH__
        if (!uiScreenshot.empty() && ++uiScreenshotFrames >= 4) {
            gfx_.SaveScreenshot(uiScreenshot);
            state_ = State::Quit;
        }
#endif

        // 帧节奏：Switch 原生 framebuffer 的 CPU 合成限制为约 30fps，
        // 与脚本 30Hz 节拍一致，并为音频/解码线程留出 CPU；PC 保持 60fps。
        uint32_t used = SDL_GetTicks() - frameStart;
#ifdef __SWITCH__
        constexpr uint32_t targetFrameMs = 33;
#else
        constexpr uint32_t targetFrameMs = 16;
#endif
        if (used < targetFrameMs) SDL_Delay(targetFrameMs - used);
    }
#ifdef __SWITCH__
    Log(LogLevel::Info, "engine: run exit cause=%s",
        state_ == State::Quit ? "state-quit" :
        (appletAlive ? "unknown" : "applet-main-loop"));
#else
    Log(LogLevel::Info, "engine: run exit cause=state-quit");
#endif
}

// ---------------- 输入 ----------------
static bool Edge(bool* held, bool down) {
    bool e = down && !*held;
    *held = down;
    return e;
}

void Engine::TickInput() {
    SDL_Event ev;
    navX_ = navY_ = 0;
    uiPagePrev_ = uiPageNext_ = false;
    pointerPressed_ = false;
    static bool aHeld = false, bHeld = false, yHeld = false, lHeld = false,
                rHeld = false, stHeld = false;
    static int stickXDir = 0, stickYDir = 0;

#ifdef __SWITCH__
    // SDL GameController 的 A/B/X/Y 枚举表示 Xbox 式位置（南/东/西/北），
    // Switch 的实体标签则是 B/A/Y/X。游戏操作按 Nintendo 标签定义：
    // 右侧实体 A=确认，下方实体 B=取消，左侧实体 Y=自动。
    constexpr int kConfirmButton = SDL_CONTROLLER_BUTTON_B; // East  -> Switch A
    constexpr int kCancelButton  = SDL_CONTROLLER_BUTTON_A; // South -> Switch B
    constexpr int kAutoButton    = SDL_CONTROLLER_BUTTON_X; // West  -> Switch Y
#else
    constexpr int kConfirmButton = SDL_CONTROLLER_BUTTON_A;
    constexpr int kCancelButton  = SDL_CONTROLLER_BUTTON_B;
    constexpr int kAutoButton    = SDL_CONTROLLER_BUTTON_Y;
#endif

    const auto handleControllerButton = [&](int button, bool down) {
        if (down) {
            if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) navY_ = -1;
            else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) navY_ = 1;
            else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) navX_ = -1;
            else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) navX_ = 1;
            else if (button == kConfirmButton) {
                if (Edge(&aHeld, true)) clicked_ = true;
            } else if (button == kCancelButton) {
                if (Edge(&bHeld, true)) cancelClicked_ = true;
            } else if (button == kAutoButton) {
                Edge(&yHeld, true);
            } else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ||
                       button == SDL_CONTROLLER_BUTTON_BACK) {
                if (Edge(&lHeld, true)) {
                    if (ui_ == UiMode::Config || ui_ == UiMode::TitleCg ||
                        ui_ == UiMode::TitleScene || ui_ == UiMode::TitleBgm)
                        uiPagePrev_ = true;
                    else if (state_ == State::Game)
                        ui_ = ui_ == UiMode::Backlog ? UiMode::None : UiMode::Backlog;
                }
            } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                if (Edge(&rHeld, true)) {
                    if (ui_ == UiMode::Config || ui_ == UiMode::TitleCg ||
                        ui_ == UiMode::TitleScene || ui_ == UiMode::TitleBgm)
                        uiPageNext_ = true;
                    else if (state_ == State::Game)
                        skipMode_ = !skipMode_;
                }
            } else if (button == SDL_CONTROLLER_BUTTON_START) {
                if (Edge(&stHeld, true) && state_ == State::Game)
                    ui_ = ui_ == UiMode::Menu ? UiMode::None
                                              : UiMode::Menu;
            }
        } else {
            if (button == kConfirmButton) Edge(&aHeld, false);
            else if (button == kCancelButton) Edge(&bHeld, false);
            else if (button == kAutoButton) Edge(&yHeld, false);
            else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ||
                     button == SDL_CONTROLLER_BUTTON_BACK) Edge(&lHeld, false);
            else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) Edge(&rHeld, false);
            else if (button == SDL_CONTROLLER_BUTTON_START) Edge(&stHeld, false);
        }
    };

    const auto handleRawJoystickButton = [&](int button, bool down) {
#ifdef __SWITCH__
        // devkitPro SDL2 的原始按钮序号跟 SDL_CONTROLLER_BUTTON_* 不同：
        // A/B/X/Y=0..3, L/R=6/7, ZL/ZR=8/9, +=10, -=11,
        // 方向键=Left/Up/Right/Down 12..15，摇杆伪按键=16..19。
        int mapped = SDL_CONTROLLER_BUTTON_INVALID;
        // 先把 devkitPro 的 Nintendo 实体标签序号归一化为 SDL 位置枚举：
        // Switch A/B/X/Y(右/下/上/左) -> SDL B/A/Y/X(东/南/北/西)。
        if (button == 0) mapped = SDL_CONTROLLER_BUTTON_B;
        else if (button == 1) mapped = SDL_CONTROLLER_BUTTON_A;
        else if (button == 2) mapped = SDL_CONTROLLER_BUTTON_Y;
        else if (button == 3) mapped = SDL_CONTROLLER_BUTTON_X;
        else if (button == 6 || button == 8 || button == 11)
            mapped = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
        else if (button == 7 || button == 9)
            mapped = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
        else if (button == 10) mapped = SDL_CONTROLLER_BUTTON_START;
        else if (button == 12 || button == 16) mapped = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        else if (button == 13 || button == 17) mapped = SDL_CONTROLLER_BUTTON_DPAD_UP;
        else if (button == 14 || button == 18) mapped = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        else if (button == 15 || button == 19) mapped = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        if (mapped != SDL_CONTROLLER_BUTTON_INVALID) handleControllerButton(mapped, down);
#else
        // 无 GameController mapping 的 PC 通用手柄保留原来的按钮兼容路径。
        handleControllerButton(button, down);
#endif
    };

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) state_ = State::Quit;
        else if (ev.type == SDL_APP_TERMINATING) state_ = State::Quit;
        else if (ev.type == SDL_AUDIODEVICEREMOVED) {
            Log(LogLevel::Error, "audio: device disconnected: %s", SDL_GetError());
        }
        else if (ev.type == SDL_MOUSEBUTTONDOWN) {
            pointerX_ = ev.button.x; pointerY_ = ev.button.y;
            pointerPressed_ = true; clicked_ = true;
            if (!pointerHeld_) pointerHeldSince_ = SDL_GetTicks();
            pointerHeld_ = true;
        }
        else if (ev.type == SDL_MOUSEBUTTONUP) {
            pointerX_ = ev.button.x; pointerY_ = ev.button.y;
            pointerHeld_ = false; holdSkip_ = false;
        }
        else if (ev.type == SDL_FINGERDOWN) {
            pointerX_ = (int)(ev.tfinger.x * kVirtualW);
            pointerY_ = (int)(ev.tfinger.y * kVirtualH);
            pointerPressed_ = true; clicked_ = true;
            if (!pointerHeld_) pointerHeldSince_ = SDL_GetTicks();
            pointerHeld_ = true;
        }
        else if (ev.type == SDL_FINGERUP) {
            pointerX_ = (int)(ev.tfinger.x * kVirtualW);
            pointerY_ = (int)(ev.tfinger.y * kVirtualH);
            pointerHeld_ = false; holdSkip_ = false;
        }
        else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
            gfx_.OpenController(ev.cdevice.which); // ADDED 的 which 是设备索引
        } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            gfx_.CloseController(ev.cdevice.which); // REMOVED 的 which 是 instance id
            aHeld = bHeld = yHeld = lHeld = rHeld = stHeld = false;
            stickXDir = stickYDir = 0;
        } else if (ev.type == SDL_JOYDEVICEADDED) {
            if (!SDL_IsGameController(ev.jdevice.which)) gfx_.OpenController(ev.jdevice.which);
        } else if (ev.type == SDL_JOYDEVICEREMOVED) {
            gfx_.CloseController(ev.jdevice.which);
        } else if (ev.type == SDL_CONTROLLERBUTTONDOWN ||
                   ev.type == SDL_CONTROLLERBUTTONUP) {
            handleControllerButton(ev.cbutton.button, ev.type == SDL_CONTROLLERBUTTONDOWN);
        } else if (ev.type == SDL_CONTROLLERAXISMOTION &&
                   (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ||
                    ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)) {
            constexpr int kStickDeadzone = 16000;
            const int dir = ev.caxis.value < -kStickDeadzone ? -1
                          : ev.caxis.value >  kStickDeadzone ?  1 : 0;
            int& previous = ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? stickXDir : stickYDir;
            if (dir && dir != previous) {
                if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) navX_ = dir;
                else navY_ = dir;
            }
            previous = dir;
        } else if (ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_JOYBUTTONUP) {
            // GameController 同时也会投递 Joystick 事件，必须忽略重复路径。
            if (!gfx_.IsControllerInstance(ev.jbutton.which))
                handleRawJoystickButton(ev.jbutton.button, ev.type == SDL_JOYBUTTONDOWN);
        } else if (ev.type == SDL_JOYAXISMOTION &&
                   !gfx_.IsControllerInstance(ev.jaxis.which) &&
                   (ev.jaxis.axis == 0 || ev.jaxis.axis == 1)) {
            constexpr int kStickDeadzone = 16000;
            const int dir = ev.jaxis.value < -kStickDeadzone ? -1
                          : ev.jaxis.value >  kStickDeadzone ?  1 : 0;
            int& previous = ev.jaxis.axis == 0 ? stickXDir : stickYDir;
            if (dir && dir != previous) {
                if (ev.jaxis.axis == 0) navX_ = dir;
                else navY_ = dir;
            }
            previous = dir;
        } else if (ev.type == SDL_KEYDOWN) {
            // Windows 的按键自动重复会把一次回车泄漏成多次剧情点击。
            if (ev.key.repeat) continue;
            const SDL_Keycode b = ev.key.keysym.sym;
            if (b == SDLK_UP || (ui_ != UiMode::None && b == SDLK_w)) navY_ = -1;
            else if (b == SDLK_DOWN || (ui_ != UiMode::None && b == SDLK_s)) navY_ = 1;
            else if (b == SDLK_LEFT || (ui_ != UiMode::None && b == SDLK_a)) navX_ = -1;
            else if (b == SDLK_RIGHT || (ui_ != UiMode::None && b == SDLK_d)) navX_ = 1;
            else if (b == SDLK_RETURN || b == SDLK_SPACE || b == SDLK_z) { if (Edge(&aHeld, true)) clicked_ = true; }
            else if (b == SDLK_ESCAPE || b == SDLK_x) { if (Edge(&bHeld, true)) cancelClicked_ = true; }
            else if (b == SDLK_PAGEUP) uiPagePrev_ = true;
            else if (b == SDLK_PAGEDOWN || b == SDLK_e) uiPageNext_ = true;
            else if (b == SDLK_a) Edge(&yHeld, true);
            else if (b == SDLK_q) { if (Edge(&lHeld, true) && state_ == State::Game) ui_ = ui_ == UiMode::Backlog ? UiMode::None : UiMode::Backlog; }
            else if (b == SDLK_w) { if (Edge(&rHeld, true)) skipMode_ = !skipMode_; }
            else if (b == SDLK_TAB) { if (Edge(&stHeld, true) && state_ == State::Game) ui_ = ui_ == UiMode::Menu ? UiMode::None : UiMode::Menu; }
        } else if (ev.type == SDL_KEYUP) {
            const SDL_Keycode b = ev.key.keysym.sym;
            if (b == SDLK_RETURN || b == SDLK_SPACE || b == SDLK_z) Edge(&aHeld, false);
            else if (b == SDLK_ESCAPE || b == SDLK_x) Edge(&bHeld, false);
            else if (b == SDLK_a) Edge(&yHeld, false);
            else if (b == SDLK_q) Edge(&lHeld, false);
            else if (b == SDLK_w) Edge(&rHeld, false);
            else if (b == SDLK_TAB) Edge(&stHeld, false);
        }
    }
    // Y = 自动模式开关
    static bool prevAuto = false;
    if (yHeld != prevAuto) { prevAuto = yHeld; if (yHeld) { autoMode_ = !autoMode_; autoTimer_ = -1; } }
    if (video_.Playing() && clicked_) {
        if (movieSkippable_) {
            video_.Stop();
            wait_.movie = false;
        }
        clicked_ = false;
    }
    holdSkip_ = config_.longPressSkip && ui_ == UiMode::None && pointerHeld_ &&
                SDL_GetTicks() - pointerHeldSince_ >= 600u;
    // 剧情画面没有“返回层级”，避免一次 B 在随后打开菜单时变成陈旧取消事件。
    if (ui_ == UiMode::None && cancelClicked_) cancelClicked_ = false;
}

// ---------------- 动画 ----------------
void Engine::MarkAnim(float seconds) {
    if (seconds <= 0.0f) return;
    const float deadline = SDL_GetTicks() / 1000.0f + seconds;
    if (deadline > wait_.animUntil) wait_.animUntil = deadline;
    wait_.animBusy = true;
}

static int Utf8CharCount(const std::string& text) {
    int count = 0;
    for (size_t i = 0; i < text.size(); ++count) {
        const uint8_t c = (uint8_t)text[i];
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        i = std::min(i + n, text.size());
    }
    return count;
}

void Engine::UpdateAnims(float dt) {
    // 过渡
    if (trans_.active) {
        trans_.t += dt;
        if (trans_.t >= trans_.dur) {
            trans_.active = false;
            if (trans_.snap) gfx_.ReleaseSnapshot(trans_.snap);
            trans_.snap = nullptr;
        }
    }
    // 上游 AddBgMoveAnimation 的 Wait=false：移动过程中对白和脚本继续。
    if (bgMove_.active) {
        bgMove_.t = std::min(bgMove_.t + dt, bgMove_.dur);
        const float k = bgMove_.dur > 0.0f ? bgMove_.t / bgMove_.dur : 1.0f;
        bg_.x = bgMove_.fromX + (bgMove_.toX - bgMove_.fromX) * k;
        bg_.y = bgMove_.fromY + (bgMove_.toY - bgMove_.fromY) * k;
        if (bgMove_.t >= bgMove_.dur) bgMove_.active = false;
    }
    // 立绘透明度；淡出到 0 后立即释放固定位置槽，避免旧立绘永久残留。
    AdvanceCharacterVisuals(chars_, dt);
    // FB 色调：按剩余时间插值，保证不同帧率下都精确落到目标值。
    if (fb_.remaining > 0.0f) {
        float step = std::min(dt, fb_.remaining);
        float t = step / fb_.remaining;
        fb_.r += (fb_.targetR - fb_.r) * t;
        fb_.g += (fb_.targetG - fb_.g) * t;
        fb_.b += (fb_.targetB - fb_.b) * t;
        fb_.remaining -= step;
        if (fb_.remaining <= 0.0001f) {
            fb_.r = fb_.targetR;
            fb_.g = fb_.targetG;
            fb_.b = fb_.targetB;
            fb_.remaining = 0.0f;
        }
    }
    // Bmp 淡入淡出
    for (auto& [id, b] : bmps_) {
        if (b.fadePerSec > 0) {
            if (b.alpha < b.targetAlpha) b.alpha = std::min(b.targetAlpha, b.alpha + b.fadePerSec * dt);
            else if (b.alpha > b.targetAlpha) b.alpha = std::max(b.targetAlpha, b.alpha - b.fadePerSec * dt);
#ifdef WA2_DIAG_STATIC_REDRAW
            if (std::abs(b.alpha - b.targetAlpha) <= 0.0001f) b.fadePerSec = 0.0f;
#endif
        }
    }
    // 抖动
    shakeX_ = shakeY_ = 0;
    float now = SDL_GetTicks() / 1000.0f;
    if (now < shakeUntil_) {
        shakeX_ = ((int)(now * 60) % 2 == 0) ? 6 : -6;
    }
    // 等待计时
    float nowS = now;
    if (wait_.timer && nowS >= wait_.timerUntil) {
        wait_.timer = false;
        wait_.c4Timer = false;
    }
    if (wait_.animBusy && nowS >= wait_.animUntil) wait_.animBusy = false;
    // 打字机
    if (wait_.textBusy && adv_.visible) {
        // 原版 msg_wait 的四档顺序：瞬时 / 慢 / 标准 / 快。
        static const int kMessageWaitFrames[4] = {0, 4, 2, 1};
        const int speed = kMessageWaitFrames[std::clamp(config_.textSpeed, 0, 3)];
        if (speed == 0) {
            adv_.shown = Utf8CharCount(adv_.segments[adv_.seg]);
            wait_.textBusy = false;
            wait_.waitClick = true;
        } else if (SDL_GetTicks() - adv_.lastCharMs >= (uint32_t)(speed * 1000 / 60)) {
            adv_.lastCharMs = SDL_GetTicks();
            adv_.shown++;
            if (adv_.shown >= Utf8CharCount(adv_.segments[adv_.seg])) {
                wait_.textBusy = false;
                wait_.waitClick = true;
            }
        }
    }
    // 自动模式
    if (autoMode_ && state_ == State::Game && !wait_.Blocking() && adv_.visible) {
        if (autoTimer_ < 0) {
            float remain = audio_.VoiceRemaining(0);
            autoTimer_ = remain + config_.autoDelayFrames * kFrameTime;
        }
    }
    if (autoTimer_ >= 0 && (wait_.waitClick)) {
        autoTimer_ -= 1 / 60.0f;
        if (autoTimer_ < 0 && !wait_.textBusy) ClickAdvance();
    }
    // 日历
    if (wait_.calender && SDL_GetTicks() / 1000.0f >= calUntil_) {
        wait_.calender = false;
    }
    // 跳过模式:快速推进
    if ((skipMode_ || holdSkip_) && state_ == State::Game && !skipDisable_ && CanSkip()) {
        if (!wait_.textBusy) ClickAdvance();
    }
}

// ---------------- 文本推进 ----------------
void Engine::ClickAdvance() {
    clicked_ = false;
    if (ui_ != UiMode::None) return;
    if (wait_.calender) { wait_.calender = false; return; }
    if (wait_.selectVisible) return;   // 选择由按钮处理
    if (wait_.textBusy) {
        // 显示全段
        adv_.shown = Utf8CharCount(adv_.segments[adv_.seg]);
        wait_.textBusy = false;
        wait_.waitClick = true;
        return;
    }
    if (wait_.waitClick) {
        // 下一段或结束
        if (adv_.seg + 1 < (int)adv_.segments.size()) {
            adv_.seg++;
            adv_.shown = 0;
            adv_.lastCharMs = SDL_GetTicks();
            wait_.textBusy = true;
            wait_.waitClick = false;
        } else {
            wait_.waitClick = false;
            adv_.visible = adv_.hide ? false : true;
        }
        autoTimer_ = -1;
    }
}

// ---------------- 脚本节拍 ----------------
void Engine::TickScript(float dt) {
    (void)dt;
    if (ui_ != UiMode::None) return;
    // 原版“演出等待：快速”：确认键只结束当前纯时间/动画等待，不越过
    // 正在显示的对白、选项、日历或影片。动画先落到终态，避免留下半透明
    // 立绘、半完成转场或未到目标值的色调。
    if (config_.fastWait && clicked_ && !wait_.textBusy && !wait_.waitClick &&
        !wait_.selectVisible && !wait_.calender && !wait_.menu && !wait_.movie &&
        (wait_.timer || wait_.animBusy)) {
        if (wait_.animBusy) UpdateAnims(3600.0f);
        wait_.timer = wait_.c4Timer = wait_.animBusy = false;
        shakeUntil_ = 0.0f;
        clicked_ = false;
    }
#ifdef WA2_DIAG_C4_SYNC_CLICK_ADVANCE
    bool synchronousConfirmAdvance = false;
#endif
    if (wait_.Blocking()) {
        // 选项的确认/方向输入由 RenderSelect 消费，不能在脚本等待门里清掉。
        if (wait_.selectVisible) return;
#ifdef WA2_DIAG_C4_SYNC_CLICK_ADVANCE
        const bool hasNextSegment = !adv_.segments.empty() &&
            adv_.seg + 1 < (int)adv_.segments.size();
        synchronousConfirmAdvance = ShouldSynchronouslyContinueAfterConfirm(
            clicked_, wait_.textBusy, wait_.waitClick, hasNextSegment);
#endif
        // 自动/跳过/点击都汇聚到 ClickAdvance
        if ((clicked_ || skipMode_ || holdSkip_)) {
#ifdef WA2_DIAG_C4_CLICK_ADVANCE
            // 定时对白采用两段式确认：第一次完成打字机；文字已完整后的
            // 第二次确认解除 0xC4 时间轴等待，再由 ClickAdvance 进入下一句。
            if (ShouldReleaseC4ForAdvance(clicked_, wait_.c4Timer,
                                          wait_.textBusy, wait_.waitClick)) {
                wait_.timer = false;
                wait_.c4Timer = false;
            }
#endif
            if (wait_.calender) wait_.calender = false;
            else if (!wait_.selectVisible && !wait_.menu) ClickAdvance();
        }
        clicked_ = false;
#ifdef WA2_DIAG_C4_SYNC_CLICK_ADVANCE
        // 上游 ClickAdv 在最后一段 WAIT_CLICK 的同一次确认中直接调用
        // ScriptParse。若仍有其他等待门，当前点击不能越过它。
        if (!synchronousConfirmAdvance || wait_.Blocking()) return;
#else
        return;
#endif
    }
    if ((skipMode_ || holdSkip_) && CanSkip()) ClickAdvance();

    Script* s = Active();
    if (!s) {
        if (replayMode_ > 0) {
            replayMode_ = 0;
            state_ = State::Title;
            OpenSpecialMode(UiMode::TitleScene);
        } else {
            state_ = State::Title;
            ui_ = UiMode::Title;
        }
        return;
    }
    ApplyPending();
#ifdef WA2_DIAG_C4_SYNC_CLICK_ADVANCE
    // 与参考实现的 IsClick 生命周期一致：同步 ScriptParse 期间 0xCE
    // 仍应读到触发本次推进的真实确认输入。
    clicked_ = synchronousConfirmAdvance;
#else
    clicked_ = false;
#endif
    TickResult r = s->Tick(*this);
    ApplyPending();
#ifdef WA2_DIAG_C4_SYNC_CLICK_ADVANCE
    // 只放行由本次同步推进刚刚创建的 0xC4 时间轴等待。语音、SE、
    // 动画和普通 WaitMs 都不会设置 c4Timer，因此不会被这条规则误跳过。
    if (synchronousConfirmAdvance && wait_.c4Timer) {
        wait_.timer = false;
        wait_.c4Timer = false;
    }
#endif
    clicked_ = false;
    {   // TEMP PROBE: 仅在活跃脚本/栈深度变化时打印(诊断用)
        static Script* lastAct = nullptr;
        static size_t lastStk = SIZE_MAX;
        Script* cur = Active();
        if (cur != lastAct || stack_.size() != lastStk) {
            Log(LogLevel::Info, "PROBE act=%p stack=%zu graveyard=%zu r=%s",
                (void*)cur, stack_.size(), graveyard_.size(), r==TickResult::End?"End":"Wait");
            lastAct = cur; lastStk = stack_.size();
        }
    }
    if (r == TickResult::End) {
        // 弹栈;底层脚本继续或回标题(goTitle 可能已清空栈,需防空)
        if (!stack_.empty()) stack_.pop_back();
        if (stack_.empty()) {
            if (replayMode_ > 0) {
                replayMode_ = 0;
                state_ = State::Title;
                OpenSpecialMode(UiMode::TitleScene);
            } else {
                state_ = State::Title;
                ui_ = UiMode::Title;
            }
        }
    }
}

// ---------------- Host:流程 ----------------
void Engine::SLoadScript(const std::string& name, int point) {
    Log(LogLevel::Info, "PROBE SLoad enter stack=%zu graveyard=%zu flags=%zu",
        stack_.size(), graveyard_.size(), gameFlags_.size());
    for (auto& s : stack_) graveyard_.push_back(std::move(s));
    stack_.clear();
    scene_.ClearChars();
    scene_.selectItems.clear();
    ResetCharacterVisuals(chars_);
    fb_ = {};   // 0.5/0.5/0.5 是上游着色器的中性色调
    gfx_.ClearCache();   // 释放旧场景的图/立绘纹理,避免内存随推进累积
    auto s = std::make_unique<Script>();
    s->SetGameFlags(&gameFlags_);
    if (!s->Load(res_, name, point)) {
        Log(LogLevel::Error, "engine: cannot load script %s", name.c_str());
        state_ = State::Title;
        ui_ = UiMode::Title;
        return;
    }
    stack_.push_back(std::move(s));
}

void Engine::SCallScript(const std::string& name, int point) {
    auto s = std::make_unique<Script>();
    s->SetGameFlags(&gameFlags_);
    if (!s->Load(res_, name, point)) {
        Log(LogLevel::Error, "engine: cannot call script %s", name.c_str());
        return;
    }
    stack_.push_back(std::move(s));
}

void Engine::CallPoint(int point) {
    Script* s = Active();
    if (!s) return;
    // 上游 wa2-godot 的 call(point) 不是 goto，也不会清空调用栈：
    // 它把“当前脚本 + 指定入口”作为子程序压栈，子程序结束后回到调用点。
    std::string name = s->name();
    auto s2 = std::make_unique<Script>();
    s2->SetGameFlags(&gameFlags_);
    if (!s2->Load(res_, name, point)) {
        Log(LogLevel::Error, "engine: cannot call point %s:%d", name.c_str(), point);
        return;
    }
    stack_.push_back(std::move(s2));
}

void Engine::GoTitle() {
    Log(LogLevel::Info, "PROBE GoTitle stack=%zu graveyard=%zu", stack_.size(), graveyard_.size());
    const bool returnToReplay = replayMode_ > 0;
    state_ = State::Title;
    ui_ = returnToReplay ? UiMode::TitleScene : UiMode::Title;
    // 回标题彻底复位音频(BGM+SE+语音),避免多次运行间音频通道残留/腐蚀
    audio_.StopAll();
    video_.Stop();
    titleBgmStarted_ = false;
    // 延迟销毁:当前脚本还在执行其 Tick,直接 clear 会 use-after-free
    // (gotitle 是当前脚本自己触发的宿主调用)
    for (auto& s : stack_) graveyard_.push_back(std::move(s));
    stack_.clear();
    scene_.ClearChars();
    ResetCharacterVisuals(chars_);
    fb_ = {};
    gfx_.ClearCache();   // 回标题时释放当前场景纹理
    if (returnToReplay) {
        replayMode_ = 0;
        OpenSpecialMode(UiMode::TitleScene);
    }
    if (systemDirty_) SaveSystemFile();
}

void Engine::ApplyPending() {
    graveyard_.clear();
}

void Engine::PushInt(int v) {
    if (Active()) Active()->PushInt(5, 3, v);
}
void Engine::PushFloat(float v) {
    if (Active()) Active()->PushFloat(5, 4, v);
}

// ---------------- Host:旗标 ----------------
int Engine::ReadSysFlag(int idx) {
    return idx >= 0 && idx < kSysFlagCount ? sysFlags_[idx] : 0;
}
void Engine::WriteSysFlag(int idx, int v) {
    if (idx >= 0 && idx < kSysFlagCount) {
        const uint8_t value = (uint8_t)v;
        if (sysFlags_[(size_t)idx] != value) {
            sysFlags_[(size_t)idx] = value;
            systemDirty_ = true;
        }
    }
}
int Engine::ReadGameFlag(int idx) {
    return idx >= 0 && idx < (int)gameFlags_.size() ? gameFlags_[(size_t)idx] : 0;
}
void Engine::WriteGameFlag(int idx, int v) {
    if (idx >= 0 && idx < kMaxGameFlags) gameFlags_[idx] = v;
}

// ---------------- Host:文本 ----------------
// 去除 <X> 标记(字体着色等,暂不实现渲染差异)
static std::string StripMarkup(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '<') {
            size_t close = s.find('>', i);
            if (close == std::string::npos) break;
            i = close + 1;
            continue;
        }
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out.push_back('\n');
            i += 2;
            continue;
        }
        out += s[i++];
    }
    return out;
}

void Engine::ShowMessage(const std::string& text, int msgIdx, int mode, bool append) {
    (void)mode;
    std::string clean = StripMarkup(text);
    if (append) {
#ifdef WA2_DIAG_TEXT_FIT
        // 参考 Wa2Func.SetMessageEx：v3=0 会追加“\\k + text”。此前把文字
        // 直接合并到末页，既破坏点击分页，也会制造本不该存在的超长文字框。
        adv_.seg = AppendDialoguePages(clean, adv_.text, adv_.segments);
        adv_.shown = 0;
        adv_.lastCharMs = SDL_GetTicks();
        adv_.visible = true;
        adv_.hide = false;
        if (!scene_.backlog.empty()) {
            scene_.backlog.back().name = adv_.name;
            scene_.backlog.back().text = adv_.text;
        }
#else
        // 续写:\k 后接续
        if (!adv_.segments.empty()) {
            adv_.segments.back() += clean;
        }
#endif
    } else {
        const uint64_t readKey = MessageReadKey(msgIdx);
        currentMessageWasRead_ = readMessages_.find(readKey) != readMessages_.end();
        if (!currentMessageWasRead_) {
            readMessages_.insert(readKey);
            systemDirty_ = true;
            if (!config_.skipUnread) skipMode_ = false;
        }
        adv_.text = clean;
        // 按 \k 切段
        adv_.segments.clear();
        size_t start = 0;
        while (true) {
            size_t p = clean.find("\\k", start);
            if (p == std::string::npos) {
                adv_.segments.push_back(clean.substr(start));
                break;
            }
            adv_.segments.push_back(clean.substr(start, p - start));
            start = p + 2;
        }
        adv_.seg = 0;
        adv_.shown = 0;
        adv_.lastCharMs = SDL_GetTicks();
        adv_.visible = true;
        adv_.hide = false;
        // backlog
        if (!adv_.name.empty() || !clean.empty())
            scene_.AddBacklog(adv_.name, clean);
    }
    wait_.textBusy = true;
    wait_.waitClick = false;
}

void Engine::EndMessage() {
    if (config_.pageVoice) audio_.StopVoice(0, 0);
    adv_.name.clear();
}

void Engine::SetName(const std::string& name) {
    adv_.name = name;
}

void Engine::WaitClick() {
    wait_.waitClick = true;
}

void Engine::HideWindow(int fadeFrames) {
    adv_.hide = true;
    MarkAnim(fadeFrames * kFrameTime);
}

void Engine::SetNovelMode(bool v) { scene_.novelMode = v; }
void Engine::SetDemoMode(bool v) { demoMode_ = v; scene_.demoMode = v; }
void Engine::SetSkipDisable(bool v) { skipDisable_ = v; }
void Engine::StopSkip() { skipMode_ = false; }

// ---------------- Host:画面 ----------------
void Engine::SetupNewBg(const std::string& path, int frame, int x, int y, int offset,
                        float sx, float sy, bool keepChar) {
    std::string oldPath = bg_.path;
    Log(LogLevel::Info, "engine: setup bg '%s' trans=%d", path.c_str(), frame);
    bg_.path = path;
    bg_.x = (float)(x - offset);
    bg_.y = (float)y;
    bg_.sx = sx > 0 ? sx : 1;
    bg_.sy = sy > 0 ? sy : 1;
    bgMove_.active = false;

    // 0 帧切换直接落到新背景；有时长时截取上一帧并交叉淡化。
    if (frame <= 0) {
        trans_.active = false;
        if (trans_.snap) gfx_.ReleaseSnapshot(trans_.snap);
        trans_.snap = nullptr;
        MarkAnim(0.0f);
        return;
    }

    trans_.snap = gfx_.CaptureScreen();
    trans_.active = true;
    trans_.t = 0;
    trans_.dur = frame * kFrameTime;
    const float secs = trans_.dur;
    MarkAnim(secs);
    (void)oldPath;
    (void)keepChar;
}

bool Engine::NeedsContinuousRedraw() const {
    if (video_.Playing() || trans_.active || bgMove_.active ||
        fb_.remaining > 0.0001f || wait_.textBusy ||
        (weather_.active && weather_.count > 0) ||
        SDL_GetTicks() / 1000.0f < shakeUntil_)
        return true;
    for (const auto& c : chars_)
        if (c.show && std::abs(c.alpha - c.targetAlpha) > 0.0001f) return true;
    for (const auto& entry : bmps_)
        if (std::abs(entry.second.alpha - entry.second.targetAlpha) > 0.0001f)
            return true;
    return false;
}

void Engine::RenderImage(int id, int efc, bool keepChar, int type, int frame,
                         int offset, int x, int y, float sx, float sy) {
    std::string path;
    if (id >= 0) {
        if (type == 1) {
            path = Res::CgName(id);
            UnlockCg(id);
        } else if (type == 2) {
            path = Res::HName(id);
        } else {
            path = Res::BgName(id, timeMode_);
        }
        if (!res_.Exists(path)) {
            Log(LogLevel::Warn, "engine: bg missing %s", path.c_str());
        }
    } else {
        path = bg_.path;   // 保持当前
    }
    scene_.bg.path = path;
    if (id >= 0) scene_.bg.id = id;
    scene_.bg.x = x;
    scene_.bg.y = y;
    scene_.bg.offset = offset;
    scene_.bg.sx = sx > 0 ? sx : 1.0f;
    scene_.bg.sy = sy > 0 ? sy : 1.0f;
    scene_.bg.type = type;
    SetupNewBg(path, frame, x, y, offset, sx, sy, keepChar);
    // 参考实现每次 B/V 都清空期望角色列表；BC 则把 CW/CRW 排队的
    // 列表与背景过渡一起提交。两条路径都必须同步画面槽位。
    if (!keepChar) scene_.ClearChars();
    UpdateChar(frame);
}

void Engine::AddChar(int id, int no, int pos) {
    if (pos < 0 || pos >= kMaxChars) {
        Log(LogLevel::Warn, "engine: ignored character id=%d with invalid pos=%d", id, pos);
        return;
    }
    scene_.AddOrUpdateChar(id, no, pos);
}

void Engine::UpdateChar(int frames) {
    if (frames > 300) Log(LogLevel::Warn, "engine: unusually long char animation: %d frames", frames);
    const float secs = std::max(0, frames) * kFrameTime;
    CommitCharacterVisuals(scene_, chars_, frames);
    MarkAnim(secs);
}

void Engine::RemoveChar(int id) {
    scene_.RemoveCharById(id);
}

void Engine::BgMove(int dx, int dy, int frames) {
    bgMove_.fromX = bg_.x;
    bgMove_.fromY = bg_.y;
    bgMove_.toX = (float)dx;
    bgMove_.toY = (float)dy;
    bgMove_.t = 0.0f;
    bgMove_.dur = std::max(0, frames) * kFrameTime;
    bgMove_.active = bgMove_.dur > 0.0f;
    if (!bgMove_.active) { bg_.x = bgMove_.toX; bg_.y = bgMove_.toY; }
}

void Engine::WaitBgMove() {
    if (bgMove_.active) MarkAnim(std::max(0.0f, bgMove_.dur - bgMove_.t));
}

void Engine::StopBgMove() {
    if (!bgMove_.active) return;
    bg_.x = bgMove_.toX;
    bg_.y = bgMove_.toY;
    bgMove_.active = false;
}

void Engine::ColorFade(int r, int g, int b, int frames) {
    Log(LogLevel::Info, "engine: FB to rgb=(%d,%d,%d) frames=%d", r, g, b, frames);
    fb_.targetR = std::clamp(r / 255.0f, 0.0f, 1.0f);
    fb_.targetG = std::clamp(g / 255.0f, 0.0f, 1.0f);
    fb_.targetB = std::clamp(b / 255.0f, 0.0f, 1.0f);
    if (frames > 0) {
        fb_.remaining = frames * kFrameTime;
        MarkAnim(fb_.remaining);
    } else {
        fb_.r = fb_.targetR;
        fb_.g = fb_.targetG;
        fb_.b = fb_.targetB;
        fb_.remaining = 0.0f;
    }
}

void Engine::ColorFadeFrom(int r, int g, int b, int frames) {
    Log(LogLevel::Info, "engine: F from rgb=(%d,%d,%d) frames=%d", r, g, b, frames);
    const float cr = std::clamp(r / 255.0f, 0.0f, 1.0f);
    const float cg = std::clamp(g / 255.0f, 0.0f, 1.0f);
    const float cb = std::clamp(b / 255.0f, 0.0f, 1.0f);
    if (frames > 0) {
        // 上游 AddFAnimation：从参数颜色回到 fb 的中性值 0.5。
        fb_.r = cr; fb_.g = cg; fb_.b = cb;
        fb_.targetR = fb_.targetG = fb_.targetB = 0.5f;
        fb_.remaining = frames * kFrameTime;
        MarkAnim(fb_.remaining);
    } else {
        SetFBColor(r, g, b);
    }
}

void Engine::SetFBColor(int r, int g, int b) {
    fb_.r = fb_.targetR = std::clamp(r / 255.0f, 0.0f, 1.0f);
    fb_.g = fb_.targetG = std::clamp(g / 255.0f, 0.0f, 1.0f);
    fb_.b = fb_.targetB = std::clamp(b / 255.0f, 0.0f, 1.0f);
    fb_.remaining = 0.0f;
}

void Engine::SetWeather(int flag, int speedX, int speedY, int turbulence,
                        int count, int flag2, int index) {
    weather_.active = count > 0;
    weather_.flag = flag;
    weather_.speedX = speedX;
    weather_.speedY = speedY;
    weather_.turbulence = turbulence;
    weather_.count = std::max(0, count);
    weather_.flag2 = flag2;
    weather_.index = index;
    weather_.startedMs = SDL_GetTicks();
    Log(LogLevel::Info,
        "engine: weather set type=%d flag=0x%x speed=(%d,%d) count=%d index=%d turbulence=%d",
        flag & 0xff, flag, speedX, speedY, count, index, turbulence);
}

void Engine::ChangeWeather(int speedX, int speedY, int count, int turbulence, int index) {
    if (!weather_.active) return;
    if (speedX != -1000) weather_.speedX = speedX;
    if (speedY != -1000) weather_.speedY = speedY;
    if (count != -1000) {
        weather_.count = std::max(0, count);
        weather_.active = weather_.count > 0;
    }
    if (turbulence != -1000) weather_.turbulence = turbulence;
    if (index != -1000) weather_.index = index;
}

void Engine::ResetWeather() {
    weather_ = {};
}

void Engine::Shake(int type, int power, int frames) {
    if (frames > 300) Log(LogLevel::Warn, "engine: unusually long shake: %d frames", frames);
    (void)type; (void)power;
    shakeUntil_ = SDL_GetTicks() / 1000.0f + frames * kFrameTime;
    MarkAnim(frames * kFrameTime);
}

void Engine::ShowCalender(int y, int m, int d, int dow) {
    calY_ = y; calM_ = m; calD_ = d; calDow_ = dow;
    wait_.calender = true;
    calUntil_ = SDL_GetTicks() / 1000.0f + 2.0f;
}

void Engine::PlayMovie(int movieId, int flagIdx) {
    // wa2-godot 的映射：00/10..24 在根目录，01/02/07/08/09 在 IC。
    std::string rel;
    if (movieId == 1 || movieId == 2)
        rel = Format("IC/mv%02d0.pak", movieId);
    else if (movieId == 7 || movieId == 8 || movieId == 9)
        rel = Format("IC/MV%02d0.pak", movieId);
    else
        rel = Format("mv%02d0.pak", movieId);
    const std::string path = PathJoin(dataDir_, rel);

    movieSkippable_ = flagIdx >= 0 && ReadSysFlag(flagIdx) == 1;
    if (flagIdx >= 0) WriteSysFlag(flagIdx, 1);
    skipMode_ = false;
    audio_.StopAll();
    const int movieVolume = (config_.masterVolume * config_.bgmVolume + 127) / 255;
    if (FileExists(path) && video_.Play(path, movieVolume)) {
        wait_.movie = true;
        Log(LogLevel::Info, "engine: movie %d -> %s (skip=%d)",
            movieId, path.c_str(), (int)movieSkippable_);
    } else {
        wait_.movie = false;
        Log(LogLevel::Warn, "engine: movie %d unavailable: %s", movieId, path.c_str());
    }
}

int Engine::TimeMode() const { return timeMode_; }
void Engine::SetTimeMode(int v) { timeMode_ = v; scene_.timeMode = v; }
void Engine::SetEffectMode(const std::string& file) { effectMode_ = file; scene_.effectMode = file; }
void Engine::SetEroMode(bool v) { scene_.eroMode = v; }

bool Engine::CanSkip() const {
    return !skipDisable_ && (config_.skipUnread || currentMessageWasRead_);
}

bool Engine::Clicked() const { return clicked_; }

// ---------------- Host:选项 ----------------
void Engine::AddSelectItem(const std::string& text, int v1, int v2, int v3) {
    scene_.selectItems.push_back({StripMarkup(text), v1, v2, v3});
}

void Engine::ShowSelect() {
    wait_.selectVisible = true;
    selectCursor_ = 0;
    // 原版把每个选择点的“已读”位存到 900 起的系统旗标区。
    // 基址由脚本编号在 SelectScript 表中的位置和 SetSelect 的参数共同决定。
    static const int kSelectScripts[] = {
        2003, 2004, 2005, 2007, 2009, 2011, 2013, 2014, 2015, 2016,
        2017, 2018, 2019, 2024, 2503, 3003, 3005, 3009, 3012, 3013,
        3014, 3016, 3904
    };
    int tableIndex = -1;
    Script* s = Active();
    const int scriptNo = s ? std::atoi(s->name().c_str()) : 0;
    for (int i = 0; i < (int)(sizeof(kSelectScripts) / sizeof(kSelectScripts[0])); ++i) {
        if (kSelectScripts[i] == scriptNo) { tableIndex = i; break; }
    }
    int subIndex = 0;
    if (s && !s->args.empty()) subIndex = (int)s->GetVar(s->args.back()).AsInt();
    selectBase_ = 900 + tableIndex * 4 + subIndex;

    // 跳过脚本标记为不可用的选项。
    for (int i = 0; i < (int)scene_.selectItems.size(); ++i) {
        if (scene_.selectItems[i].v2 == ReadSysFlag(scene_.selectItems[i].v1)) {
            selectCursor_ = i;
            break;
        }
    }
}

// ---------------- Host:音频 ----------------
void Engine::PlayBgm(int id, bool loop, int vol) {
    if (id >= 0 && audio_.PlayBgm(id, loop, vol, res_)) WriteSysFlag(100 + id, 1);
}
void Engine::StopBgm(int fadeFrames) {
    audio_.StopBgm((int)(fadeFrames * kFrameTime * 1000));
}
void Engine::SetVoiceLabel(int label) { voiceLabel_ = label; }
int Engine::CurrentVoiceLabel() const { return voiceLabel_; }
void Engine::PlayVoice(int label, int id, int chr, int volume, bool loop, int channel) {
    if (channel == 0) {
        const int group = VoicePreferenceGroup(chr);
        if (group >= 0 && !config_.charVoice[(size_t)group]) return;
        const bool mainEroCharacter = chr >= 1 && chr <= 5;
        if (scene_.eroMode && config_.eroVoice && !mainEroCharacter) return;
    }
    audio_.PlayVoice(label, id, chr, channel, volume, loop, res_);
}
void Engine::WaitVoice(int ch) {
    float remain = audio_.VoiceRemaining(ch);
    if (remain > 0) {
        wait_.timer = true;
        wait_.c4Timer = false;
        wait_.timerUntil = SDL_GetTicks() / 1000.0f + remain;
    }
}
void Engine::StopVoice(int fadeMs, int ch) { audio_.StopVoice(fadeMs, ch); }
void Engine::SetVoiceVolume(int ch, int vol, int frames) {
    (void)frames;
    audio_.SetVoiceVolume(ch, vol, 0);
}
void Engine::PlaySe(int ch, int id, bool loop, int fadeInMs, int vol) {
    audio_.PlaySe(ch, id, loop, (int)(fadeInMs * kFrameTime * 1000), vol, res_);
}
void Engine::StopSe(int ch, int fadeMs) {
    audio_.StopSe(ch, (int)(fadeMs * kFrameTime * 1000));
}
void Engine::SetSeVolume(int ch, int vol, int frames) {
    (void)frames;
    audio_.SetSeVolume(ch, vol, 0);
}
void Engine::WaitSe(int ch) {
    float remain = audio_.SeRemaining(ch);
    if (remain > 0) {
        wait_.timer = true;
        wait_.c4Timer = false;
        wait_.timerUntil = SDL_GetTicks() / 1000.0f + remain;
    }
}

// ---------------- Host:定时 ----------------
void Engine::WaitMs(float ms) {
    wait_.timer = true;
    wait_.c4Timer = false;
    wait_.timerUntil = SDL_GetTicks() / 1000.0f + ms / 1000.0f;
}
void Engine::StartTimer() { timerStart_ = SDL_GetTicks(); }
int Engine::ElapsedTimerMs() { return (int)(SDL_GetTicks() - timerStart_); }
void Engine::WaitUntilTimerMs(float targetMs) {
    const float remainingMs = targetMs - (float)ElapsedTimerMs();
    wait_.c4Timer = false;
    if (remainingMs > 0.0f) {
        wait_.timer = true;
        wait_.c4Timer = true;
        wait_.timerUntil = SDL_GetTicks() / 1000.0f + remainingMs / 1000.0f;
    }
}

// ---------------- Host:Bmp ----------------
void Engine::LoadBmp(int id, const std::string& path, int z) {
    bmps_[id] = {ToLower(path), z, 1, 1, 0};
}
void Engine::ReleaseBmp(int id) { bmps_.erase(id); }
void Engine::SetBmpParam(int id, int mode, int alpha, int frames) {
    if (frames > 300) Log(LogLevel::Warn, "engine: unusually long bmp animation: id=%d frames=%d", id, frames);
    (void)mode;
    auto it = bmps_.find(id);
    if (it == bmps_.end()) return;
    it->second.targetAlpha = alpha / 255.0f;
    it->second.fadePerSec = frames > 0 ? 1.0f / (frames * kFrameTime) : 0;
    if (frames > 0) MarkAnim(frames * kFrameTime);
}

// ---------------- 渲染 ----------------
void Engine::Render() {
    gfx_.Clear();
    int ox = shakeX_, oy = shakeY_;

    if (video_.Playing()) {
        video_.Render();
        return;
    }

    if (state_ == State::Logo) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0, 255);
        return;
    }

    // 背景(或过渡)
    Tex* bgTex = bg_.path.empty() ? nullptr : gfx_.Get(bg_.path, res_, effectMode_);
    if (bgTex) {
        gfx_.DrawTextureFit(bgTex, kVirtualW / 2.0f + bg_.x, kVirtualH / 2.0f + bg_.y,
                            bg_.sx, bg_.sy, 1.0f);
    }
    if (trans_.active) {
        float a = 1.0f - trans_.t / (trans_.dur > 0 ? trans_.dur : 0.01f);
        if (trans_.snap) {
            SDL_Rect dst{0, 0, kVirtualW, kVirtualH};
            SDL_SetTextureAlphaMod(trans_.snap, (uint8_t)(a * 255));
            SDL_RenderCopy(gfx_.renderer(), trans_.snap, nullptr, &dst);
        } else {
            // Switch 禁用昂贵的 framebuffer readback 后，以黑色淡出旧画面、
            // 淡入新背景。保留原脚本时长和等待语义，不产生临时大纹理。
            gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0,
                          (uint8_t)std::clamp(a * 255.0f, 0.0f, 255.0f));
        }
    }

    RenderWeather(false);

    // 立绘(按 kCharOrder 远→近)
    for (int oi = 0; oi < kMaxChars; oi++) {
        CharDraw& c = chars_[kCharOrder[oi]];
        if (!c.show || c.alpha <= 0) continue;
        // TEMP PROBE: kCharPos 越界检测(防垃圾坐标传给 SDL)
        if (c.pos < 0 || c.pos >= kMaxChars) {
            Log(LogLevel::Error, "GFX char pos OOB c.pos=%d id=%d no=%d", c.pos, c.id, c.no);
            continue;
        }
        std::string path = Res::CharName(c.id, c.no);
        Tex* t = gfx_.Get(path, res_, effectMode_);
        if (!t) continue;
        int x = kVirtualW / 2 + kCharPos[c.pos] - t->w / 2;
        int y = kVirtualH - t->h;
        gfx_.DrawTexture(t, x + ox, y + oy, 0, 0, c.alpha);
    }

    // Bmp 自由图层
    for (auto& [id, b] : bmps_) {
        Tex* t = gfx_.Get(b.path, res_, "");
        if (t) gfx_.DrawTexture(t, ox, oy, 0, 0, b.alpha);
    }

    RenderWeather(true);

    // 上游 shader：以 0.5 为中性，向 0/1 偏移时混合到黑/白。
    // SDL2 无全局 shader，这里用等价的 alpha 色层近似；对 WA2 常用灰阶淡入淡出语义一致。
    const float fr = fb_.r - 0.5f, fg = fb_.g - 0.5f, fb = fb_.b - 0.5f;
    const float bias = std::clamp(std::sqrt(fr * fr + fg * fg + fb * fb) * 2.0f, 0.0f, 1.0f);
    if (bias > 0.001f) {
        gfx_.FillRect(ox, oy, kVirtualW, kVirtualH,
                      fb_.r >= 0.5f ? 255 : 0,
                      fb_.g >= 0.5f ? 255 : 0,
                      fb_.b >= 0.5f ? 255 : 0,
                      (uint8_t)(bias * 255.0f));
    }

    // 游戏内 UI
    if (state_ == State::Game) {
        RenderAdvWindow();
        RenderSelect();
        RenderCalender();
    }
    RenderUi();
    (void)oy;
}

static float WrapCoord(float value, float span) {
    value = std::fmod(value, span);
    return value < 0.0f ? value + span : value;
}

void Engine::RenderWeather(bool frontLayer) {
    if (!weather_.active || weather_.count <= 0) return;
    const bool front = weather_.index != 0;
    if (front != frontLayer) return;

    // Switch 软件渲染下限制单帧 draw call；位置由稳定哈希和时间直接计算，无堆分配。
    const int count = std::min(weather_.count, 256);
    const int type = weather_.flag & 0xff;
    const float elapsed = (SDL_GetTicks() - weather_.startedMs) / 1000.0f;
    for (int i = 0; i < count; ++i) {
        uint32_t h = (uint32_t)i * 747796405u + 2891336453u;
        h ^= h >> 16; h *= 2246822519u; h ^= h >> 13;
        const float baseX = (float)(h % (kVirtualW + 64)) - 32.0f;
        const float baseY = (float)((h >> 11) % (kVirtualH + 96)) - 48.0f;
        const float scale = 0.45f + ((h >> 23) & 0xff) / 255.0f;
        const float drift = std::sin(elapsed * (0.7f + scale) + i * 0.71f) *
                            (6.0f + std::abs(weather_.turbulence) * 0.04f);
        const float x = WrapCoord(baseX + elapsed * weather_.speedX * scale * 0.10f + drift + 32.0f,
                                  kVirtualW + 64.0f) - 32.0f;
        const float y = WrapCoord(baseY + elapsed * weather_.speedY * scale * 0.10f + 48.0f,
                                  kVirtualH + 96.0f) - 48.0f;

        if (type == 0) { // rain
            gfx_.FillRect((int)x, (int)y, 2, 10 + (int)(scale * 8.0f), 190, 210, 235, 150);
        } else if (type == 1) { // sakura/petals
            gfx_.FillRect((int)x, (int)y, 4 + (int)(scale * 3.0f), 3, 255, 205, 220, 205);
        } else { // snow and the remaining particle modes
            const int size = 2 + (int)(scale * 3.0f);
            const uint8_t alpha = (uint8_t)std::min(235, 120 + (int)(scale * 90.0f));
            gfx_.FillRect((int)x, (int)y, size, size, 245, 248, 255, alpha);
        }
    }
}

void Engine::RenderAdvWindow() {
    if (!adv_.visible || adv_.hide) return;
    if (scene_.novelMode) {
        // 小说模式:无框居中
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0,
                      (uint8_t)std::clamp(config_.novelWindowAlpha, 0, 256));
        const std::string& seg = adv_.segments.empty() ? adv_.text : adv_.segments[adv_.seg];
#ifdef WA2_DIAG_TEXT_FIT
        // 参考 Wa2AdvMain/Wa2Label：小说模式 28px、39 字/行。旧实现完全
        // 不自动换行，长段落会直接横穿屏幕。
        if (adv_.layoutText != seg || !adv_.layoutNovelMode) {
            adv_.layout = FitDialogueText(seg, kNovelTextWidth, kNovelTextHeight);
            adv_.layoutText = seg;
            adv_.layoutNovelMode = true;
            if (!adv_.layout.fits) {
                Log(LogLevel::Warn,
                    "novel: text exceeds adaptive 16px layout (%d chars, %zu lines)",
                    adv_.layout.rawCharCount, adv_.layout.lines.size());
            }
        }
        const int maxChars = wait_.textBusy ? adv_.shown : -1;
        const int size = adv_.layout.fontSize;
        const int shadowPad = std::max(1, size * 2 / 28);
        int y = kNovelTextY;
        for (const DialogueTextLine& line : adv_.layout.lines) {
            if (y + size + shadowPad > kNovelTextSafeBottom) break;
            const int avail = DialogueLineVisibleChars(line, maxChars);
            if (avail <= 0) break;
            gfx_.DrawTextTyped(line.text, kNovelTextX, y, size, avail, 255, 255, 255);
            y += adv_.layout.lineAdvance;
        }
#else
        gfx_.DrawTextTyped(seg, 120, 80, kTextSize, adv_.shown, 255, 255, 255);
#endif
        return;
    }
    // 原版窗口由两层 sys_ 图组成，而不是临时黑色矩形。
    Tex* window = gfx_.Get("sys_00001.tga", res_, "");
    Tex* frame = gfx_.Get("sys_00000.tga", res_, "");
    const int configuredAlpha = scene_.bg.type == 1
        ? config_.cgWindowAlpha : config_.windowAlpha;
    const float windowAlpha = std::clamp(configuredAlpha / 256.0f, 0.0f, 1.0f);
    if (window) gfx_.DrawTexture(window, kWinX, kWinY, kWinW, kWinH, windowAlpha);
    if (frame) gfx_.DrawTexture(frame, kWinX, kWinY, kWinW, kWinH, 1.0f);
    if (!window && !frame)
        gfx_.FillRect(kWinX, kWinY, kWinW, kWinH, 0, 22, 34,
                      (uint8_t)std::clamp(configuredAlpha, 0, 255));
    // 名字
    if (!adv_.name.empty()) {
        gfx_.DrawText(adv_.name, kNameX, kNameY, kNameSize, 255, 255, 255);
    }
    // 正文(逐字)
    const std::string& seg = adv_.segments.empty() ? adv_.text : adv_.segments[adv_.seg];
    int maxChars = wait_.textBusy ? adv_.shown : -1;
#ifdef WA2_DIAG_TEXT_FIT
    if (adv_.layoutText != seg || adv_.layoutNovelMode) {
        // 必须按完整段落选字号；若按当前 shown 重排，逐字显示时文字会跳行。
#ifdef WA2_DIAG_TEXT_REFLOW
        // 普通对白按 DrawTextTyped 实际采用的字形 advance 累计。
#ifdef WA2_RELEASE_BUILD
        // 正式版采用实机最终确认的 x=978 右边界（比 F5 多一个全角字格）。
#elif defined(WA2_DIAG_TEXT_SNOW_SAFE)
        // F5 把正文止于 x=950，确保字形及阴影不碰右侧雪花装饰。
#elif defined(WA2_DIAG_TEXT_SAFE_WIDTH)
        // F4 恢复参考 UI 的 784px 安全区，只保留 F3 已验证方向正确的软换行。
#else
        // F3 的 920px 范围已被实机证实过宽，仅为历史产物保留。
#endif
        // 脚本单换行是旧 28 字格的排版提示，允许重新流排；连续换行保留
        // 为段落边界。rawCharIndices 确保软换行不破坏逐字显示进度。
        auto measureGlyph = [this](const std::string& glyph, int size) {
            // DrawTextTyped 对 ASCII 空格使用半角 advance；其余路径与
            // TextWidth 一致。这里必须复刻绘制语义，不能只按字符数估算。
            if (glyph == " ") return size / 2;
            return gfx_.TextWidth(glyph, size);
        };
        adv_.layout = FitDialogueTextMeasured(
            seg, kAdvTextWidth, kAdvTextHeight,
            DialogueNewlinePolicy::ReflowSingle, measureGlyph);
#else
        adv_.layout = FitDialogueText(seg, kAdvTextWidth, kAdvTextHeight);
#endif
        adv_.layoutText = seg;
        adv_.layoutNovelMode = false;
        if (!adv_.layout.fits) {
            Log(LogLevel::Warn,
                "dialogue: text exceeds adaptive 16px layout (%d chars, %zu lines)",
                adv_.layout.rawCharCount, adv_.layout.lines.size());
        }
    }
    const int size = adv_.layout.fontSize;
    const int shadowPad = std::max(1, size * 2 / 28);
    int y = kTextY;
    for (const DialogueTextLine& line : adv_.layout.lines) {
        // 即便遇到异常超长文本，任何字形及其阴影都不能画出文字框安全区。
        if (y + size + shadowPad > kAdvTextSafeBottom) break;
        const int avail = DialogueLineVisibleChars(line, maxChars);
        if (avail <= 0) break;
        gfx_.DrawTextTyped(line.text, kTextX, y, size, avail, 255, 255, 255);
        y += adv_.layout.lineAdvance;
    }
#else
    // 简单折行:手动按宽度断行
    int x = kTextX, y = kTextY;
    int lineWidth = 920;
    int drawn = 0;
    size_t i = 0;
    while (i < seg.size()) {
        // 找出能放进一行的最大字符数
        size_t j = i;
        int w = 0;
        int lineChars = 0;
        bool explicitBreak = false;
        size_t lineEnd = seg.size();
        for (; j < seg.size();) {
            if (seg[j] == '\n') {
                lineEnd = j;
                explicitBreak = true;
                break;
            }
            size_t next = j;
            uint32_t cp = 0;
            // 简易 UTF-8 步进
            uint8_t c = (uint8_t)seg[j];
            int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            next = std::min(j + n, seg.size());
            (void)cp;
            w += kTextSize;   // 全角宽度近似
            if (w > lineWidth) { lineEnd = j; break; }
            j = next;
            lineChars++;
        }
        int avail = maxChars < 0 ? lineChars : std::min(lineChars, maxChars - drawn);
        if (avail <= 0) break;
        std::string part = seg.substr(i, lineEnd - i);
        int n = gfx_.DrawTextTyped(part, x, y, kTextSize, avail, 255, 255, 255);
        drawn += n;
        if (n < lineChars) break;
        if (explicitBreak) {
            if (maxChars >= 0 && drawn >= maxChars) break;
            drawn++; // 换行控制符也占用一个打字机进度
            i = lineEnd + 1;
            y += gfx_.LineHeight(kTextSize);
            if (y > kWinY + kWinH - 20) break;
        } else if (lineEnd < seg.size()) {
            i = lineEnd;
            y += gfx_.LineHeight(kTextSize);
            if (y > kWinY + kWinH - 20) break;
        } else {
            break;
        }
    }
#endif
    // 下一段指示
    if (!wait_.textBusy && adv_.seg + 1 < (int)adv_.segments.size()) {
        gfx_.FillRect(kWinX + kWinW - 38, kWinY + kWinH - 32, 12, 12, 150, 230, 255, 220);
    }
}

void Engine::RenderSelect() {
    if (!wait_.selectVisible) return;
    int n = (int)scene_.selectItems.size();
    if (n == 0) { wait_.selectVisible = false; return; }
    int boxW = 560, boxH = 64;
    int y0 = kVirtualH / 2 - n * (boxH + 12) / 2;
    for (int i = 0; i < n; i++) {
        int y = y0 + i * (boxH + 12);
        bool cur = i == selectCursor_;
        bool enabled = scene_.selectItems[i].v2 == ReadSysFlag(scene_.selectItems[i].v1);
        bool read = selectBase_ >= 0 && (ReadSysFlag(selectBase_) & (1 << i)) != 0;
        gfx_.FillRect(kVirtualW / 2 - boxW / 2, y, boxW, boxH,
                      cur && enabled ? 60 : 20, cur && enabled ? 60 : 20,
                      cur && enabled ? 100 : 30, 220);
        gfx_.DrawText(scene_.selectItems[i].text,
                      kVirtualW / 2 - boxW / 2 + 24, y + 16, 28,
                      enabled ? (cur ? 255 : 200) : 100,
                      enabled ? (cur ? 230 : 200) : 100,
                      enabled ? (cur ? 180 : 200) : 100);
        if (read) gfx_.DrawText("已读", kVirtualW / 2 + boxW / 2 - 72,
                                y + 20, 20, 150, 190, 230);
    }
    // 选择操作：方向和确认都使用 TickInput 产生的单次事件，Joy-Con 同样有效。
    auto enabled = [this](int i) {
        return scene_.selectItems[i].v2 == ReadSysFlag(scene_.selectItems[i].v1);
    };
    if (navY_ != 0) {
        int dir = navY_ < 0 ? -1 : 1;
        for (int tries = 0; tries < n; ++tries) {
            selectCursor_ = (selectCursor_ + dir + n) % n;
            if (enabled(selectCursor_)) break;
        }
    }
    navY_ = 0;
    if (clicked_ && enabled(selectCursor_)) {
        clicked_ = false;
        Script* s = Active();
        if (s && !s->args.empty()) {
            Val v;
            v.kind = Val::Int;
            v.i = selectCursor_;
            s->SetVar(s->args.back(), v);
            // 已读标记(sys flag)
            if (selectBase_ >= 0)
                WriteSysFlag(selectBase_, ReadSysFlag(selectBase_) | (1 << selectCursor_));
        }
        scene_.selectItems.clear();
        wait_.selectVisible = false;
    }
}

void Engine::RenderCalender() {
    if (!wait_.calender) return;
    gfx_.FillRect(kVirtualW / 2 - 220, kVirtualH / 2 - 80, 440, 160, 20, 20, 40, 230);
    std::string text = Format("%d 年 %d 月 %d 日", calY_, calM_, calD_);
    gfx_.DrawText(text, kVirtualW / 2 - 120, kVirtualH / 2 - 40, 36, 255, 255, 255);
}

void Engine::RenderTitleBackdrop() {
    // 坐标和图集切片来自上游 wa2-godot/scene/title_menu.tscn。
    // T0000 高 856px，最终标题构图显示 y=136..855 的 720p 区域。
    Tex* bg = gfx_.Get("t0000.tga", res_, "");
    if (bg) {
        if (bg->h >= 856)
            gfx_.DrawTextureRegion(bg, 0, bg->h - kVirtualH, std::min(bg->w, kVirtualW), kVirtualH,
                                   0, 0, kVirtualW, kVirtualH, 1.0f);
        else
            gfx_.DrawTexture(bg, 0, 0, kVirtualW, kVirtualH, 1.0f);
    } else {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 8, 24, 40, 255);
    }

    Tex* logo = gfx_.Get("t0011.tga", res_, "");
    if (logo) gfx_.DrawTexture(logo, 264, 1, 752, 448, 1.0f);
    Tex* copyright = gfx_.Get("t0020.tga", res_, "");
    if (copyright) gfx_.DrawTexture(copyright, 998, 678, 264, 24, 1.0f);
}

void Engine::StartChapter(const std::string& scriptName) {
    audio_.StopAll();
    video_.Stop();
    // 标题菜单开始任一篇章都属于新游戏；篇章内部的 SLoad 会继续沿用这组旗标。
    gameFlags_.assign(kMaxGameFlags, 0);
    titleBgmStarted_ = false;
    clicked_ = false;
    cancelClicked_ = false;
    wait_.Clear();
    adv_ = {};
    if (scriptName != "9999") replayMode_ = 0;
    startScript_ = scriptName;
    state_ = State::Game;
    ui_ = UiMode::None;
    SLoadScript(scriptName, 0);
}

void Engine::StartReplay(int slot) {
    if (slot < 0 || slot >= (int)SceneReplaySlots().size()) return;
    replayMode_ = slot + 1;
    StartChapter("9999");
}

bool Engine::PointerIn(int x, int y, int w, int h) const {
    return pointerPressed_ && pointerX_ >= x && pointerX_ < x + w &&
           pointerY_ >= y && pointerY_ < y + h;
}

void Engine::ConsumeGridInput(int count, int columns) {
    if (count <= 0) return;
    uiCursor_ = MoveGridCursor(uiCursor_, count, columns, navX_, navY_);
    navX_ = navY_ = 0;
}

void Engine::OpenSpecialMode(UiMode mode) {
    state_ = State::Title;
    ui_ = mode;
    uiCursor_ = 0;
    uiPage_ = 0;
    uiScroll_ = 0;
    clicked_ = cancelClicked_ = false;
    titleBgmStarted_ = true;
    if (mode == UiMode::TitleCg) {
        PlayBgm(41, true, 255);
    } else if (mode == UiMode::TitleScene) {
        PlayBgm(15, true, 255);
    }
}

void Engine::CloseSpecialMode() {
    audio_.StopVoice(0, 0);
    voiceMessagePlaying_ = -1;
    ui_ = UiMode::TitleSpecial;
    uiCursor_ = 0;
    uiPage_ = 0;
    uiScroll_ = 0;
    clicked_ = cancelClicked_ = false;
    if (audio_.PlayBgm(31, true, 255, res_)) WriteSysFlag(100 + 31, 1);
    titleBgmStarted_ = true;
}

void Engine::DrawSpecialBackdrop(const std::string& title,
                                 const std::string& subtitle, bool paged) {
    RenderTitleBackdrop();
    gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 2, 12, 24, 205);
    gfx_.FillRect(36, 28, 1208, 76, 8, 29, 47, 230);
    gfx_.FillRect(36, 102, 1208, 2, 120, 213, 239, 210);
    gfx_.FillRect(36, 112, 1208, 552, 2, 12, 24, 185);
    gfx_.FillRect(48, 112, 4, 552, 116, 211, 239, 175);
    gfx_.DrawText(title, 64, 38, 34, 238, 250, 255);
    const int sw = gfx_.TextWidth(subtitle, 20);
    gfx_.DrawText(subtitle, 1215 - sw, 57, 20, 155, 199, 218);
    gfx_.DrawText(paged ? "A 确认   B 返回   L/R 翻页"
                        : "方向键选择   A 播放   B 返回",
                  64, 675, 20, 151, 190, 208);
}

void Engine::DrawSpecialGridCell(int index, int x, int y, int w, int h,
                                 bool selected, bool unlocked, int imageId) {
    const uint8_t borderR = selected ? 250 : 81;
    const uint8_t borderG = selected ? 213 : 132;
    const uint8_t borderB = selected ? 146 : 155;
    gfx_.FillRect(x - 3, y - 3, w + 6, h + 6, borderR, borderG, borderB,
                  selected ? 245 : 190);
    gfx_.FillRect(x, y, w, h, 4, 20, 34, 245);

    Tex* thumb = nullptr;
    if (unlocked && imageId > 0) {
        const std::string tv = Res::TvName(imageId);
        if (res_.Exists(tv)) thumb = gfx_.Get(tv, res_, "");
    }
    if (thumb) {
        gfx_.DrawTexture(thumb, x + 3, y + 3, w - 6, h - 31, 1.0f);
    } else {
        gfx_.FillRect(x + 3, y + 3, w - 6, h - 31,
                      unlocked ? 20 : 8, unlocked ? 48 : 24,
                      unlocked ? 63 : 34, 255);
        const int cx = x + w / 2, cy = y + (h - 28) / 2;
        gfx_.FillRect(cx - 1, cy - 24, 3, 49, 158, 205, 224, unlocked ? 150 : 75);
        gfx_.FillRect(cx - 24, cy - 1, 49, 3, 158, 205, 224, unlocked ? 150 : 75);
        gfx_.FillRect(cx - 14, cy - 14, 29, 29, 76, 133, 157, unlocked ? 105 : 48);
    }
    gfx_.FillRect(x + 3, y + h - 27, w - 6, 24, 4, 17, 29, 235);
    const std::string number = imageId > 0
        ? Format("%02d   %06d%s", index + 1, imageId, unlocked ? "" : "   LOCK")
        : Format("%02d   ---", index + 1);
    gfx_.DrawText(number, x + 10, y + h - 25, 17,
                  unlocked ? 213 : 91, unlocked ? 235 : 113,
                  unlocked ? 244 : 126);
}

void Engine::RenderCgMode() {
    DrawSpecialBackdrop("CG MODE", "画廊 / 14 PAGE");
    constexpr int kPages = 14, kPerPage = 12, kColumns = 4;
    if (uiPagePrev_ || uiPageNext_) {
        uiPage_ = (uiPage_ + (uiPagePrev_ ? kPages - 1 : 1)) % kPages;
        uiCursor_ = 0;
        gfx_.ClearCache();
    }
    uiPagePrev_ = uiPageNext_ = false;

    constexpr int x0 = 78, y0 = 145, w = 264, h = 136, gx = 20, gy = 19;
    for (int i = 0; i < kPerPage; ++i) {
        const int x = x0 + (i % kColumns) * (w + gx);
        const int y = y0 + (i / kColumns) * (h + gy);
        if (PointerIn(x, y, w, h)) uiCursor_ = i;
    }
    if (PointerIn(500, 666, 80, 42)) {
        uiPage_ = (uiPage_ + kPages - 1) % kPages; uiCursor_ = 0; clicked_ = false;
    } else if (PointerIn(700, 666, 80, 42)) {
        uiPage_ = (uiPage_ + 1) % kPages; uiCursor_ = 0; clicked_ = false;
    }
    ConsumeGridInput(kPerPage, kColumns);

    const auto& slots = CgSlots();
    const int base = uiPage_ * kPerPage;
    for (int i = 0; i < kPerPage; ++i) {
        const int global = base + i;
        const bool valid = global < (int)slots.size() && !slots[(size_t)global].empty();
        const bool unlocked = valid && CgSlotUnlocked(slots[(size_t)global], unlockedCgs_);
        const int imageId = valid ? slots[(size_t)global][0] : -1;
        const int x = x0 + (i % kColumns) * (w + gx);
        const int y = y0 + (i / kColumns) * (h + gy);
        DrawSpecialGridCell(global, x, y, w, h, uiCursor_ == i, unlocked, imageId);
    }

    const int global = base + uiCursor_;
    const bool valid = global < (int)slots.size() && !slots[(size_t)global].empty();
    const bool unlocked = valid && CgSlotUnlocked(slots[(size_t)global], unlockedCgs_);
    const std::string status = !valid ? "预留空槽" : unlocked
        ? Format("已解锁 · %zu 张差分", slots[(size_t)global].size())
        : "尚未在剧情中解锁";
    gfx_.DrawText(status, 860, 675, 19, unlocked ? 226 : 142,
                  unlocked ? 238 : 161, unlocked ? 245 : 175);
    gfx_.DrawText(Format("<  %02d / %02d  >", uiPage_ + 1, kPages), 568, 675,
                  20, 225, 237, 243);

    if (clicked_) {
        clicked_ = false;
        if (unlocked) {
            cgViewSlot_ = global;
            cgViewVariant_ = 0;
            ui_ = UiMode::TitleCgView;
            gfx_.ClearCache();
        }
    }
    if (cancelClicked_) { cancelClicked_ = false; CloseSpecialMode(); }
}

void Engine::RenderCgViewer() {
    const auto& slots = CgSlots();
    if (cgViewSlot_ < 0 || cgViewSlot_ >= (int)slots.size() ||
        slots[(size_t)cgViewSlot_].empty()) {
        ui_ = UiMode::TitleCg;
        return;
    }
    const auto& variants = slots[(size_t)cgViewSlot_];
    cgViewVariant_ = std::clamp(cgViewVariant_, 0, (int)variants.size() - 1);
    Tex* cg = gfx_.Get(Res::CgName(variants[(size_t)cgViewVariant_]), res_, "");
    if (cg) gfx_.DrawTexture(cg, 0, 0, kVirtualW, kVirtualH, 1.0f);
    else gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 3, 15, 26, 255);
    gfx_.FillRect(0, 0, kVirtualW, 50, 1, 9, 17, 175);
    gfx_.FillRect(0, 674, kVirtualW, 46, 1, 9, 17, 190);
    gfx_.DrawText(Format("CG %03d   %d / %zu", cgViewSlot_ + 1,
                         cgViewVariant_ + 1, variants.size()),
                  30, 11, 23, 240, 247, 250);
    gfx_.DrawText("←/→ 切换   A 下一张   B 返回画廊", 800, 684, 20, 212, 231, 240);

    if (navX_ != 0) {
        const int n = (int)variants.size();
        cgViewVariant_ = (cgViewVariant_ + (navX_ < 0 ? n - 1 : 1)) % n;
        navX_ = navY_ = 0;
        gfx_.ClearCache();
    }
    if (clicked_) {
        clicked_ = false;
        if (++cgViewVariant_ >= (int)variants.size()) ui_ = UiMode::TitleCg;
        gfx_.ClearCache();
    }
    if (cancelClicked_) {
        cancelClicked_ = false;
        ui_ = UiMode::TitleCg;
        gfx_.ClearCache();
    }
}

void Engine::RenderSceneReplay() {
    DrawSpecialBackdrop("SCENE REPLAY", "名场面回放 / 2 PAGE");
    constexpr int kPages = 2, kPerPage = 12, kColumns = 4;
    if (uiPagePrev_ || uiPageNext_) {
        uiPage_ = 1 - uiPage_; uiCursor_ = 0; gfx_.ClearCache();
    }
    uiPagePrev_ = uiPageNext_ = false;
    constexpr int x0 = 78, y0 = 145, w = 264, h = 136, gx = 20, gy = 19;
    for (int i = 0; i < kPerPage; ++i) {
        const int x = x0 + (i % kColumns) * (w + gx);
        const int y = y0 + (i / kColumns) * (h + gy);
        if (PointerIn(x, y, w, h)) uiCursor_ = i;
    }
    if (PointerIn(500, 666, 80, 42)) {
        uiPage_ = 1 - uiPage_; uiCursor_ = 0; clicked_ = false;
    } else if (PointerIn(700, 666, 80, 42)) {
        uiPage_ = 1 - uiPage_; uiCursor_ = 0; clicked_ = false;
    }
    ConsumeGridInput(kPerPage, kColumns);

    const auto& slots = SceneReplaySlots();
    const int base = uiPage_ * kPerPage;
    for (int i = 0; i < kPerPage; ++i) {
        const int global = base + i;
        const auto& slot = slots[(size_t)global];
        const bool unlocked = ReadSysFlag(slot.unlockFlag) == 1;
        const int x = x0 + (i % kColumns) * (w + gx);
        const int y = y0 + (i / kColumns) * (h + gy);
        DrawSpecialGridCell(global, x, y, w, h, uiCursor_ == i, unlocked, slot.thumbnailCg);
    }
    const auto& selected = slots[(size_t)(base + uiCursor_)];
    const bool unlocked = ReadSysFlag(selected.unlockFlag) == 1;
    static const char* kChapter[] = {"IC", "CLOSING", "CODA"};
    gfx_.DrawText(Format("%s / SCENE %02d / %s%s",
                         kChapter[std::clamp(selected.chapter, 0, 2)],
                         base + uiCursor_ + 1, selected.sourceScript,
                         unlocked ? "" : " / LOCK"),
                  824, 675, 17, unlocked ? 220 : 135, unlocked ? 235 : 154,
                  unlocked ? 243 : 168);
    gfx_.DrawText(Format("<  %d / %d  >", uiPage_ + 1, kPages), 590, 675,
                  20, 225, 237, 243);
    if (clicked_) {
        clicked_ = false;
        if (unlocked) StartReplay(base + uiCursor_);
    }
    if (cancelClicked_) { cancelClicked_ = false; CloseSpecialMode(); }
}

void Engine::RenderBgmMode() {
    DrawSpecialBackdrop("MUSIC MODE", "原声鉴赏 / 63 TRACKS");
    // 与参考实现一致的隐藏解锁条件。
    if (ReadSysFlag(0x68) == 1 && ReadSysFlag(0xa3) == 1)
        WriteSysFlag(100 + 0x22, 1);
    constexpr int kPages = 2;
    if (uiPagePrev_ || uiPageNext_) { uiPage_ = 1 - uiPage_; uiCursor_ = 0; }
    uiPagePrev_ = uiPageNext_ = false;
    if (PointerIn(500, 666, 80, 42) || PointerIn(700, 666, 80, 42)) {
        uiPage_ = 1 - uiPage_; uiCursor_ = 0; clicked_ = false;
    }
    const auto& tracks = BgmSlots();
    const int base = uiPage_ == 0 ? 0 : 31;
    const int count = uiPage_ == 0 ? 31 : 32;
    constexpr int x0 = 92, y0 = 137, w = 530, h = 30, gx = 22, gy = 1;
    for (int i = 0; i < count; ++i) {
        const int x = x0 + (i % 2) * (w + gx);
        const int y = y0 + (i / 2) * (h + gy);
        if (PointerIn(x, y, w, h)) uiCursor_ = i;
    }
    ConsumeGridInput(count, 2);
    for (int i = 0; i < count; ++i) {
        const int id = tracks[(size_t)(base + i)];
        const bool unlocked = ReadSysFlag(100 + id) == 1;
        const bool playing = audio_.CurrentBgmId() == id;
        const bool selected = uiCursor_ == i;
        const int x = x0 + (i % 2) * (w + gx);
        const int y = y0 + (i / 2) * (h + gy);
        if (selected) gfx_.FillRect(x - 3, y, w + 6, h, 204, 174, 111, 225);
        gfx_.FillRect(x, y + 2, w, h - 4, 5, 25, 40, selected ? 245 : 205);
        gfx_.DrawText(Format("%s TRACK %02d     BGM %03d%s",
                             playing ? "▶" : " ", base + i + 1, id,
                             unlocked ? "" : "     LOCK"),
                      x + 12, y + 5, 18,
                      unlocked ? 221 : 91, unlocked ? 236 : 115,
                      unlocked ? 244 : 130);
    }
    gfx_.DrawText(Format("<  %d / %d  >", uiPage_ + 1, kPages), 590, 675,
                  20, 225, 237, 243);
    if (clicked_) {
        clicked_ = false;
        const int id = tracks[(size_t)(base + uiCursor_)];
        if (ReadSysFlag(100 + id) == 1) audio_.PlayBgm(id, true, 255, res_);
    }
    if (cancelClicked_) { cancelClicked_ = false; CloseSpecialMode(); }
}

void Engine::RenderVoiceMessages() {
    DrawSpecialBackdrop("SPECIAL MESSAGE", "声优访谈 / 5 MESSAGES", false);
    if (voiceMessagePlaying_ >= 0 && audio_.VoiceRemaining(0) <= 0.0f)
        voiceMessagePlaying_ = -1;
    constexpr int count = 5, x0 = 74, y = 166, w = 210, h = 380, gap = 30;
    static const int kVoiceOrder[count] = {3, 1, 0, 2, 4};
    for (int i = 0; i < count; ++i) {
        const int x = x0 + i * (w + gap);
        if (PointerIn(x, y, w, h)) uiCursor_ = i;
    }
    ConsumeGridInput(count, count);
    for (int i = 0; i < count; ++i) {
        const int x = x0 + i * (w + gap);
        const bool selected = uiCursor_ == i;
        gfx_.FillRect(x - 3, y - 3, w + 6, h + 6,
                      selected ? 245 : 73, selected ? 208 : 127,
                      selected ? 145 : 153, selected ? 245 : 190);
        gfx_.FillRect(x, y, w, h, 4, 21, 35, 245);
        Tex* portrait = nullptr;
        const int voiceId = kVoiceOrder[i];
        const std::string tga = Format("sys_0720%d.tga", voiceId);
        const std::string png = Format("sys_0720%d.png", voiceId);
        if (res_.Exists(tga)) portrait = gfx_.Get(tga, res_, "");
        else if (res_.Exists(png)) portrait = gfx_.Get(png, res_, "");
        if (portrait) gfx_.DrawTexture(portrait, x + 5, y + 5, w - 10, h - 72, 1.0f);
        else {
            gfx_.FillRect(x + 5, y + 5, w - 10, h - 72, 15, 48, 64, 255);
            const std::string mark = Format("VOICE %d", i + 1);
            gfx_.DrawText(mark, x + (w - gfx_.TextWidth(mark, 24)) / 2,
                          y + 145, 24, 160, 210, 229);
        }
        gfx_.FillRect(x + 5, y + h - 63, w - 10, 58, 3, 15, 27, 240);
        gfx_.DrawText(Format("访谈 %d", i + 1), x + 17, y + h - 55, 22,
                      229, 239, 245);
        gfx_.DrawText(voiceMessagePlaying_ == voiceId ? "播放中" : "A 播放",
                      x + 17, y + h - 29, 16,
                      voiceMessagePlaying_ == voiceId ? 251 : 137,
                      voiceMessagePlaying_ == voiceId ? 211 : 178, 154);
    }
    if (clicked_) {
        clicked_ = false;
        const int id = kVoiceOrder[uiCursor_];
        const std::string name = Format("9500_000%d_%02d.ogg", id, id + 1);
        if (res_.Exists(name) && audio_.PlayVoiceFile(name, 0, 255, false, res_) > 0.0f)
            voiceMessagePlaying_ = id;
    }
    if (cancelClicked_) { cancelClicked_ = false; CloseSpecialMode(); }
}

void Engine::RenderUi() {
    if (ui_ == UiMode::None) return;
    if (ui_ == UiMode::TitleCg) { RenderCgMode(); return; }
    if (ui_ == UiMode::TitleCgView) { RenderCgViewer(); return; }
    if (ui_ == UiMode::TitleScene) { RenderSceneReplay(); return; }
    if (ui_ == UiMode::TitleBgm) { RenderBgmMode(); return; }
    if (ui_ == UiMode::TitleVoice) { RenderVoiceMessages(); return; }
    if (ui_ == UiMode::Config) { RenderConfigUi(); return; }

    auto centerText = [&](const std::string& s, int y, int size, bool sel) {
        int w = gfx_.TextWidth(s, size);
        gfx_.DrawText(s, kVirtualW / 2 - w / 2, y, size,
                      sel ? 255 : 160, sel ? 220 : 160, sel ? 140 : 160);
    };

    if (ui_ == UiMode::Title || ui_ == UiMode::TitleStart ||
        ui_ == UiMode::TitleSpecial || ui_ == UiMode::TitleNovel) {
        RenderTitleBackdrop();
        Tex* atlas = gfx_.Get("t0100.tga", res_, "");
        auto drawRows = [this, atlas](const std::vector<int>& rows,
                                      const std::vector<std::string>& fallback) {
            // Godot 场景的 320x40 按钮以 1.25 倍显示，中心/间距保持原样。
            const int x = 440, y0 = 350, w = 400, h = 50;
            for (int i = 0; i < (int)rows.size(); ++i) {
                if (PointerIn(x, y0 + i * h, w, h)) uiCursor_ = i;
                if (atlas) {
                    const int sx = i == uiCursor_ ? 320 : 0;
                    gfx_.DrawTextureRegion(atlas, sx, rows[i], 320, 40,
                                           x, y0 + i * h, w, h, 1.0f);
                } else if (i < (int)fallback.size()) {
                    int tw = gfx_.TextWidth(fallback[i], 34);
                    gfx_.DrawText(fallback[i], kVirtualW / 2 - tw / 2, y0 + i * h + 5,
                                  34, i == uiCursor_ ? 255 : 220,
                                  i == uiCursor_ ? 220 : 235, 210);
                }
            }
        };

        if (ui_ == UiMode::Title) {
            const std::vector<std::string> items = {"开始游戏", "继续游戏", "系统设置", "特别模式", "结束游戏"};
            drawRows({0, 40, 80, 120, 160}, items);
            HandleMenuInput((int)items.size(), false, [this](int idx) {
                Log(LogLevel::Info, "title: main selected %d", idx);
                switch (idx) {
                case 0: ui_ = UiMode::TitleStart; uiCursor_ = 0; break;
                case 1: ui_ = UiMode::Load; uiCursor_ = 0; break;
                case 2: ui_ = UiMode::Config; uiCursor_ = uiPage_ = uiScroll_ = 0; break;
                case 3: ui_ = UiMode::TitleSpecial; uiCursor_ = 0; break;
                case 4: state_ = State::Quit; break;
                }
            });
        } else if (ui_ == UiMode::TitleStart) {
            const std::vector<std::string> items = {"序章", "终章", "coda", "返回"};
            drawRows({200, 240, 280, 320}, items);
            HandleMenuInput((int)items.size(), true, [this](int idx) {
                Log(LogLevel::Info, "title: chapter selected %d", idx);
                if (idx == 0) StartChapter("1001");
                else if (idx == 1) StartChapter("2001");
                else if (idx == 2) StartChapter("3001");
                else { ui_ = UiMode::Title; uiCursor_ = 0; }
            });
        } else if (ui_ == UiMode::TitleSpecial) {
            const std::vector<std::string> items = {
                "电子小说", "CG模式", "场景回放", "音乐模式", "声优访谈", "返回"
            };
            drawRows({360, 400, 440, 480, 520, 320}, items);
            HandleMenuInput((int)items.size(), true, [this](int idx) {
                if (idx == 0) { ui_ = UiMode::TitleNovel; uiCursor_ = 0; }
                else if (idx == 1) OpenSpecialMode(UiMode::TitleCg);
                else if (idx == 2) OpenSpecialMode(UiMode::TitleScene);
                else if (idx == 3) OpenSpecialMode(UiMode::TitleBgm);
                else if (idx == 4) OpenSpecialMode(UiMode::TitleVoice);
                else if (idx == 5) { ui_ = UiMode::Title; uiCursor_ = 0; }
            });
        } else {
            // T0100 的 560/600 行就是原版两篇电子小说按钮。
            const std::vector<std::string> items = {"电子小说 1", "电子小说 2", "返回"};
            drawRows({560, 600, 320}, items);
            HandleMenuInput((int)items.size(), true, [this](int idx) {
                if (idx == 0) StartChapter("5000");
                else if (idx == 1) StartChapter("5100");
                else { ui_ = UiMode::TitleSpecial; uiCursor_ = 0; }
            });
        }
    } else if (ui_ == UiMode::Menu) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0, 150);
        std::vector<std::string> items = {"继续", "存档", "读取", "设置", "回到标题", "退出游戏"};
        for (int i = 0; i < (int)items.size(); i++) {
            if (PointerIn(400, 194 + i * 64, 480, 54)) uiCursor_ = i;
            centerText(items[i], 200 + i * 64, 32, i == uiCursor_);
        }
        HandleMenuInput((int)items.size(), true, [this](int idx) {
            switch (idx) {
            case 0: ui_ = UiMode::None; break;
            case 1: ui_ = UiMode::Save; uiCursor_ = 0; break;
            case 2: ui_ = UiMode::Load; uiCursor_ = 0; break;
            case 3: ui_ = UiMode::Config; uiCursor_ = uiPage_ = uiScroll_ = 0; break;
            case 4: GoTitle(); break;
            case 5: state_ = State::Quit; break;
            }
        });
    } else if (ui_ == UiMode::Save || ui_ == UiMode::Load) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0, 180);
        gfx_.DrawText(ui_ == UiMode::Save ? "存档" : "读档", 80, 60, 40, 255, 255, 255);
        for (int i = 0; i < kSaveSlots; i++) {
            if (PointerIn(120, 130 + i * 80, 1040, 62)) uiCursor_ = i;
            std::string label = Format("槽位 %d  %s", i + 1, SlotMeta(i).c_str());
            gfx_.DrawText(label, 140, 140 + i * 80, 30, i == uiCursor_ ? 255 : 170,
                          i == uiCursor_ ? 220 : 170, i == uiCursor_ ? 140 : 170);
        }
        HandleMenuInput(kSaveSlots, true, [this](int idx) {
            if (ui_ == UiMode::Save) {
                if (SaveToSlotFile(idx)) ui_ = UiMode::None;
            } else {
                if (LoadFromSlotFile(idx)) { ui_ = UiMode::None; state_ = State::Game; }
            }
        });
    } else if (ui_ == UiMode::Backlog) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0, 220);
        gfx_.DrawText("回看(B/L 关闭,↑↓ 滚动)", 80, 30, 26, 200, 200, 200);
        int y = 80;
        int line = 0;
        for (int bi = (int)scene_.backlog.size() - 1 - backlogScroll_; bi >= 0; bi--, y += 66) {
            if (y > kVirtualH - 40) break;
            if (line++ > 8) break;
            const auto& e = scene_.backlog[bi];
            std::string full = e.name.empty() ? e.text : e.name + "  " + e.text;
            gfx_.DrawText(full.substr(0, 46), 80, y, 26, 230, 230, 230);
        }
        BacklogInput();
    }
}

void Engine::RenderConfigUi() {
    if (state_ == State::Title) RenderTitleBackdrop();
    gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 2, 12, 24, 218);
    gfx_.FillRect(34, 25, 1212, 670, 5, 24, 39, 235);
    gfx_.FillRect(34, 25, 6, 670, 119, 211, 239, 210);
    gfx_.DrawText("SYSTEM CONFIGURATION", 64, 38, 34, 236, 248, 253);
    gfx_.DrawText("原版设置项目 · 所有修改即时生效并自动保存", 690, 51, 20,
                  145, 189, 207);

    static const char* kTabs[3] = {"文字・操作", "声音", "显示"};
    if (!configResetConfirm_) {
        for (int p = 0; p < 3; ++p) {
            const int x = 76 + p * 252;
            if (PointerIn(x, 91, 230, 45)) {
                uiPage_ = p; uiCursor_ = uiScroll_ = 0; clicked_ = false;
            }
        }
        if (uiPagePrev_ || uiPageNext_) {
            uiPage_ = (uiPage_ + (uiPagePrev_ ? 2 : 1)) % 3;
            uiCursor_ = uiScroll_ = 0;
        }
    }
    uiPagePrev_ = uiPageNext_ = false;
    for (int p = 0; p < 3; ++p) {
        const int x = 76 + p * 252;
        gfx_.FillRect(x, 91, 230, 45,
                      p == uiPage_ ? 111 : 14, p == uiPage_ ? 185 : 47,
                      p == uiPage_ ? 211 : 65, p == uiPage_ ? 235 : 210);
        gfx_.DrawText(kTabs[p], x + 24, 101, 23,
                      p == uiPage_ ? 255 : 166, p == uiPage_ ? 255 : 198,
                      p == uiPage_ ? 255 : 211);
    }

    const int count = uiPage_ == 0 ? 8 : uiPage_ == 1 ? 14 : 4;
    if (configResetConfirm_) {
        if (navX_ || navY_) configResetCursor_ = 1 - configResetCursor_;
        navX_ = navY_ = 0;
        if (PointerIn(438, 385, 170, 48)) configResetCursor_ = 0;
        else if (PointerIn(672, 385, 170, 48)) configResetCursor_ = 1;
        if (cancelClicked_) {
            cancelClicked_ = clicked_ = false;
            configResetConfirm_ = false;
        } else if (clicked_) {
            clicked_ = false;
            if (configResetCursor_ == 0) {
                config_.SetDefaults();
                ApplyAudioConfig();
                SaveConfigFile();
            }
            configResetConfirm_ = false;
        }
    } else {
        ConfigAdjustInput(count);
    }
    constexpr int kVisibleRows = 8;
    if (uiCursor_ < uiScroll_) uiScroll_ = uiCursor_;
    if (uiCursor_ >= uiScroll_ + kVisibleRows) uiScroll_ = uiCursor_ - kVisibleRows + 1;
    uiScroll_ = std::clamp(uiScroll_, 0, std::max(0, count - kVisibleRows));

    static const char* kTextRows[] = {
        "文字显示速度", "自动播放等待", "允许跳过未读", "演出等待方式",
        "确认框默认选项", "页面结束停止语音", "长按触屏快进", "恢复默认设置"
    };
    static const char* kSoundRows[] = {
        "总音量", "BGM 音量", "SE 音量", "语音音量",
        "北原春希", "小木曽雪菜", "冬马和纱", "杉浦小春", "和泉千晶",
        "风冈麻理", "饭冢武也", "水泽依绪", "其他男性角色", "其他女性角色"
    };
    static const char* kDisplayRows[] = {
        "普通对话框透明度", "CG 对话框透明度", "电子小说遮罩透明度",
        "H 场景只保留主要角色语音"
    };
    static const char* kSpeedNames[] = {"瞬时", "慢", "标准", "快"};

    auto drawSlider = [this](int y, int value, int maximum, const std::string& valueText) {
        constexpr int x = 742, width = 372;
        gfx_.FillRect(x, y + 21, width, 8, 16, 51, 67, 255);
        const int filled = maximum > 0 ? value * width / maximum : 0;
        if (filled > 0) gfx_.FillRect(x, y + 21, filled, 8, 110, 207, 235, 255);
        const int knob = x + std::clamp(filled, 0, width);
        gfx_.FillRect(knob - 4, y + 15, 8, 20, 239, 211, 155, 255);
        gfx_.DrawText(valueText, 1132, y + 10, 20, 220, 235, 242);
    };
    auto drawToggle = [this](int y, bool enabled) {
        gfx_.FillRect(941, y + 9, 173, 34, 12, 44, 61, 255);
        gfx_.FillRect(enabled ? 1030 : 945, y + 12, 80, 28,
                      enabled ? 101 : 76, enabled ? 193 : 104,
                      enabled ? 221 : 126, 255);
        gfx_.DrawText(enabled ? "开启" : "关闭", enabled ? 1047 : 962,
                      y + 15, 18, 242, 248, 251);
    };

    constexpr int y0 = 154, rowH = 58;
    for (int i = uiScroll_; i < count && i < uiScroll_ + kVisibleRows; ++i) {
        const int y = y0 + (i - uiScroll_) * rowH;
        const bool selected = i == uiCursor_;
        if (selected) {
            gfx_.FillRect(70, y, 1140, 51, 190, 164, 105, 225);
            gfx_.FillRect(74, y + 3, 1132, 45, 8, 35, 52, 248);
        } else {
            gfx_.FillRect(74, y + 3, 1132, 45, 5, 27, 43, 218);
        }
        const char* label = uiPage_ == 0 ? kTextRows[i]
                          : uiPage_ == 1 ? kSoundRows[i] : kDisplayRows[i];
        gfx_.DrawText(label, 101, y + 10, 23, selected ? 246 : 187,
                      selected ? 240 : 211, selected ? 222 : 222);

        if (uiPage_ == 0) {
            if (i == 0) {
                drawSlider(y, config_.textSpeed, 3, kSpeedNames[config_.textSpeed]);
            } else if (i == 1) {
                drawSlider(y, config_.autoDelayFrames - 60, 540,
                           Format("%.1f 秒", config_.autoDelayFrames * kFrameTime));
            } else if (i == 2) drawToggle(y, config_.skipUnread);
            else if (i == 3) {
                gfx_.DrawText(config_.fastWait ? "快速" : "标准", 1037, y + 14,
                              20, 224, 237, 243);
            } else if (i == 4) {
                gfx_.DrawText(config_.confirmDefaultYes ? "是" : "否", 1060, y + 14,
                              20, 224, 237, 243);
            } else if (i == 5) drawToggle(y, config_.pageVoice);
            else if (i == 6) drawToggle(y, config_.longPressSkip);
            else gfx_.DrawText("A  恢复原版默认值", 876, y + 12, 20, 232, 214, 173);
        } else if (uiPage_ == 1) {
            if (i < 4) {
                const int value = i == 0 ? config_.masterVolume : i == 1 ? config_.bgmVolume
                                : i == 2 ? config_.seVolume : config_.voiceVolume;
                drawSlider(y, value, 255, Format("%3d", value));
            } else {
                drawToggle(y, config_.charVoice[(size_t)(i - 4)] != 0);
            }
        } else {
            if (i < 3) {
                const int value = i == 0 ? config_.windowAlpha
                                : i == 1 ? config_.cgWindowAlpha : config_.novelWindowAlpha;
                drawSlider(y, value, 256, Format("%3d", value));
            } else drawToggle(y, config_.eroVoice);
        }
    }

    if (uiPage_ == 2) {
        // 透明度即时预览，避免用户反复退出设置确认效果。
        gfx_.FillRect(735, 421, 410, 177, 39, 67, 79, 255);
        gfx_.FillRect(750, 438, 380, 143, 0, 20, 33,
                      (uint8_t)std::clamp(config_.windowAlpha, 0, 255));
        gfx_.FillRect(750, 438, 380, 3, 166, 220, 237, 220);
        gfx_.DrawText("对话框透明度预览", 790, 476, 25, 238, 245, 248);
        gfx_.DrawText("调整后会立即作用于当前剧情画面", 790, 520, 19, 170, 204, 217);
    }
    if (uiPage_ == 1 && count > kVisibleRows)
        gfx_.DrawText(Format("%d / %d", uiCursor_ + 1, count), 1132, 625, 18,
                      148, 190, 208);
    gfx_.DrawText("方向键选择/调整   A 切换或确认   B 保存并返回   L/R 切换分页",
                  72, 656, 20, 157, 197, 214);

    if (configResetConfirm_) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0, 145);
        gfx_.FillRect(347, 227, 586, 250, 190, 164, 105, 245);
        gfx_.FillRect(352, 232, 576, 240, 5, 27, 43, 252);
        const std::string prompt = "所有设置恢复为原版默认值？";
        gfx_.DrawText(prompt, 640 - gfx_.TextWidth(prompt, 27) / 2,
                      282, 27, 239, 246, 250);
        gfx_.DrawText("此操作会立即保存", 535, 330, 19, 151, 190, 208);
        for (int i = 0; i < 2; ++i) {
            const int x = i == 0 ? 438 : 672;
            const bool selected = configResetCursor_ == i;
            gfx_.FillRect(x, 385, 170, 48,
                          selected ? 110 : 16, selected ? 190 : 50,
                          selected ? 216 : 68, 245);
            const char* label = i == 0 ? "是" : "否";
            gfx_.DrawText(label, x + 75, 395, 23, 248, 251, 253);
        }
    }
}

std::string Engine::SlotMeta(int slot) {
    std::string path = SlotPath(slot);
    if (!FileExists(path)) return "(空)";
    std::vector<uint8_t> data = ReadFileAll(path);
    if (data.size() < 8) return "(损坏)";
    SaveData sav;
    sav.Reset();
    ByteReader in(data.data(), data.size());
    if (!sav.Load(in)) return "(损坏)";
    return sav.meta.preview.empty() ? sav.meta.chapter : sav.meta.preview;
}

// 通用菜单输入:返回 true = 有确认
bool Engine::HandleMenuInputImpl(int count, bool allowCancel, int* outIdx, bool* canceled) {
    bool act = false;
    if (navY_ < 0) uiCursor_ = (uiCursor_ + count - 1) % count;
    if (navY_ > 0) uiCursor_ = (uiCursor_ + 1) % count;
    navY_ = 0;
    if (clicked_) { clicked_ = false; act = true; *outIdx = uiCursor_; }
    if (cancelClicked_) {
        cancelClicked_ = false;
        if (!allowCancel) return act;
        *canceled = true;
        CancelUi();
        return false;
    }
    if (act) *outIdx = uiCursor_;
    return act;
}

void Engine::CancelUi() {
    if (ui_ == UiMode::TitleCgView) {
        ui_ = UiMode::TitleCg;
        uiCursor_ = 0;
    } else if (ui_ == UiMode::TitleCg || ui_ == UiMode::TitleScene ||
               ui_ == UiMode::TitleBgm || ui_ == UiMode::TitleVoice) {
        CloseSpecialMode();
        return;
    } else if (ui_ == UiMode::TitleNovel) {
        ui_ = UiMode::TitleSpecial;
        uiCursor_ = 0;
    } else if (ui_ == UiMode::TitleStart || ui_ == UiMode::TitleSpecial) {
        ui_ = UiMode::Title;
    } else if (ui_ == UiMode::Menu || ui_ == UiMode::Backlog) {
        ui_ = UiMode::None;
    } else if (ui_ == UiMode::Save || ui_ == UiMode::Load || ui_ == UiMode::Config) {
        ui_ = state_ == State::Game ? UiMode::Menu : UiMode::Title;
    }
    uiCursor_ = 0;
}

void Engine::BacklogInput() {
    if (navY_ < 0) backlogScroll_++;
    if (navY_ > 0) backlogScroll_ = std::max(0, backlogScroll_ - 1);
    navY_ = 0;
}

void Engine::ConfigAdjustInput(int count) {
    if (navY_ < 0) uiCursor_ = (uiCursor_ + count - 1) % count;
    if (navY_ > 0) uiCursor_ = (uiCursor_ + 1) % count;
    constexpr int y0 = 154, rowH = 58, visibleRows = 8;
    bool pointerRowHit = false;
    if (pointerPressed_ && pointerX_ >= 70 && pointerX_ < 1210 && pointerY_ >= y0) {
        const int visible = (pointerY_ - y0) / rowH;
        const int candidate = uiScroll_ + visible;
        if (visible >= 0 && visible < visibleRows && candidate < count) {
            uiCursor_ = candidate;
            pointerRowHit = true;
        }
    }
    int d = navX_ < 0 ? -1 : navX_ > 0 ? 1 : 0;
    navX_ = navY_ = 0;
    bool activate = clicked_ && (!pointerPressed_ || pointerRowHit);
    clicked_ = false;
    bool changed = false;

    // 触屏直接定位滑块；手柄 A 则向右调一档。
    const bool sliderRow = (uiPage_ == 0 && uiCursor_ < 2) ||
                           (uiPage_ == 1 && uiCursor_ < 4) ||
                           (uiPage_ == 2 && uiCursor_ < 3);
    if (pointerPressed_ && sliderRow && pointerX_ >= 742 && pointerX_ <= 1114) {
        const int numerator = std::clamp(pointerX_ - 742, 0, 372);
        if (uiPage_ == 0 && uiCursor_ == 0)
            config_.textSpeed = (numerator * 3 + 186) / 372;
        else if (uiPage_ == 0)
            config_.autoDelayFrames = 60 + (numerator * 540 + 186) / 372;
        else if (uiPage_ == 1) {
            int* value = uiCursor_ == 0 ? &config_.masterVolume
                       : uiCursor_ == 1 ? &config_.bgmVolume
                       : uiCursor_ == 2 ? &config_.seVolume : &config_.voiceVolume;
            *value = (numerator * 255 + 186) / 372;
        } else {
            int* value = uiCursor_ == 0 ? &config_.windowAlpha
                       : uiCursor_ == 1 ? &config_.cgWindowAlpha : &config_.novelWindowAlpha;
            *value = (numerator * 256 + 186) / 372;
        }
        changed = true;
        activate = false;
    }

    if (sliderRow && activate) d = 1;
    auto adjustBool = [&](bool& value) {
        if (d < 0) value = false;
        else if (d > 0) value = true;
        else if (activate) value = !value;
        else return;
        changed = true;
    };

    if (uiPage_ == 0) {
        if (uiCursor_ == 0 && d) {
            config_.textSpeed = std::clamp(config_.textSpeed + d, 0, 3); changed = true;
        } else if (uiCursor_ == 1 && d) {
            config_.autoDelayFrames = std::clamp(config_.autoDelayFrames + d * 27, 60, 600);
            changed = true;
        } else if (uiCursor_ == 2) adjustBool(config_.skipUnread);
        else if (uiCursor_ == 3) adjustBool(config_.fastWait);
        else if (uiCursor_ == 4) adjustBool(config_.confirmDefaultYes);
        else if (uiCursor_ == 5) adjustBool(config_.pageVoice);
        else if (uiCursor_ == 6) adjustBool(config_.longPressSkip);
        else if (uiCursor_ == 7 && activate) {
            configResetConfirm_ = true;
            configResetCursor_ = config_.confirmDefaultYes ? 0 : 1;
        }
    } else if (uiPage_ == 1) {
        if (uiCursor_ < 4 && d) {
            int* value = uiCursor_ == 0 ? &config_.masterVolume
                       : uiCursor_ == 1 ? &config_.bgmVolume
                       : uiCursor_ == 2 ? &config_.seVolume : &config_.voiceVolume;
            *value = std::clamp(*value + d * 13, 0, 255); changed = true;
        } else if (uiCursor_ >= 4) {
            bool value = config_.charVoice[(size_t)(uiCursor_ - 4)] != 0;
            adjustBool(value);
            config_.charVoice[(size_t)(uiCursor_ - 4)] = value ? 1 : 0;
        }
    } else {
        if (uiCursor_ < 3 && d) {
            int* value = uiCursor_ == 0 ? &config_.windowAlpha
                       : uiCursor_ == 1 ? &config_.cgWindowAlpha : &config_.novelWindowAlpha;
            *value = std::clamp(*value + d * 13, 0, 256); changed = true;
        } else if (uiCursor_ == 3) adjustBool(config_.eroVoice);
    }
    if (changed) {
        ApplyAudioConfig();
        SaveConfigFile();
    }
    if (cancelClicked_) {
        cancelClicked_ = false;
        CancelUi();
    }
}

// ---------------- 存档 ----------------
static constexpr uint32_t kRuntimeSaveMagic = 0x32545352u; // "RST2"

std::string Engine::SlotPath(int slot) const {
    char buf[64];
    snprintf(buf, sizeof(buf), "save%d.bin", slot);
    return PathJoin(saveDir_, buf);
}

void Engine::BuildSav(SaveData* sav) {
    sav->Reset();
    sav->gameFlags = gameFlags_;
    sav->sysFlags = sysFlags_;
    sav->meta.chapter = Active() ? Active()->name() : startScript_;
    sav->meta.preview = scene_.backlog.empty() ? "" : scene_.backlog.back().text.substr(0, 60);
    sav->meta.timestamp = (uint64_t)SDL_GetTicks() * 1000;
    ByteBuf eb;
    // SceneState 是脚本语义层，bg_/Adv 是实际渲染层。旧版只写前者，
    // RenderImage 又没有同步 scene_.bg，导致所有旧存档的背景路径为空。
    SceneState savedScene = scene_;
    savedScene.bg.path = bg_.path;
    savedScene.bg.x = (int)bg_.x;
    savedScene.bg.y = (int)bg_.y;
    savedScene.bg.offset = 0;
    savedScene.bg.sx = bg_.sx;
    savedScene.bg.sy = bg_.sy;
    savedScene.timeMode = timeMode_;
    savedScene.effectMode = effectMode_;
    savedScene.demoMode = demoMode_;
    savedScene.voiceLabel = voiceLabel_;
    savedScene.Save(eb);
    eb.I32((int32_t)stack_.size());
    for (auto& s : stack_) s->Save(eb);
    eb.I32(timeMode_);
    eb.Str(effectMode_);
    eb.I32(voiceLabel_);

    // RST2 扩展块保持 WAM1 外壳兼容，同时补齐上游 wa2-godot 存档中
    // 原本就有的当前文本等待态、选项、BGM 和循环环境音。
    eb.U32(kRuntimeSaveMagic);
    eb.I32(adv_.visible ? 1 : 0);
    eb.I32(adv_.hide ? 1 : 0);
    eb.Str(adv_.name);
    eb.Str(adv_.text);
    eb.I32((int32_t)adv_.segments.size());
    for (const auto& segment : adv_.segments) eb.Str(segment);
    eb.I32(adv_.seg);
    eb.I32(adv_.shown);
    eb.I32(wait_.textBusy ? 1 : 0);
    eb.I32(wait_.waitClick ? 1 : 0);
    eb.I32(wait_.selectVisible ? 1 : 0);
    eb.I32((int32_t)scene_.selectItems.size());
    for (const auto& item : scene_.selectItems) {
        eb.Str(item.text);
        eb.I32(item.v1); eb.I32(item.v2); eb.I32(item.v3);
    }
    eb.I32(audio_.CurrentBgmId());
    eb.I32(audio_.CurrentBgmLoop() ? 1 : 0);
    eb.I32(audio_.CurrentBgmVolume());
    const auto loopSe = audio_.LoopingSe();
    eb.I32((int32_t)loopSe.size());
    for (const auto& se : loopSe) {
        eb.I32(se.channel); eb.I32(se.id); eb.I32(se.volume);
    }
    sav->engineBlock = eb.data();
}

bool Engine::ApplySav(const SaveData& sav) {
    std::vector<int> loadedGameFlags = sav.gameFlags;
    std::vector<uint8_t> loadedSysFlags = sav.sysFlags;
    if (loadedGameFlags.size() > kMaxGameFlags || loadedSysFlags.size() > kSysFlagCount)
        return false;
    loadedGameFlags.resize(kMaxGameFlags, 0);
    loadedSysFlags.resize(kSysFlagCount, 0);
    // SYSTEM 旗标是全局收藏/完成度；读取旧槽位不能把后来取得的内容锁回去。
    for (size_t i = 0; i < loadedSysFlags.size() && i < sysFlags_.size(); ++i) {
        const uint8_t merged = (uint8_t)(loadedSysFlags[i] | sysFlags_[i]);
        if (merged != sysFlags_[i]) systemDirty_ = true;
        loadedSysFlags[i] = merged;
    }

    ByteReader in(sav.engineBlock.data(), sav.engineBlock.size());
    SceneState loadedScene;
    if (!loadedScene.Load(in)) return false;
    int32_t n = in.I32();
    if (!in.Ok() || n <= 0 || n > 64) return false;
    std::vector<std::unique_ptr<Script>> loadedStack;
    loadedStack.reserve((size_t)n);
    for (int32_t i = 0; i < n; i++) {
        auto s = std::make_unique<Script>();
        if (!s->LoadState(in, res_)) return false;
        loadedStack.push_back(std::move(s));
    }
    const int loadedTimeMode = in.I32();
    const std::string loadedEffectMode = in.Str();
    const int loadedVoiceLabel = in.I32();
    if (!in.Ok()) return false;

    struct RuntimeState {
        bool present = false;
        bool advVisible = false, advHide = false;
        std::string advName, advText;
        std::vector<std::string> segments;
        int seg = 0, shown = 0;
        bool textBusy = false, waitClick = false, selectVisible = false;
        std::vector<SelectItem> selectItems;
        int bgmId = -1, bgmLoop = 0, bgmVolume = 255;
        std::vector<Audio::LoopSeState> loopSe;
    } runtime;

    if (in.Remaining()) {
        if (in.U32() != kRuntimeSaveMagic) return false;
        runtime.present = true;
        runtime.advVisible = in.I32() != 0;
        runtime.advHide = in.I32() != 0;
        runtime.advName = in.Str();
        runtime.advText = in.Str();
        const int32_t segments = in.I32();
        if (!in.Ok() || segments < 0 || segments > 64) return false;
        runtime.segments.reserve((size_t)segments);
        for (int32_t i = 0; i < segments; ++i) runtime.segments.push_back(in.Str());
        runtime.seg = in.I32();
        runtime.shown = in.I32();
        runtime.textBusy = in.I32() != 0;
        runtime.waitClick = in.I32() != 0;
        runtime.selectVisible = in.I32() != 0;
        const int32_t selections = in.I32();
        if (!in.Ok() || selections < 0 || selections > 64) return false;
        runtime.selectItems.reserve((size_t)selections);
        for (int32_t i = 0; i < selections; ++i) {
            SelectItem item;
            item.text = in.Str();
            item.v1 = in.I32(); item.v2 = in.I32(); item.v3 = in.I32();
            runtime.selectItems.push_back(std::move(item));
        }
        runtime.bgmId = in.I32();
        runtime.bgmLoop = in.I32();
        runtime.bgmVolume = in.I32();
        const int32_t loopCount = in.I32();
        if (!in.Ok() || loopCount < 0 || loopCount > Audio::kSeChannels) return false;
        runtime.loopSe.reserve((size_t)loopCount);
        for (int32_t i = 0; i < loopCount; ++i) {
            Audio::LoopSeState se;
            se.channel = in.I32(); se.id = in.I32(); se.volume = in.I32();
            if (se.channel < 0 || se.channel >= Audio::kSeChannels || se.id < 0)
                return false;
            runtime.loopSe.push_back(se);
        }
    }
    if (!in.Ok() || in.Remaining() != 0) return false;

    if (!runtime.present && loadedScene.bg.path.empty() && !loadedStack.empty()) {
        LegacySceneProbe probe(res_, loadedGameFlags, loadedSysFlags);
        BgInfo recovered;
        Script* target = loadedStack.back().get();
        if (probe.Recover(target->name(), target->pos(), &recovered)) {
            loadedScene.bg = std::move(recovered);
            Log(LogLevel::Info, "save: recovered legacy background %s",
                loadedScene.bg.path.c_str());
        } else {
            Log(LogLevel::Warn, "save: legacy background could not be reconstructed");
        }
    }

    // 到这里才提交，格式损坏或脚本缺失不会再把当前运行态清空一半。
    audio_.StopAll();
    video_.Stop();
    if (trans_.snap) gfx_.ReleaseSnapshot(trans_.snap);
    trans_ = {};
    gfx_.ClearCache();
    bmps_.clear();
    bgMove_ = {};
    weather_ = {};
    fb_ = {};

    gameFlags_ = std::move(loadedGameFlags);
    sysFlags_ = std::move(loadedSysFlags);
    for (auto& s : loadedStack) s->SetGameFlags(&gameFlags_);
    for (auto& s : stack_) graveyard_.push_back(std::move(s));
    stack_ = std::move(loadedStack);
    scene_ = std::move(loadedScene);
    timeMode_ = loadedTimeMode;
    effectMode_ = loadedEffectMode;
    voiceLabel_ = loadedVoiceLabel;
    demoMode_ = scene_.demoMode;

    bg_.path = scene_.bg.path;
    bg_.id = scene_.bg.id;
    bg_.x = (float)(scene_.bg.x - scene_.bg.offset);
    bg_.y = (float)scene_.bg.y;
    bg_.sx = scene_.bg.sx > 0 ? scene_.bg.sx : 1.0f;
    bg_.sy = scene_.bg.sy > 0 ? scene_.bg.sy : 1.0f;
    ResetCharacterVisuals(chars_);
    CommitCharacterVisuals(scene_, chars_, 0);

    wait_.Clear();
    adv_ = {};
    if (runtime.present) {
        adv_.visible = runtime.advVisible;
        adv_.hide = runtime.advHide;
        adv_.name = std::move(runtime.advName);
        adv_.text = std::move(runtime.advText);
        adv_.segments = std::move(runtime.segments);
        if (adv_.segments.empty()) adv_.segments.push_back(adv_.text);
        adv_.seg = std::clamp(runtime.seg, 0, (int)adv_.segments.size() - 1);
        adv_.shown = std::clamp(runtime.shown, 0, Utf8CharCount(adv_.segments[adv_.seg]));
        adv_.lastCharMs = SDL_GetTicks();
        wait_.textBusy = runtime.textBusy;
        wait_.waitClick = runtime.waitClick;
        scene_.selectItems = std::move(runtime.selectItems);
        if (runtime.selectVisible && !scene_.selectItems.empty()) ShowSelect();
    } else if (!scene_.backlog.empty()) {
        // 兼容 0.1.8 及更早存档：脚本位置已经越过当前 SetMessage，若不
        // 重建点击门，读档首帧会直接执行后面的三条超长 SE 指令。
        const BacklogEntry& last = scene_.backlog.back();
        adv_.visible = true;
        adv_.name = last.name;
        adv_.text = last.text;
        size_t start = 0;
        while (true) {
            const size_t p = adv_.text.find("\\k", start);
            if (p == std::string::npos) { adv_.segments.push_back(adv_.text.substr(start)); break; }
            adv_.segments.push_back(adv_.text.substr(start, p - start));
            start = p + 2;
        }
        adv_.seg = (int)adv_.segments.size() - 1;
        adv_.shown = Utf8CharCount(adv_.segments[adv_.seg]);
        adv_.lastCharMs = SDL_GetTicks();
        wait_.waitClick = true;
    }

    titleBgmStarted_ = false;
    if (runtime.present && runtime.bgmId >= 0)
        audio_.PlayBgm(runtime.bgmId, runtime.bgmLoop != 0, runtime.bgmVolume, res_);
    if (runtime.present) {
        for (const auto& se : runtime.loopSe)
            audio_.PlaySe(se.channel, se.id, true, 0, se.volume, res_);
    }
    Log(LogLevel::Info,
        "save: restored script=%s pos=%u bg=%s runtime=%d loop-se=%zu",
        Active() ? Active()->name().c_str() : "(none)", Active() ? Active()->pos() : 0,
        bg_.path.empty() ? "(legacy-missing)" : bg_.path.c_str(), runtime.present ? 1 : 0,
        runtime.loopSe.size());
    return true;
}

bool Engine::SaveToSlotFile(int slot) {
    SaveData sav;
    BuildSav(&sav);
    ByteBuf out;
    sav.Save(out);
    const bool ok = WriteFileAll(SlotPath(slot), out.data().data(), out.data().size());
    if (ok && systemDirty_) SaveSystemFile();
    return ok;
}

bool Engine::LoadFromSlotFile(int slot) {
    std::vector<uint8_t> data = ReadFileAll(SlotPath(slot));
    if (data.empty()) return false;
    SaveData sav;
    sav.Reset();
    ByteReader in(data.data(), data.size());
    if (!sav.Load(in)) return false;
    if (!ApplySav(sav)) return false;
    ApplyAudioConfig();
    return true;
}

// ---------------- 配置 ----------------
void Engine::LoadConfigFile() {
    std::vector<uint8_t> data = ReadFileAll(PathJoin(saveDir_, "config.bin"));
    if (!data.empty()) {
        ByteReader in(data.data(), data.size());
        config_.Load(in);
    }
}

void Engine::SaveConfigFile() {
    if (suppressPersistence_) return;
    ByteBuf out;
    config_.Save(out);
    WriteFileAll(PathJoin(saveDir_, "config.bin"), out.data().data(), out.data().size());
}

void Engine::ApplyAudioConfig() {
    const auto effective = [this](int channel) {
        return (std::clamp(config_.masterVolume, 0, 255) *
                std::clamp(channel, 0, 255) + 127) / 255;
    };
    audio_.SetVolumes(effective(config_.bgmVolume), effective(config_.seVolume),
                      effective(config_.voiceVolume));
}

uint64_t Engine::MessageReadKey(int msgIdx) const {
    const std::string& script = Active() ? Active()->name() : startScript_;
    uint32_t hash = 2166136261u;
    for (unsigned char c : script) { hash ^= c; hash *= 16777619u; }
    return ((uint64_t)hash << 32) | (uint32_t)msgIdx;
}

bool Engine::IsCgUnlocked(int id) const {
    return unlockedCgs_.find(id) != unlockedCgs_.end();
}

void Engine::UnlockCg(int id) {
    if (id >= 0 && unlockedCgs_.insert(id).second) systemDirty_ = true;
}

static constexpr uint32_t kSystemProgressMagic = 0x32535957u; // "WYS2"

void Engine::LoadSystemFile() {
    const std::vector<uint8_t> data = ReadFileAll(PathJoin(saveDir_, "wa2ns_system.bin"));
    if (data.empty()) return;
    ByteReader in(data);
    if (in.U32() != kSystemProgressMagic || in.U32() != 1) {
        Log(LogLevel::Warn, "system: ignored incompatible wa2ns_system.bin");
        return;
    }
    std::vector<uint8_t> flags = in.Bytes();
    const uint32_t cgCount = in.U32();
    if (!in.Ok() || flags.size() > kSysFlagCount || cgCount > 4096u) return;
    std::unordered_set<int> cgs;
    for (uint32_t i = 0; i < cgCount; ++i) cgs.insert(in.I32());
    const uint32_t readCount = in.U32();
    if (!in.Ok() || readCount > 500000u) return;
    std::unordered_set<uint64_t> reads;
    reads.reserve(readCount);
    for (uint32_t i = 0; i < readCount; ++i) {
        const uint64_t hi = in.U32();
        const uint64_t lo = in.U32();
        reads.insert((hi << 32) | lo);
    }
    if (!in.Ok() || in.Remaining() != 0) {
        Log(LogLevel::Warn, "system: rejected truncated progress file");
        return;
    }
    flags.resize(kSysFlagCount, 0);
    sysFlags_ = std::move(flags);
    unlockedCgs_ = std::move(cgs);
    readMessages_ = std::move(reads);
    systemDirty_ = false;
    Log(LogLevel::Info, "system: loaded flags=%zu cg=%zu read=%zu",
        sysFlags_.size(), unlockedCgs_.size(), readMessages_.size());
}

void Engine::SaveSystemFile() {
    if (suppressPersistence_) return;
    ByteBuf out;
    out.U32(kSystemProgressMagic);
    out.U32(1);
    out.Bytes(sysFlags_.data(), sysFlags_.size());
    std::vector<int> cgs(unlockedCgs_.begin(), unlockedCgs_.end());
    std::sort(cgs.begin(), cgs.end());
    out.U32((uint32_t)cgs.size());
    for (int id : cgs) out.I32(id);
    std::vector<uint64_t> reads(readMessages_.begin(), readMessages_.end());
    std::sort(reads.begin(), reads.end());
    out.U32((uint32_t)reads.size());
    for (uint64_t key : reads) {
        out.U32((uint32_t)(key >> 32));
        out.U32((uint32_t)key);
    }
    if (WriteFileAll(PathJoin(saveDir_, "wa2ns_system.bin"),
                     out.data().data(), out.data().size())) {
        systemDirty_ = false;
        lastSystemSaveMs_ = SDL_GetTicks();
    }
}

void Engine::ImportOriginalSystemFile() {
    std::string path = PathJoin(dataDir_, "SYSTEM.sav");
    if (!FileExists(path)) path = PathJoin(dataDir_, "system.sav");
    const std::vector<uint8_t> data = ReadFileAll(path);
    if (data.empty()) return;

    size_t importedFlags = 0, importedCgs = 0;
    constexpr size_t kSystemFlagOffset = 0x268480;
    for (int i = 0; i < kSysFlagCount; ++i) {
        const size_t offset = kSystemFlagOffset + (size_t)i * 4u;
        if (offset + 4 > data.size()) break;
        const uint32_t value = ReadU32(data.data() + offset);
        const uint8_t compact = (uint8_t)std::min<uint32_t>(value, 255u);
        if (compact && sysFlags_[(size_t)i] != compact) {
            sysFlags_[(size_t)i] = compact;
            ++importedFlags;
            systemDirty_ = true;
        }
    }
    constexpr size_t kCgFlagOffset = 0x80000;
    for (const auto& slot : CgSlots()) {
        for (int id : slot) {
            const size_t offset = kCgFlagOffset + (size_t)id;
            if (offset < data.size() && data[offset] && unlockedCgs_.insert(id).second) {
                ++importedCgs;
                systemDirty_ = true;
            }
        }
    }
    Log(LogLevel::Info, "system: imported original SYSTEM.sav flags=%zu cg=%zu",
        importedFlags, importedCgs);
}

void Engine::MergeProgressFromSaves() {
    size_t merged = 0;
    for (int slot = 0; slot < kSaveSlots; ++slot) {
        const std::vector<uint8_t> data = ReadFileAll(SlotPath(slot));
        if (data.empty()) continue;
        SaveData sav;
        sav.Reset();
        ByteReader in(data);
        if (!sav.Load(in)) continue;
        const size_t count = std::min(sysFlags_.size(), sav.sysFlags.size());
        for (size_t i = 0; i < count; ++i) {
            const uint8_t value = (uint8_t)(sysFlags_[i] | sav.sysFlags[i]);
            if (value != sysFlags_[i]) {
                sysFlags_[i] = value;
                ++merged;
                systemDirty_ = true;
            }
        }
    }
    if (merged) Log(LogLevel::Info, "system: merged %zu progress flags from save slots", merged);
}

void Engine::Shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    Log(LogLevel::Info, "engine: shutdown begin initialized=%d",
        initialized_ ? 1 : 0);
    if (initialized_ && !suppressPersistence_) {
        SaveConfigFile();
        if (systemDirty_) SaveSystemFile();
    }
    Log(LogLevel::Info, "engine: shutdown video begin");
    video_.Shutdown();
    Log(LogLevel::Info, "engine: shutdown video complete");
    Log(LogLevel::Info, "engine: shutdown audio begin");
    audio_.Shutdown();
    Log(LogLevel::Info, "engine: shutdown audio complete");
    Log(LogLevel::Info, "engine: shutdown gfx begin");
    gfx_.Shutdown();
    initialized_ = false;
    Log(LogLevel::Info, "engine: shutdown gfx complete");
    LogFlush();
}

} // namespace wa2
