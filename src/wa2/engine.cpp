// engine.cpp — 引擎宿主实现
#include "engine.h"
#include "util.h"
#include "funcs.h"

#include <SDL2/SDL.h>
#ifdef __SWITCH__
#include <switch.h>   // appletMainLoop: 正确的 Switch 生命周期
#endif
#include <algorithm>
#include <cmath>
#include <cstdlib>   // getenv(WA2_REAL / WA2_REAL_START)

namespace wa2 {

// 文本窗布局：对应 wa2-godot/AdvMain.tscn 的原版 1280x720 坐标。
static const int kWinX = 16, kWinY = 505, kWinW = 1248, kWinH = 208;
static const int kTextX = 278, kTextY = 562, kNameX = 277, kNameY = 524;
static const int kNameSize = 28, kTextSize = 32;

bool Engine::Init(const std::string& dataDirOverride) {
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
    Log(LogLevel::Info, "engine: save dir = %s", saveDir_.c_str());
    if (res_.UsesPatchFont() && !gfx_.EnablePatchFont(res_)) return false;
    if (!audio_.Init()) return false;
    if (!video_.Init(gfx_.renderer())) return false;
    audio_.SetVolumes(config_.bgmVolume, config_.seVolume, config_.voiceVolume);

    // 原版标题场景自己负责 Logo/背景动画；启动阶段只保留极短黑屏。
    logoUntil_ = 0.15f;
    title_ = SDL_GetTicks();
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
    // 无窗口焦点依赖的标题菜单回归入口；普通运行未设置环境变量时完全不启用。
    int titleTestTarget = -1, titleTestMoves = 0;
    if (const char* test = std::getenv("WA2_TEST_CHAPTER")) {
        int v = std::atoi(test);
        if (v >= 0 && v <= 2) titleTestTarget = v;
    }
#ifdef __SWITCH__
    while (state_ != State::Quit && appletMainLoop()) {
#else
    while (state_ != State::Quit) {
#endif
        uint32_t frameStart = SDL_GetTicks();
        uint32_t now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;

        TickInput();
        audio_.Update();
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
        video_.Update();
        if (wait_.movie && !video_.Playing()) wait_.movie = false;
        UpdateAnims(dt);

        if (state_ == State::Logo && now / 1000.0f >= logoUntil_) {
            state_ = State::Title;
            ui_ = UiMode::Title;
            uiCursor_ = 0;
        }
        if (state_ == State::Title && !titleBgmStarted_) {
            audio_.PlayBgm(31, true, config_.bgmVolume, res_);
            titleBgmStarted_ = true;
        }

        // 脚本节拍 30Hz
        if (state_ == State::Game) {
            scriptAcc_ += dt;
            while (scriptAcc_ >= kScriptTick) {
                scriptAcc_ -= kScriptTick;
                TickScript(kScriptTick);
            }
        }

        Render();
        gfx_.Present();

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
                if (Edge(&lHeld, true))
                    ui_ = ui_ == UiMode::Backlog ? UiMode::None : UiMode::Backlog;
            } else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                if (Edge(&rHeld, true)) skipMode_ = !skipMode_;
            } else if (button == SDL_CONTROLLER_BUTTON_START) {
                if (Edge(&stHeld, true))
                    ui_ = ui_ == UiMode::Menu ? UiMode::None
                                              : (state_ == State::Game ? UiMode::Menu : UiMode::None);
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
        else if (ev.type == SDL_MOUSEBUTTONDOWN) clicked_ = true;
        else if (ev.type == SDL_FINGERDOWN) clicked_ = true;
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
            else if (b == SDLK_a) Edge(&yHeld, true);
            else if (b == SDLK_q) { if (Edge(&lHeld, true)) ui_ = ui_ == UiMode::Backlog ? UiMode::None : UiMode::Backlog; }
            else if (b == SDLK_w) { if (Edge(&rHeld, true)) skipMode_ = !skipMode_; }
            else if (b == SDLK_TAB) { if (Edge(&stHeld, true)) ui_ = ui_ == UiMode::Menu ? UiMode::None : (state_ == State::Game ? UiMode::Menu : UiMode::None); }
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
    // 立绘透明度
    for (auto& c : chars_) {
        if (c.fadePerSec > 0) {
            if (c.alpha < c.targetAlpha) c.alpha = std::min(c.targetAlpha, c.alpha + c.fadePerSec * dt);
            else if (c.alpha > c.targetAlpha) c.alpha = std::max(c.targetAlpha, c.alpha - c.fadePerSec * dt);
        }
    }
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
    if (wait_.timer && nowS >= wait_.timerUntil) wait_.timer = false;
    if (wait_.animBusy && nowS >= wait_.animUntil) wait_.animBusy = false;
    // 打字机
    if (wait_.textBusy && adv_.visible) {
        int speed = 1 + (3 - config_.textSpeed) * 2;   // 帧/字
        if (SDL_GetTicks() - adv_.lastCharMs >= (uint32_t)(speed * 1000 / 60)) {
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
            autoTimer_ = remain + (1.0f + config_.autoSpeed * 0.5f);
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
    if (skipMode_ && state_ == State::Game && !skipDisable_ && CanSkip()) {
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
    if (wait_.Blocking()) {
        // 选项的确认/方向输入由 RenderSelect 消费，不能在脚本等待门里清掉。
        if (wait_.selectVisible) return;
        // 自动/跳过/点击都汇聚到 ClickAdvance
        if ((clicked_ || skipMode_)) {
            if (wait_.calender) wait_.calender = false;
            else if (!wait_.selectVisible && !wait_.menu) ClickAdvance();
        }
        clicked_ = false;
        return;
    }
    if (skipMode_ && CanSkip()) ClickAdvance();

    Script* s = Active();
    if (!s) {
        state_ = State::Title;
        ui_ = UiMode::Title;
        return;
    }
    ApplyPending();
    clicked_ = false;
    TickResult r = s->Tick(*this);
    ApplyPending();
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
            state_ = State::Title;
            ui_ = UiMode::Title;
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
    for (auto& c : chars_) { c.show = false; c.alpha = 0.f; c.targetAlpha = 0.f; c.fadePerSec = 0.f; } // 复位渲染立绘槽(否则多轮后无空槽→立绘消失)
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
    state_ = State::Title;
    ui_ = UiMode::Title;
    // 回标题彻底复位音频(BGM+SE+语音),避免多次运行间音频通道残留/腐蚀
    audio_.StopAll();
    video_.Stop();
    titleBgmStarted_ = false;
    // 延迟销毁:当前脚本还在执行其 Tick,直接 clear 会 use-after-free
    // (gotitle 是当前脚本自己触发的宿主调用)
    for (auto& s : stack_) graveyard_.push_back(std::move(s));
    stack_.clear();
    for (auto& c : chars_) { c.show = false; c.alpha = 0.f; c.targetAlpha = 0.f; c.fadePerSec = 0.f; } // 复位立绘槽
    fb_ = {};
    gfx_.ClearCache();   // 回标题时释放当前场景纹理
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
    if (idx >= 0 && idx < kSysFlagCount) sysFlags_[idx] = (uint8_t)v;
}
int Engine::ReadGameFlag(int idx) {
    return gameFlags_[idx];
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
        // 续写:\k 后接续
        if (!adv_.segments.empty()) {
            adv_.segments.back() += clean;
        }
    } else {
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
        // 已读标记
        WriteSysFlag(800000 + msgIdx, 1);
    }
    wait_.textBusy = true;
    wait_.waitClick = false;
}

void Engine::EndMessage() {
    audio_.StopVoice(0, 0);
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
void Engine::SetDemoMode(bool v) { demoMode_ = v; }
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
        if (!keepChar) {
            for (auto& c : chars_) if (c.show) { c.targetAlpha = 0; c.fadePerSec = 999.0f; }
        }
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
    if (!keepChar) {
        for (auto& c : chars_) {
            if (c.show) { c.targetAlpha = 0; c.fadePerSec = 1.0f / secs; }
        }
    }
}

void Engine::RenderImage(int id, int efc, bool keepChar, int type, int frame,
                         int offset, int x, int y, float sx, float sy) {
    std::string path;
    if (id >= 0) {
        if (type == 1) {
            path = Res::CgName(id);
            WriteSysFlag(id, 1);   // CG 收藏
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
    SetupNewBg(path, frame, x, y, offset, sx, sy, keepChar);
}

void Engine::AddChar(int id, int no, int pos) {
    scene_.AddOrUpdateChar(id, no, pos);
    for (auto& c : chars_) {
        if (c.show && c.pos == pos) { c.id = id; c.no = no; return; }
    }
    for (auto& c : chars_) {
        if (!c.show) {
            c.show = true; c.id = id; c.no = no; c.pos = pos;
            c.alpha = 0; c.targetAlpha = 1; c.fadePerSec = 0;   // UpdateChar 设定速度
            return;
        }
    }
}

void Engine::UpdateChar(int frames) {
    if (frames > 300) Log(LogLevel::Warn, "engine: unusually long char animation: %d frames", frames);
    float secs = frames * kFrameTime;
    float speed = 1.0f / (secs > 0.01f ? secs : 0.01f);
    for (auto& c : chars_) {
        if (!c.show) continue;
        if (c.alpha < 1) { c.targetAlpha = 1; c.fadePerSec = speed; }
    }
    MarkAnim(secs);
}

void Engine::RemoveChar(int pos) {
    scene_.RemoveCharAt(pos);
    for (auto& c : chars_) {
        if (c.show && c.pos == pos) { c.targetAlpha = 0; c.fadePerSec = 0; }
    }
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
    if (FileExists(path) && video_.Play(path, config_.bgmVolume)) {
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
    // v1:全部视为已读(配置项 skipUnread 决定未读是否可跳)
    (void)config_;
    return !skipDisable_;
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
    audio_.PlayBgm(id, loop, vol, res_);
}
void Engine::StopBgm(int fadeFrames) {
    audio_.StopBgm((int)(fadeFrames * kFrameTime * 1000));
}
void Engine::SetVoiceLabel(int label) { voiceLabel_ = label; }
int Engine::CurrentVoiceLabel() const { return voiceLabel_; }
void Engine::PlayVoice(int label, int id, int ch, bool loop, int track) {
    (void)track;
    audio_.PlayVoice(label, id, ch, loop, res_);
}
void Engine::WaitVoice(int ch) {
    float remain = audio_.VoiceRemaining(ch);
    if (remain > 0) {
        wait_.timer = true;
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
        wait_.timerUntil = SDL_GetTicks() / 1000.0f + remain;
    }
}

// ---------------- Host:定时 ----------------
void Engine::WaitMs(float ms) {
    wait_.timer = true;
    wait_.timerUntil = SDL_GetTicks() / 1000.0f + ms / 1000.0f;
}
void Engine::StartTimer() { timerStart_ = SDL_GetTicks(); }
int Engine::ElapsedTimerMs() { return (int)(SDL_GetTicks() - timerStart_); }

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
        const std::string& seg = adv_.segments.empty() ? adv_.text : adv_.segments[adv_.seg];
        gfx_.DrawTextTyped(seg, 120, 80, kTextSize, adv_.shown, 255, 255, 255);
        return;
    }
    // 原版窗口由两层 sys_ 图组成，而不是临时黑色矩形。
    Tex* window = gfx_.Get("sys_00001.tga", res_, "");
    Tex* frame = gfx_.Get("sys_00000.tga", res_, "");
    if (window) gfx_.DrawTexture(window, kWinX, kWinY, kWinW, kWinH, 0.50f);
    if (frame) gfx_.DrawTexture(frame, kWinX, kWinY, kWinW, kWinH, 1.0f);
    if (!window && !frame)
        gfx_.FillRect(kWinX, kWinY, kWinW, kWinH, 0, 22, 34, 210);
    // 名字
    if (!adv_.name.empty()) {
        gfx_.DrawText(adv_.name, kNameX, kNameY, kNameSize, 255, 255, 255);
    }
    // 正文(逐字)
    const std::string& seg = adv_.segments.empty() ? adv_.text : adv_.segments[adv_.seg];
    int maxChars = wait_.textBusy ? adv_.shown : -1;
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
    startScript_ = scriptName;
    state_ = State::Game;
    ui_ = UiMode::None;
    SLoadScript(scriptName, 0);
}

void Engine::RenderUi() {
    if (ui_ == UiMode::None) return;

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
                case 2: ui_ = UiMode::Config; uiCursor_ = 0; break;
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
                else if (idx == 5) { ui_ = UiMode::Title; uiCursor_ = 0; }
                else Log(LogLevel::Info, "title: special menu item %d not wired yet", idx);
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
        for (int i = 0; i < (int)items.size(); i++)
            centerText(items[i], 200 + i * 64, 32, i == uiCursor_);
        HandleMenuInput((int)items.size(), true, [this](int idx) {
            switch (idx) {
            case 0: ui_ = UiMode::None; break;
            case 1: ui_ = UiMode::Save; uiCursor_ = 0; break;
            case 2: ui_ = UiMode::Load; uiCursor_ = 0; break;
            case 3: ui_ = UiMode::Config; uiCursor_ = 0; break;
            case 4: GoTitle(); break;
            case 5: state_ = State::Quit; break;
            }
        });
    } else if (ui_ == UiMode::Save || ui_ == UiMode::Load) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0, 180);
        gfx_.DrawText(ui_ == UiMode::Save ? "存档" : "读档", 80, 60, 40, 255, 255, 255);
        for (int i = 0; i < kSaveSlots; i++) {
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
    } else if (ui_ == UiMode::Config) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0, 180);
        gfx_.DrawText("设置(←→ 调整,A 确认,B 返回)", 80, 50, 30, 255, 255, 255);
        std::vector<std::string> rows = {
            Format("文字速度  %d", config_.textSpeed),
            Format("自动速度  %d", config_.autoSpeed),
            Format("BGM 音量  %d", config_.bgmVolume),
            Format("SE  音量  %d", config_.seVolume),
            Format("语音音量  %d", config_.voiceVolume),
        };
        for (int i = 0; i < (int)rows.size(); i++)
            gfx_.DrawText(rows[i], 140, 120 + i * 70, 30, i == uiCursor_ ? 255 : 170,
                          i == uiCursor_ ? 220 : 170, i == uiCursor_ ? 140 : 170);
        ConfigAdjustInput((int)rows.size());
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
    if (ui_ == UiMode::TitleNovel) {
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
    const int d = navX_ < 0 ? -1 : navX_ > 0 ? 1 : 0;
    navX_ = navY_ = 0;
    if (d != 0) {
        switch (uiCursor_) {
        case 0: config_.textSpeed = (config_.textSpeed + d + 4) % 4; break;
        case 1: config_.autoSpeed = (config_.autoSpeed + d + 4) % 4; break;
        case 2: config_.bgmVolume = std::clamp(config_.bgmVolume + d * 16, 0, 255); break;
        case 3: config_.seVolume = std::clamp(config_.seVolume + d * 16, 0, 255); break;
        case 4: config_.voiceVolume = std::clamp(config_.voiceVolume + d * 16, 0, 255); break;
        }
        audio_.SetVolumes(config_.bgmVolume, config_.seVolume, config_.voiceVolume);
        SaveConfigFile();
    }
    if (clicked_ || cancelClicked_) {
        clicked_ = cancelClicked_ = false;
        CancelUi();
    }
}

// ---------------- 存档 ----------------
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
    scene_.Save(eb);
    eb.I32((int32_t)stack_.size());
    for (auto& s : stack_) s->Save(eb);
    eb.I32(timeMode_);
    eb.Str(effectMode_);
    eb.I32(voiceLabel_);
    sav->engineBlock = eb.data();
}

void Engine::ApplySav(const SaveData& sav) {
    gameFlags_ = sav.gameFlags;
    gameFlags_.resize(kMaxGameFlags, 0);
    sysFlags_ = sav.sysFlags;
    sysFlags_.resize(kSysFlagCount, 0);
    ByteReader in(sav.engineBlock.data(), sav.engineBlock.size());
    if (!scene_.Load(in)) return;
    int32_t n = in.I32();
    for (auto& s : stack_) graveyard_.push_back(std::move(s));
    stack_.clear();
    for (int32_t i = 0; i < n; i++) {
        auto s = std::make_unique<Script>();
        s->SetGameFlags(&gameFlags_);
        if (!s->LoadState(in, res_)) return;
        stack_.push_back(std::move(s));
    }
    timeMode_ = in.I32();
    effectMode_ = in.Str();
    voiceLabel_ = in.I32();
    wait_.Clear();
}

bool Engine::SaveToSlotFile(int slot) {
    SaveData sav;
    BuildSav(&sav);
    ByteBuf out;
    sav.Save(out);
    return WriteFileAll(SlotPath(slot), out.data().data(), out.data().size());
}

bool Engine::LoadFromSlotFile(int slot) {
    std::vector<uint8_t> data = ReadFileAll(SlotPath(slot));
    if (data.empty()) return false;
    SaveData sav;
    sav.Reset();
    ByteReader in(data.data(), data.size());
    if (!sav.Load(in)) return false;
    ApplySav(sav);
    audio_.SetVolumes(config_.bgmVolume, config_.seVolume, config_.voiceVolume);
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
    ByteBuf out;
    config_.Save(out);
    WriteFileAll(PathJoin(saveDir_, "config.bin"), out.data().data(), out.data().size());
}

void Engine::Shutdown() {
    SaveConfigFile();
    video_.Shutdown();
    audio_.Shutdown();
    gfx_.Shutdown();
    LogFlush();
}

} // namespace wa2
