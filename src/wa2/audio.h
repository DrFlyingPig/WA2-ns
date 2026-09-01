// audio.h — SDL2_mixer 音频(BGM A/B 循环 / SE / 语音)
#pragma once

#include "wa2.h"
#include "res.h"
#include <SDL2/SDL_mixer.h>   // 提供 Mix_Music/Mix_Chunk(SDL_mixer 各版本内部标签不同,须用头文件)
#include <atomic>

namespace wa2 {

class Audio {
public:
    bool Init();
    void Shutdown();
    void Update();   // 主线程每帧调用：BGM A→B 链接处理
    void NotifyMusicFinished(); // 仅供 SDL_mixer 音频回调设置原子标志

    void SetVolumes(int bgm, int se, int voice);   // 0-255

    bool PlayBgm(int id, bool loop, int vol, Res& res);   // vol 0-255
    void StopBgm(int fadeMs);
    // 语音:{label:04d}_{id:04d}_{chr:02d}.ogg;返回时长秒(未知为 0)
    float PlayVoice(int label, int id, int ch, bool loop, Res& res);
    void StopVoice(int fadeMs, int ch);
    void SetVoiceVolume(int ch, int vol, int fadeMs);
    float VoiceRemaining(int ch) const;

    bool PlaySe(int ch, int id, bool loop, int fadeInMs, int vol, Res& res);  // ch<0 自动分配
    void StopSe(int ch, int fadeMs);
    void SetSeVolume(int ch, int vol, int fadeMs);
    float SeRemaining(int ch) const;

    void StopAll();

private:
    bool noAudio_ = false;   // 临时: WA2_NOAUDIO 关闭 SDL_mixer(避开 ASan+SDL_mixer 音频线程)
    Mix_Music* bgmA_ = nullptr;   // 前奏段
    Mix_Music* bgmB_ = nullptr;   // 循环段
    // Mix_Music 可能在播放期间继续读取 RWops，底层字节必须活到 Mix_FreeMusic。
    std::vector<uint8_t> bgmAData_, bgmBData_;
    bool bgmLoop_ = true;
    bool bgmInLoopPart_ = false;
    bool bgmStopping_ = false;
    std::atomic<bool> musicFinished_{false};
    int bgmVol_ = 200;
    struct Chan {
        Mix_Chunk* chunk = nullptr;
        uint32_t startMs = 0;
        int lengthMs = 0;
        bool loop = false;
    };
    Chan se_[8];
    Chan voice_[4];
    int seVol_ = 220, voiceVol_ = 255;

    void FreeBgm();
    Chan* SeChan(int ch) { return &se_[ch & 7]; }
    Chan* VoiceChan(int ch) { return &voice_[ch & 3]; }
};

} // namespace wa2
