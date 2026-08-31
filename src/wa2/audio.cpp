// audio.cpp — SDL2_mixer 实现
#include "audio.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <cstdlib>

namespace wa2 {

static Audio* g_audio = nullptr;   // Mix_HookMusicFinished 需要静态回调

static void OnMusicFinished() {
    if (g_audio) g_audio->Update();
}

bool Audio::Init() {
    // TEMP-DIAG-AUDIO: 本次对照诊断包强制音频 no-op(不经 SDL_mixer/不开音频线程)
    // 用于分离"音频线程"是否是多轮随机系统崩溃的来源。还原时删掉此行。
    noAudio_ = true;
    if (noAudio_) return true;
    Mix_Init(MIX_INIT_OGG);   // 初始化需要的解码器(OGG);demo 用 WAV 亦无害
    if (Mix_OpenAudio(48000, MIX_DEFAULT_FORMAT, 2, 4096) != 0) {
        Log(LogLevel::Error, "audio: open failed: %s", Mix_GetError());
        return false;
    }
    Mix_AllocateChannels(32);
    // 通道布局:0-7 SE,8-11 语音,12 BGM 占位
    g_audio = this;
    Mix_HookMusicFinished(OnMusicFinished);
    return true;
}

void Audio::Shutdown() {
    if (noAudio_) return;
    StopAll();
    Mix_HookMusicFinished(nullptr);
    g_audio = nullptr;
    Mix_CloseAudio();
}

void Audio::SetVolumes(int bgm, int se, int voice) {
    if (noAudio_) return;
    bgmVol_ = bgm; seVol_ = se; voiceVol_ = voice;
    Mix_VolumeMusic(bgmVol_);
    for (int i = 0; i < 8; i++) Mix_Volume(i, seVol_);
    for (int i = 0; i < 4; i++) Mix_Volume(8 + i, voiceVol_);
}

void Audio::Update() {
    if (noAudio_) return;
    // BGM A 段播完 → 无缝接 B 段循环
    if (bgmB_) {
        Mix_Music* b = bgmB_;
        bgmB_ = nullptr;      // 防止重入
        if (Mix_PlayMusic(b, -1) != 0) {
            Log(LogLevel::Warn, "audio: bgm loop failed: %s", Mix_GetError());
        }
        bgmB_ = b;
    }
}

void Audio::FreeBgm() {
    if (noAudio_) return;
    if (bgmA_) { Mix_FreeMusic(bgmA_); bgmA_ = nullptr; }
    if (bgmB_) { Mix_FreeMusic(bgmB_); bgmB_ = nullptr; }
    Mix_HaltMusic();
}

bool Audio::PlayBgm(int id, bool loop, int vol, Res& res) {
    if (noAudio_) return false;
    FreeBgm();
    Mix_VolumeMusic(vol * bgmVol_ / 255);
    // A(前奏/主段):.ogg 优先,demo 用 .wav 回退
    std::string single = Res::BgmName(id, false);          // bgm_xxx.ogg
    std::string singleWav = single;
    if (singleWav.size() > 4) singleWav.replace(singleWav.size() - 4, 4, ".wav");
    std::vector<std::string> cands = {single, singleWav};
    ResLoc a = res.Find(cands);
    if (!a.found) {
        Log(LogLevel::Warn, "audio: bgm %d missing", id);
        return false;
    }
    Log(LogLevel::Info, "audio: bgm %d found %s", id, a.name.c_str());
    std::vector<uint8_t> data = res.Load(a.name);
    SDL_RWops* rw = SDL_RWFromMem(data.data(), (int)data.size());
    bgmA_ = Mix_LoadMUS_RW(rw, 1);   // frees rw
    if (!bgmA_) {
        Log(LogLevel::Warn, "audio: bgm decode failed: %s", Mix_GetError());
        return false;
    }
    Log(LogLevel::Info, "audio: bgm %d music loaded", id);
    // B(循环段):存在则 A 播一次后接 B 循环;否则 A 自身作为整曲循环
    std::string loopPart = Res::BgmName(id, true);         // bgm_xxx_b.ogg
    std::vector<std::string> lc = {loopPart};
    if (loopPart.size() > 4) { std::string w = loopPart; w.replace(w.size() - 4, 4, ".wav"); lc.push_back(w); }
    ResLoc b = res.Find(lc);
    if (b.found) {
        Mix_PlayMusic(bgmA_, 0);
        std::vector<uint8_t> db = res.Load(b.name);
        SDL_RWops* rwb = SDL_RWFromMem(db.data(), (int)db.size());
        bgmB_ = Mix_LoadMUS_RW(rwb, 1);
        if (!bgmB_) Log(LogLevel::Warn, "audio: bgm B decode failed: %s", Mix_GetError());
        Log(LogLevel::Info, "audio: bgm %d playing A (B next)", id);
    } else {
        Mix_PlayMusic(bgmA_, loop ? -1 : 0);
        Log(LogLevel::Info, "audio: bgm %d playing single (loop=%d)", id, (int)loop);
    }
    return true;
}

void Audio::StopBgm(int fadeMs) {
    if (noAudio_) return;
    if (fadeMs > 0) Mix_FadeOutMusic(fadeMs);
    else Mix_HaltMusic();
    // 释放延后到下一次 PlayBgm(避免淡出中断)
    if (fadeMs <= 0) FreeBgm();
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
