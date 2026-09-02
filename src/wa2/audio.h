// audio.h — SDL2_mixer 音频(BGM A/B 循环 / SE / 语音)
#pragma once

#include "wa2.h"
#include "res.h"
#include "audio_ring.h"
#include <SDL2/SDL_mixer.h>   // 提供 Mix_Music/Mix_Chunk(SDL_mixer 各版本内部标签不同,须用头文件)
#include <atomic>

namespace wa2 {

// 电影音轨由 VideoPlayer 在引擎线程流式解码,输出 PCM 写入自己的环形缓冲;
// Audio 的后混音回调(MIX_CHANNEL_POST)实时读取并混入最终输出。
// 引擎线程是唯一生产者,SDL 音频线程是唯一消费者。
class MovieAudioSource {
public:
    virtual ~MovieAudioSource() = default;
    // 实时音频线程调用;返回实际读到的字节数(不足部分保持静音)。
    virtual size_t ReadMoviePcm(void* dst, size_t bytes) = 0;
    // 0-255,调用方(引擎)已按总音量归一。
    virtual int MovieVolume255() const = 0;
};

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

    // 电影音轨:注册/注销后混音 PCM 源。src 在下一次 SetMovieAudioSource
    // (或 Audio 销毁)前必须保持存活;内部持有 SDL 音频锁,可在引擎线程调用。
    void SetMovieAudioSource(MovieAudioSource* src);
    void StopAll();

private:
    bool noAudio_ = false;   // 临时: WA2_NOAUDIO 关闭 SDL_mixer(避开 ASan+SDL_mixer 音频线程)
    bool mixerInitialized_ = false;
    bool deviceOpen_ = false;
    // 后混音效果链在所有平台注册:Switch 混流式 SE,PC/Switch 混电影 PCM。
    bool postEffectRegistered_ = false;
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
    // 由音频回调线程只写一次,主线程输出日志。不要在实时回调中做文件 I/O。
    std::atomic<size_t> callbackStackBytes_{0};
    bool callbackStackReported_ = false;
    bool TryPlayStreamSe(int ch, int id, bool loop, int fadeInMs, int vol,
                         const std::string& name, std::vector<uint8_t>&& data);
    void PumpStreamSeDecoders();
    void MixStreamSe(void* stream, int len);
    void RetireFinishedStreams();
    void StopStreamSe(int ch, int fadeMs);
#endif
    // 后混音回调:Switch 里混流式 SE,所有平台混电影 PCM。
    static void SDLCALL StreamSePostEffect(int channel, void* stream, int len, void* udata);

    // 电影音轨后混音:src 由引擎线程换入换出,音频线程只读。
    std::atomic<MovieAudioSource*> movieSrc_{nullptr};
    // 后混音回调专用暂存(注册源时按设备缓冲上限分配)。
    std::vector<uint8_t> movieMixScratch_;
    // 空样本循环块:保证电影播放期间混音回调始终有活动通道,
    // 后混音效果链稳定执行(部分 SDL_mixer 版本在全静音时会跳过)。
    std::vector<uint8_t> movieSilence_;
    Mix_Chunk* movieSilenceChunk_ = nullptr;
    void MixMovieAudio(void* stream, int len);

    void FreeBgm();
    Chan* SeChan(int ch) { return ch >= 0 && ch < kSeChannels ? &se_[ch] : nullptr; }
    Chan* VoiceChan(int ch) { return ch >= 0 && ch < kVoiceChannels ? &voice_[ch] : nullptr; }
};

} // namespace wa2
