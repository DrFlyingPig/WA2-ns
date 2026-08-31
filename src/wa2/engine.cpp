// engine.cpp — 引擎宿主实现
#include "engine.h"
#include "util.h"
#include "funcs.h"

#include <SDL2/SDL.h>
#ifdef __SWITCH__
#include <switch.h>   // appletMainLoop: 正确的 Switch 生命周期
#endif
#include <algorithm>

namespace wa2 {

// 文本窗布局
static const int kWinX = 40, kWinY = 520, kWinW = 1200, kWinH = 164;
static const int kNameSize = 28, kTextSize = 32;

bool Engine::Init(const std::string& dataDirOverride) {
    gameFlags_.assign(kMaxGameFlags, 0);
    sysFlags_.assign(kSysFlagCount, 0);

    // 数据目录:显式指定 > sdmc:/wa2 > Wa2Res > out/Wa2Res > romfs demo
    if (!dataDirOverride.empty()) dataDir_ = dataDirOverride;
    else {
#ifdef __SWITCH__
        if (FileExists("sdmc:/wa2/game.ini")) dataDir_ = "sdmc:/wa2";
        else dataDir_ = "romfs:/demo";
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
        PathJoin(dataDir_, "saves");
#endif
    Log(LogLevel::Info, "engine: data dir = %s", dataDir_.c_str());

    LoadConfigFile();
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
    if (!audio_.Init()) return false;
    audio_.SetVolumes(config_.bgmVolume, config_.seVolume, config_.voiceVolume);

    logoUntil_ = 1.5f;
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
    uint32_t last = SDL_GetTicks();
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
        UpdateAnims(dt);

        if (state_ == State::Logo && now / 1000.0f >= logoUntil_) { state_ = State::Title; ui_ = UiMode::Title; }

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

        // 帧节奏:软件渲染器无可用的 VSYNC 时(退回 flags=0),全速空转会烧满 CPU 造成卡顿。
        // 统一把帧长补到 ~60fps,既有 VSYNC 时也安全(等待几乎为零)。
        uint32_t used = SDL_GetTicks() - frameStart;
        if (used < 16) SDL_Delay(16 - used);
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
    static bool aHeld = false, bHeld = false, yHeld = false, lHeld = false,
                rHeld = false, stHeld = false;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) state_ = State::Quit;
        else if (ev.type == SDL_APP_TERMINATING) state_ = State::Quit;
        else if (ev.type == SDL_MOUSEBUTTONDOWN) clicked_ = true;
        else if (ev.type == SDL_FINGERDOWN) clicked_ = true;
        else if (ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_KEYDOWN) {
            int b = ev.type == SDL_KEYDOWN ? ev.key.keysym.sym : ev.jbutton.button;
            if (b == SDLK_RETURN || b == SDL_CONTROLLER_BUTTON_A || b == SDLK_z) { if (Edge(&aHeld, true)) clicked_ = true; }
            else if (b == SDLK_ESCAPE || b == SDL_CONTROLLER_BUTTON_B || b == SDLK_x) { if (Edge(&bHeld, true)) clicked_ = true; }
            else if (b == SDL_CONTROLLER_BUTTON_Y || b == SDLK_a) Edge(&yHeld, true);
            else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || b == SDLK_q) { if (Edge(&lHeld, true)) ui_ = ui_ == UiMode::Backlog ? UiMode::None : UiMode::Backlog; }
            else if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER || b == SDLK_w) { if (Edge(&rHeld, true)) skipMode_ = !skipMode_; }
            else if (b == SDL_CONTROLLER_BUTTON_START || b == SDLK_TAB) { if (Edge(&stHeld, true)) ui_ = ui_ == UiMode::Menu ? UiMode::None : (state_ == State::Game ? UiMode::Menu : UiMode::None); }
        } else if (ev.type == SDL_JOYBUTTONUP || ev.type == SDL_KEYUP) {
            int b = ev.type == SDL_KEYUP ? ev.key.keysym.sym : ev.jbutton.button;
            if (b == SDLK_RETURN || b == SDL_CONTROLLER_BUTTON_A || b == SDLK_z) Edge(&aHeld, false);
            else if (b == SDLK_ESCAPE || b == SDL_CONTROLLER_BUTTON_B || b == SDLK_x) Edge(&bHeld, false);
            else if (b == SDL_CONTROLLER_BUTTON_Y || b == SDLK_a) Edge(&yHeld, false);
            else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || b == SDLK_q) Edge(&lHeld, false);
            else if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER || b == SDLK_w) Edge(&rHeld, false);
            else if (b == SDL_CONTROLLER_BUTTON_START || b == SDLK_TAB) Edge(&stHeld, false);
        }
    }
    // Y = 自动模式开关
    static bool prevAuto = false;
    if (yHeld != prevAuto) { prevAuto = yHeld; if (yHeld) { autoMode_ = !autoMode_; autoTimer_ = -1; } }
}

// ---------------- 动画 ----------------
void Engine::MarkAnim(float seconds) {
    if (seconds > wait_.animUntil) wait_.animUntil = seconds;
    wait_.animBusy = seconds > 0;
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
    // 立绘透明度
    for (auto& c : chars_) {
        if (c.fadePerSec > 0) {
            if (c.alpha < c.targetAlpha) c.alpha = std::min(c.targetAlpha, c.alpha + c.fadePerSec * dt);
            else if (c.alpha > c.targetAlpha) c.alpha = std::max(c.targetAlpha, c.alpha - c.fadePerSec * dt);
        }
    }
    // FB 覆盖
    if (fb_.fadePerSec > 0) {
        if (fb_.alpha < fb_.targetAlpha) fb_.alpha = std::min(fb_.targetAlpha, fb_.alpha + fb_.fadePerSec * dt);
        else if (fb_.alpha > fb_.targetAlpha) fb_.alpha = std::max(fb_.targetAlpha, fb_.alpha - fb_.fadePerSec * dt);
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
            if (adv_.shown >= (int)adv_.segments[adv_.seg].size()) {
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
        adv_.shown = (int)adv_.segments[adv_.seg].size();
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
    if (r == TickResult::End) {
        // 弹栈;底层脚本继续或回标题
        stack_.pop_back();
        if (stack_.empty()) {
            state_ = State::Title;
            ui_ = UiMode::Title;
        }
    }
}

// ---------------- Host:流程 ----------------
void Engine::SLoadScript(const std::string& name, int point) {
    for (auto& s : stack_) graveyard_.push_back(std::move(s));
    stack_.clear();
    scene_.ClearChars();
    scene_.selectItems.clear();
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
    s->Load(res_, name, point);
    stack_.push_back(std::move(s));
}

void Engine::CallPoint(int point) {
    Script* s = Active();
    if (!s) return;
    // 重新以指定入口载入同脚本
    std::string name = s->name();
    for (auto& old : stack_) graveyard_.push_back(std::move(old));
    stack_.clear();
    auto s2 = std::make_unique<Script>();
    s2->SetGameFlags(&gameFlags_);
    s2->Load(res_, name, point);
    stack_.push_back(std::move(s2));
}

void Engine::GoTitle() {
    state_ = State::Title;
    ui_ = UiMode::Title;
    audio_.StopBgm(500);
    stack_.clear();
    graveyard_.clear();
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

    // 过渡:截屏 → 交叉淡化到新背景
    trans_.snap = gfx_.CaptureScreen();
    trans_.active = true;
    trans_.t = 0;
    trans_.dur = frame * kFrameTime;
    float secs = trans_.dur;
    MarkAnim(secs);
    (void)oldPath;
    if (!keepChar) {
        for (auto& c : chars_) {
            if (c.show) { c.targetAlpha = 0; c.fadePerSec = 1.0f / (secs > 0.01f ? secs : 0.01f); }
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
    bg_.x = (float)dx;
    bg_.y = (float)dy;
    MarkAnim(frames * kFrameTime);
}

void Engine::ColorFade(int r, int g, int b, int frames) {
    fb_.r = (uint8_t)r; fb_.g = (uint8_t)g; fb_.b = (uint8_t)b;
    if (frames > 0) {
        fb_.targetAlpha = 1;
        fb_.fadePerSec = 1.0f / (frames * kFrameTime);
        MarkAnim(frames * kFrameTime);
    } else {
        fb_.alpha = fb_.targetAlpha = 1;
        fb_.fadePerSec = 0;
    }
}

void Engine::SetFBColor(int r, int g, int b) {
    fb_.r = (uint8_t)r; fb_.g = (uint8_t)g; fb_.b = (uint8_t)b;
}

void Engine::Shake(int type, int power, int frames) {
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
    (void)movieId;
    // v1:影片以"黑屏等待"占位
    Log(LogLevel::Info, "engine: movie %d skipped (flag %d)", movieId, flagIdx);
    WaitMs(500);
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
    selectBase_ = 4 * 0 + 900;   // v1:固定基址(按脚本扩展见 docs)
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

    if (state_ == State::Logo) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 0, 0, 0, 255);
        gfx_.DrawText("WA2-ns", kVirtualW / 2 - 90, kVirtualH / 2 - 40, 48, 255, 255, 255);
        gfx_.Present();
        return;
    }

    // 背景(或过渡)
    Tex* bgTex = bg_.path.empty() ? nullptr : gfx_.Get(bg_.path, res_, effectMode_);
    if (bgTex) {
        gfx_.DrawTextureFit(bgTex, kVirtualW / 2.0f + bg_.x, kVirtualH / 2.0f + bg_.y,
                            bg_.sx, bg_.sy, 1.0f);
    }
    if (trans_.active && trans_.snap) {
        float a = 1.0f - trans_.t / (trans_.dur > 0 ? trans_.dur : 0.01f);
        SDL_Rect dst{0, 0, kVirtualW, kVirtualH};
        SDL_SetTextureAlphaMod(trans_.snap, (uint8_t)(a * 255));
        SDL_RenderCopy(gfx_.renderer(), trans_.snap, nullptr, &dst);
    }

    // 立绘(按 kCharOrder 远→近)
    for (int oi = 0; oi < kMaxChars; oi++) {
        CharDraw& c = chars_[kCharOrder[oi]];
        if (!c.show || c.alpha <= 0) continue;
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

    // FB 色彩覆盖
    if (fb_.alpha > 0.001f) {
        gfx_.FillRect(ox, oy, kVirtualW, kVirtualH, fb_.r, fb_.g, fb_.b, (uint8_t)(fb_.alpha * 255));
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

void Engine::RenderAdvWindow() {
    if (!adv_.visible || adv_.hide) return;
    int alpha = 200;
    if (scene_.novelMode) {
        // 小说模式:无框居中
        const std::string& seg = adv_.segments.empty() ? adv_.text : adv_.segments[adv_.seg];
        gfx_.DrawTextTyped(seg, 120, 80, kTextSize, adv_.shown, 255, 255, 255);
        return;
    }
    // 窗框
    gfx_.FillRect(kWinX, kWinY, kWinW, kWinH, 0, 0, 0, alpha);
    gfx_.FillRect(kWinX, kWinY, kWinW, 2, 255, 255, 255, 120);
    // 名字
    if (!adv_.name.empty()) {
        gfx_.DrawText(adv_.name, kWinX + 24, kWinY + 10, kNameSize, 255, 230, 150);
    }
    // 正文(逐字)
    const std::string& seg = adv_.segments.empty() ? adv_.text : adv_.segments[adv_.seg];
    int maxChars = wait_.textBusy ? adv_.shown : -1;
    // 简单折行:手动按宽度断行
    int x = kWinX + 24, y = kWinY + 52;
    int lineWidth = kWinW - 48;
    int drawn = 0;
    size_t i = 0;
    while (i < seg.size()) {
        // 找出能放进一行的最大字符数
        size_t j = i;
        int w = 0;
        size_t lineEnd = seg.size();
        for (; j < seg.size();) {
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
        }
        int avail = maxChars < 0 ? (int)(seg.size() - i) : maxChars - drawn;
        if (avail <= 0) break;
        std::string part = seg.substr(i, std::min(lineEnd, i + (size_t)avail) - i);
        int n = gfx_.DrawTextTyped(part, x, y, kTextSize, avail, 255, 255, 255);
        drawn += n;
        if (lineEnd < seg.size() && drawn < (maxChars < 0 ? 999999 : maxChars)) {
            i = lineEnd;
            y += gfx_.LineHeight(kTextSize);
            if (y > kWinY + kWinH - 20) break;
        } else {
            break;
        }
    }
    // 下一段指示
    if (!wait_.textBusy && adv_.seg + 1 < (int)adv_.segments.size()) {
        gfx_.FillRect(kWinX + kWinW - 30, kWinY + kWinH - 24, 14, 14, 255, 255, 0, 200);
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
        gfx_.FillRect(kVirtualW / 2 - boxW / 2, y, boxW, boxH,
                      cur ? 60 : 20, cur ? 60 : 20, cur ? 100 : 30, 220);
        gfx_.DrawText(scene_.selectItems[i].text,
                      kVirtualW / 2 - boxW / 2 + 24, y + 16, 28,
                      cur ? 255 : 200, cur ? 230 : 200, cur ? 180 : 200);
    }
    // 选择操作
    static bool upHeld = false, downHeld = false, okHeld = false;
    SDL_PumpEvents();
    const uint8_t* kb = SDL_GetKeyboardState(nullptr);
    bool up = kb[SDL_SCANCODE_UP] || kb[SDL_SCANCODE_W];
    bool down = kb[SDL_SCANCODE_DOWN] || kb[SDL_SCANCODE_S];
    bool ok = kb[SDL_SCANCODE_RETURN] || kb[SDL_SCANCODE_SPACE];
    if (Edge(&upHeld, up)) selectCursor_ = (selectCursor_ + n - 1) % n;
    if (Edge(&downHeld, down)) selectCursor_ = (selectCursor_ + 1) % n;
    if (clicked_ || Edge(&okHeld, ok)) {
        clicked_ = false;
        Script* s = Active();
        if (s && !s->args.empty()) {
            Val v;
            v.kind = Val::Int;
            v.i = selectCursor_;
            s->SetVar(s->args.back(), v);
            // 已读标记(sys flag)
            WriteSysFlag(selectBase_ + ReadGameFlag(100) * 4, ReadSysFlag(selectBase_) | (1 << selectCursor_));
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

void Engine::RenderUi() {
    if (ui_ == UiMode::None) return;

    auto centerText = [&](const std::string& s, int y, int size, bool sel) {
        int w = gfx_.TextWidth(s, size);
        gfx_.DrawText(s, kVirtualW / 2 - w / 2, y, size,
                      sel ? 255 : 160, sel ? 220 : 160, sel ? 140 : 160);
    };

    if (ui_ == UiMode::Title) {
        gfx_.FillRect(0, 0, kVirtualW, kVirtualH, 10, 14, 30, 255);
        gfx_.DrawText("WHITE ALBUM2", kVirtualW / 2 - 220, 140, 52, 240, 240, 255);
        gfx_.DrawText("WA2-ns Switch Port (Tech Preview)", kVirtualW / 2 - 200, 210, 24, 150, 150, 170);
        std::vector<std::string> items = {"开始游戏", "读取存档", "设置", "退出"};
        for (int i = 0; i < (int)items.size(); i++)
            centerText(items[i], 320 + i * 70, 34, i == uiCursor_);
        HandleMenuInput((int)items.size(), false, [this, &items](int idx) {
            switch (idx) {
            case 0: {
                state_ = State::Game;
                ui_ = UiMode::None;
                SLoadScript(startScript_, 0);
                break;
            }
            case 1: ui_ = UiMode::Load; uiCursor_ = 0; break;
            case 2: ui_ = UiMode::Config; uiCursor_ = 0; break;
            case 3: state_ = State::Quit; break;
            }
        });
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
    static bool upHeld = false, downHeld = false, okHeld = false, cancelHeld = false;
    SDL_PumpEvents();
    const uint8_t* kb = SDL_GetKeyboardState(nullptr);
    bool up = kb[SDL_SCANCODE_UP], down = kb[SDL_SCANCODE_DOWN];
    bool ok = kb[SDL_SCANCODE_RETURN] || kb[SDL_SCANCODE_SPACE] || kb[SDL_SCANCODE_Z];
    bool cancel = kb[SDL_SCANCODE_ESCAPE] || kb[SDL_SCANCODE_X];
    bool act = false;
    if (Edge(&upHeld, up)) uiCursor_ = (uiCursor_ + count - 1) % count;
    if (Edge(&downHeld, down)) uiCursor_ = (uiCursor_ + 1) % count;
    if (clicked_) { clicked_ = false; act = true; *outIdx = uiCursor_; }
    else if (Edge(&okHeld, ok)) { act = true; *outIdx = uiCursor_; }
    if (allowCancel && (Edge(&cancelHeld, cancel) || (cancel && Edge(&cancelHeld, true)))) {
        *canceled = true;
        ui_ = UiMode::None;
        return false;
    }
    if (act) *outIdx = uiCursor_;
    return act;
}

void Engine::BacklogInput() {
    static bool upHeld = false, downHeld = false;
    SDL_PumpEvents();
    const uint8_t* kb = SDL_GetKeyboardState(nullptr);
    if (Edge(&upHeld, (bool)kb[SDL_SCANCODE_UP])) backlogScroll_++;
    if (Edge(&downHeld, (bool)kb[SDL_SCANCODE_DOWN])) backlogScroll_ = std::max(0, backlogScroll_ - 1);
}

void Engine::ConfigAdjustInput(int count) {
    static bool leftHeld = false, rightHeld = false, okHeld = false, cancelHeld = false,
                upHeld = false, downHeld = false;
    SDL_PumpEvents();
    const uint8_t* kb = SDL_GetKeyboardState(nullptr);
    if (Edge(&upHeld, (bool)kb[SDL_SCANCODE_UP])) uiCursor_ = (uiCursor_ + count - 1) % count;
    if (Edge(&downHeld, (bool)kb[SDL_SCANCODE_DOWN])) uiCursor_ = (uiCursor_ + 1) % count;
    int d = 0;
    if (Edge(&leftHeld, (bool)kb[SDL_SCANCODE_LEFT])) d = -1;
    if (Edge(&rightHeld, (bool)kb[SDL_SCANCODE_RIGHT])) d = 1;
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
    if (Edge(&okHeld, (bool)(kb[SDL_SCANCODE_RETURN] || kb[SDL_SCANCODE_Z])) ||
        Edge(&cancelHeld, (bool)kb[SDL_SCANCODE_X])) {
        ui_ = UiMode::None;
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
    audio_.Shutdown();
    gfx_.Shutdown();
    LogFlush();
}

} // namespace wa2
