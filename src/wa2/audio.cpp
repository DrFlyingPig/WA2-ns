// audio.cpp — SDL2_mixer 实现
#include "audio.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <atomic>
#include <cstdlib>

namespace wa2 {

static std::atomic<Audio*> g_audio{nullptr}; // Mix_HookMusicFinished 需要静态入口

static void OnMusicFinished() {
    // 此回调可从 SDL 音频线程调用，也会在 Mix_HaltMusic 内部
    // 持有音频锁时同步调用。绝对不能在这里再 Play/Halt/Free。
    if (Audio* audio = g_audio.load(std::memory_order_acquire))
        audio->NotifyMusicFinished();
}

bool Audio::Init() {
    noAudio_ = std::getenv("WA2_NOAUDIO") != nullptr;
    if (noAudio_) return true;
    Mix_Init(MIX_INIT_OGG);   // 初始化需要的解码器(OGG);demo 用 WAV 亦无害
    if (Mix_OpenAudio(48000, MIX_DEFAULT_FORMAT, 2, 4096) != 0) {
        Log(LogLevel::Error, "audio: open failed: %s", Mix_GetError());
        return false;
    }
    Mix_AllocateChannels(32);
    // 通道布局:0-7 SE,8-11 语音,12 BGM 占位
    musicFinished_.store(false, std::memory_order_relaxed);
    g_audio.store(this, std::memory_order_release);
    Mix_HookMusicFinished(OnMusicFinished);
    return true;
}

void Audio::Shutdown() {
    if (noAudio_) return;
    // 先取消回调，再停止音乐；否则 Mix_HaltMusic 会再投递完成事件。
    Mix_HookMusicFinished(nullptr);
    g_audio.store(nullptr, std::memory_order_release);
    StopAll();
    Mix_CloseAudio();
    Mix_Quit();
}

void Audio::SetVolumes(int bgm, int se, int voice) {
    if (noAudio_) return;
    bgmVol_ = bgm; seVol_ = se; voiceVol_ = voice;
    Mix_VolumeMusic(bgmVol_);
    for (int i = 0; i < 8; i++) Mix_Volume(i, seVol_);
    for (int i = 0; i < 4; i++) Mix_Volume(8 + i, voiceVol_);
}

void Audio::NotifyMusicFinished() {
    musicFinished_.store(true, std::memory_order_release);
}

void Audio::Update() {
    if (noAudio_) return;
    if (!musicFinished_.exchange(false, std::memory_order_acq_rel)) return;

    // 主动停止/淡出也会触发 finished hook，不能误当成前奏自然结束。
    if (bgmStopping_) {
        FreeBgm();
        return;
    }

    // BGM A 段播完 → 无缝接 B 段循环
    if (bgmB_ && !bgmInLoopPart_) {
        bgmInLoopPart_ = true;
        if (Mix_PlayMusic(bgmB_, bgmLoop_ ? -1 : 0) != 0) {
            Log(LogLevel::Warn, "audio: bgm loop failed: %s", Mix_GetError());
        } else {
            Log(LogLevel::Info, "audio: bgm intro finished, playing B (loop=%d)",
                bgmLoop_ ? 1 : 0);
        }
    }
}

void Audio::FreeBgm() {
    if (noAudio_) return;
    // Mix_HaltMusic 会同步调用 finished hook。停止前后都清除标志，
    // 防止旧曲的停止事件在新曲加载后被当成“A 段播完”。
    musicFinished_.store(false, std::memory_order_release);
    Mix_HaltMusic();
    musicFinished_.store(false, std::memory_order_release);
    if (bgmA_) { Mix_FreeMusic(bgmA_); bgmA_ = nullptr; }
    if (bgmB_) { Mix_FreeMusic(bgmB_); bgmB_ = nullptr; }
    bgmAData_.clear();
    bgmBData_.clear();
    bgmInLoopPart_ = false;
    bgmStopping_ = false;
}

bool Audio::PlayBgm(int id, bool loop, int vol, Res& res) {
    if (noAudio_) return false;
    FreeBgm();
    bgmLoop_ = loop;
    Mix_VolumeMusic(vol * bgmVol_ / 255);
    // 单文件优先；否则使用零售版常见的 _a 前奏 + _b 循环段。
    std::string single = Res::BgmName(id, false);          // bgm_xxx.ogg
    std::string intro = Res::BgmIntroName(id);             // bgm_xxx_a.ogg
    std::string singleWav = single;
    if (singleWav.size() > 4) singleWav.replace(singleWav.size() - 4, 4, ".wav");
    std::string introWav = intro;
    if (introWav.size() > 4) introWav.replace(introWav.size() - 4, 4, ".wav");
    std::vector<std::string> cands = {single, intro, singleWav, introWav};
    ResLoc a = res.Find(cands);
    if (!a.found) {
        Log(LogLevel::Warn, "audio: bgm %d missing", id);
        return false;
    }
    Log(LogLevel::Info, "audio: bgm %d found %s", id, a.name.c_str());
    bgmAData_ = res.Load(a.name);
    SDL_RWops* rw = SDL_RWFromMem(bgmAData_.data(), (int)bgmAData_.size());
    bgmA_ = Mix_LoadMUS_RW(rw, 1);   // frees rw
    if (!bgmA_) {
        Log(LogLevel::Warn, "audio: bgm decode failed: %s", Mix_GetError());
        return false;
    }
    Log(LogLevel::Info, "audio: bgm %d music loaded", id);
    const bool splitTrack = a.name == intro || a.name == introWav;
    if (splitTrack) {
        std::string loopPart = Res::BgmName(id, true);      // bgm_xxx_b.ogg
        std::vector<std::string> lc = {loopPart};
        if (loopPart.size() > 4) { std::string w = loopPart; w.replace(w.size() - 4, 4, ".wav"); lc.push_back(w); }
        ResLoc b = res.Find(lc);
        if (b.found) {
            bgmBData_ = res.Load(b.name);
            SDL_RWops* rwb = SDL_RWFromMem(bgmBData_.data(), (int)bgmBData_.size());
            bgmB_ = Mix_LoadMUS_RW(rwb, 1);
        }
        if (bgmB_) {
            Mix_PlayMusic(bgmA_, 0);
            Log(LogLevel::Info, "audio: bgm %d playing A (B next)", id);
        } else {
            if (b.found) Log(LogLevel::Warn, "audio: bgm B decode failed: %s", Mix_GetError());
            Mix_PlayMusic(bgmA_, loop ? -1 : 0);
            Log(LogLevel::Info, "audio: bgm %d playing A only (loop=%d)", id, (int)loop);
        }
    } else {
        Mix_PlayMusic(bgmA_, loop ? -1 : 0);
        Log(LogLevel::Info, "audio: bgm %d playing single (loop=%d)", id, (int)loop);
    }
    return true;
}

void Audio::StopBgm(int fadeMs) {
    if (noAudio_) return;
    if (fadeMs > 0) {
        bgmStopping_ = true;
        musicFinished_.store(false, std::memory_order_release);
        if (!Mix_FadeOutMusic(fadeMs)) FreeBgm();
    } else {
        FreeBgm();
    }
}

float Audio::PlayVoice(int label, int id, int ch, bool loop, Res& res) {
    if (noAudio_) return 0;
    Chan& c = *VoiceChan(ch);
    if (c.chunk) { Mix_HaltChannel(8 + (ch & 3)); Mix_FreeChunk(c.chunk); c.chunk = nullptr; }
    std::string name = Res::VoiceName(label, id, ch & 0xFF);
    std::vector<uint8_t> data = res.Load(name);
    if (data.empty()) {
        Log(LogLevel::Info, "audio: voice %s missing", name.c_str());
        return 0;
    }
    SDL_RWops* rw = SDL_RWFromMem(data.data(), (int)data.size());
    c.chunk = Mix_LoadWAV_RW(rw, 1);
    if (!c.chunk) return 0;
    Mix_Volume(8 + (ch & 3), voiceVol_);
    Mix_PlayChannel(8 + (ch & 3), c.chunk, loop ? -1 : 0);
    c.startMs = SDL_GetTicks();
    c.lengthMs = (int)((uint64_t)c.chunk->alen * 1000 / 176400);   // 44.1k 16bit 立体声
    c.loop = loop;
    return c.lengthMs / 1000.0f;
}

void Audio::StopVoice(int fadeMs, int ch) {
    if (noAudio_) return;
    Chan& c = *VoiceChan(ch);
    if (fadeMs > 0) Mix_FadeOutChannel(8 + (ch & 3), fadeMs);
    else Mix_HaltChannel(8 + (ch & 3));
    if (c.chunk && fadeMs <= 0) { Mix_FreeChunk(c.chunk); c.chunk = nullptr; }
}

void Audio::SetVoiceVolume(int ch, int vol, int fadeMs) {
    if (noAudio_) return;
    (void)fadeMs;
    Mix_Volume(8 + (ch & 3), vol * voiceVol_ / 255);
}

float Audio::VoiceRemaining(int ch) const {
    if (noAudio_) return 0;
    const Chan& c = *const_cast<Audio*>(this)->VoiceChan(ch);
    if (!c.chunk || c.loop) return 0;
    int elapsed = (int)(SDL_GetTicks() - c.startMs);
    return elapsed < c.lengthMs ? (c.lengthMs - elapsed) / 1000.0f : 0;
}

bool Audio::PlaySe(int ch, int id, bool loop, int fadeInMs, int vol, Res& res) {
    if (noAudio_) return false;
    // 自动分配:找空闲通道
    if (ch < 0) {
        for (int i = 0; i < 8; i++) {
            if (!Mix_Playing(i)) { ch = i; break; }
        }
        if (ch < 0) ch = 0;
    }
    Chan& c = *SeChan(ch);
    if (c.chunk) { Mix_HaltChannel(ch & 7); Mix_FreeChunk(c.chunk); c.chunk = nullptr; }
    // wav 优先,回退 ogg
    std::vector<std::string> cands = {Res::SeName(id)};
    std::string ogg = Res::SeName(id);
    ogg = ogg.substr(0, ogg.size() - 4) + ".ogg";
    cands.push_back(ogg);
    ResLoc loc = res.Find(cands);
    if (!loc.found) {
        Log(LogLevel::Info, "audio: se %d missing", id);
        return false;
    }
    std::vector<uint8_t> data = res.Load(loc.name);
    SDL_RWops* rw = SDL_RWFromMem(data.data(), (int)data.size());
    c.chunk = Mix_LoadWAV_RW(rw, 1);
    if (!c.chunk) return false;
    Mix_Volume(ch & 7, vol * seVol_ / 255);
    if (fadeInMs > 0)
        Mix_FadeInChannelTimed(ch & 7, c.chunk, loop ? -1 : 0, fadeInMs, -1);
    else
        Mix_PlayChannelTimed(ch & 7, c.chunk, loop ? -1 : 0, -1);
    c.startMs = SDL_GetTicks();
    c.lengthMs = (int)((uint64_t)c.chunk->alen * 1000 / 176400);
    c.loop = loop;
    return true;
}

void Audio::StopSe(int ch, int fadeMs) {
    if (noAudio_) return;
    Chan& c = *SeChan(ch);
    if (fadeMs > 0) Mix_FadeOutChannel(ch & 7, fadeMs);
    else Mix_HaltChannel(ch & 7);
    if (c.chunk && fadeMs <= 0) { Mix_FreeChunk(c.chunk); c.chunk = nullptr; }
}

void Audio::SetSeVolume(int ch, int vol, int fadeMs) {
    if (noAudio_) return;
    (void)fadeMs;
    Mix_Volume(ch & 7, vol * seVol_ / 255);
}

float Audio::SeRemaining(int ch) const {
    if (noAudio_) return 0;
    const Chan& c = *const_cast<Audio*>(this)->SeChan(ch);
    if (!c.chunk || c.loop) return 0;
    int elapsed = (int)(SDL_GetTicks() - c.startMs);
    return elapsed < c.lengthMs ? (c.lengthMs - elapsed) / 1000.0f : 0;
}

void Audio::StopAll() {
    if (noAudio_) return;
    FreeBgm();
    for (int i = 0; i < 8; i++) {
        if (se_[i].chunk) { Mix_HaltChannel(i); Mix_FreeChunk(se_[i].chunk); se_[i].chunk = nullptr; }
    }
    for (int i = 0; i < 4; i++) {
        if (voice_[i].chunk) { Mix_HaltChannel(8 + i); Mix_FreeChunk(voice_[i].chunk); voice_[i].chunk = nullptr; }
    }
}

} // namespace wa2
