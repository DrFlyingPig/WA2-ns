// audio.h — SDL2_mixer 音频(BGM A/B 循环 / SE / 语音)
#pragma once

#include "wa2.h"
#include "res.h"
#include "audio_ring.h"
#include <SDL2/SDL_mixer.h>   // 提供 Mix_Music/Mix_Chunk(SDL_mixer 各版本内部标签不同,须用头文件)
#include <atomic>

namespace wa2 {

class Audio {
public:
    static constexpr int kSeChannels = 10;
    static constexpr int kVoiceChannels = 10;
    static constexpr int kVoiceBase = kSeChannels;

    struct LoopSeState {
        int channel = 0;
        int id = -1;
        int volume = 255;
    };
#ifdef __SWITCH__
    struct StreamSe;   // 仅 Switch 使用的长环境音流式解码状态
#endif

    ~Audio();
    bool Init();
    void Shutdown();
    // Engine 工作线程异常结束时由 NRO 入口线程调用。
    // 不访问 Audio 实例，只取消全局回调并回收 SDL_mixer 线程。
    static void EmergencyShutdown();
    void Update();   // 主线程每帧调用：BGM A→B 链接处理
    void NotifyMusicFinished(); // 仅供 SDL_mixer 音频回调设置原子标志

    void SetVolumes(int bgm, int se, int voice);   // 0-255

    bool PlayBgm(int id, bool loop, int vol, Res& res);   // vol 0-255
    void StopBgm(int fadeMs);
    // 语音:{label:04d}_{id:04d}_{chr:02d}.ogg;返回时长秒(未知为 0)
    float PlayVoice(int label, int id, int chr, int channel, int volume,
                    bool loop, Res& res);
    // 特别模式声优访谈使用 9500_000x_xx.ogg，不符合普通剧情三段编号。
    float PlayVoiceFile(const std::string& name, int channel, int volume,
                        bool loop, Res& res);
    void StopVoice(int fadeMs, int ch);
    void SetVoiceVolume(int ch, int vol, int fadeMs);
    float VoiceRemaining(int ch) const;

    bool PlaySe(int ch, int id, bool loop, int fadeInMs, int vol, Res& res);  // ch<0 自动分配
    void StopSe(int ch, int fadeMs);
    void SetSeVolume(int ch, int vol, int fadeMs);
    float SeRemaining(int ch) const;

    int CurrentBgmId() const { return bgmId_; }
    bool CurrentBgmLoop() const { return bgmLoop_; }
    int CurrentBgmVolume() const { return bgmScriptVol_; }
    std::vector<LoopSeState> LoopingSe() const;

    void StopAll();

private:
    bool noAudio_ = false;   // 临时: WA2_NOAUDIO 关闭 SDL_mixer(避开 ASan+SDL_mixer 音频线程)
    bool mixerInitialized_ = false;
    bool deviceOpen_ = false;
#ifdef __SWITCH__
    bool postEffectRegistered_ = false;
#endif
    Mix_Music* bgmA_ = nullptr;   // 前奏段
    Mix_Music* bgmB_ = nullptr;   // 循环段
    // Mix_Music 可能在播放期间继续读取 RWops，底层字节必须活到 Mix_FreeMusic。
    std::vector<uint8_t> bgmAData_, bgmBData_;
    bool bgmLoop_ = true;
    int bgmId_ = -1;
    int bgmScriptVol_ = 255;
    bool bgmInLoopPart_ = false;
    bool bgmStopping_ = false;
    std::atomic<bool> musicFinished_{false};
    int bgmVol_ = 200;
    struct Chan {
        Mix_Chunk* chunk = nullptr;
        uint32_t startMs = 0;
        int lengthMs = 0;
        bool loop = false;
        int id = -1;
        int volume = 255;
    };
    Chan se_[kSeChannels];
    Chan voice_[kVoiceChannels];
    // 流式 SE 的后混音回调会从 SDL 音频线程读取主音量。
    std::atomic<int> seVol_{220};
    int voiceVol_ = 255;

#ifdef __SWITCH__
    StreamSe* streamSe_[kSeChannels] = {};
    // 由音频回调线程只写一次，主线程输出日志。不要在实时回调中做文件 I/O。
    std::atomic<size_t> callbackStackBytes_{0};
    bool callbackStackReported_ = false;
    static void SDLCALL StreamSePostEffect(int channel, void* stream, int len, void* udata);
    bool TryPlayStreamSe(int ch, int id, bool loop, int fadeInMs, int vol,
                         const std::string& name, std::vector<uint8_t>&& data);
    void PumpStreamSeDecoders();
    void MixStreamSe(void* stream, int len);
    void RetireFinishedStreams();
    void StopStreamSe(int ch, int fadeMs);
#endif

    void FreeBgm();
    Chan* SeChan(int ch) { return ch >= 0 && ch < kSeChannels ? &se_[ch] : nullptr; }
    Chan* VoiceChan(int ch) { return ch >= 0 && ch < kVoiceChannels ? &voice_[ch] : nullptr; }
};

} // namespace wa2
