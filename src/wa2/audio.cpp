// audio.cpp — SDL2_mixer 实现
#include "audio.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef __SWITCH__
#include <switch.h>
#include <tremor/ivorbisfile.h>
#endif

namespace wa2 {

static std::atomic<Audio*> g_audio{nullptr}; // Mix_HookMusicFinished 需要静态入口

static int MixerVolume(int scriptVolume, int masterVolume) {
    const int script = std::clamp(scriptVolume, 0, 255);
    const int master = std::clamp(masterVolume, 0, 255);
    return (script * master * MIX_MAX_VOLUME + 255 * 255 / 2) / (255 * 255);
}

static void OnMusicFinished() {
    // 此回调可从 SDL 音频线程调用，也会在 Mix_HaltMusic 内部
    // 持有音频锁时同步调用。绝对不能在这里再 Play/Halt/Free。
    if (Audio* audio = g_audio.load(std::memory_order_acquire))
        audio->NotifyMusicFinished();
}

#ifdef __SWITCH__
struct Audio::StreamSe {
    enum class Kind { Ogg, Wav } kind = Kind::Ogg;
    std::vector<uint8_t> data;
    size_t readPos = 0;
    OggVorbis_File vorbis{};
    bool vorbisOpen = false;
    size_t pcmBegin = 0;
    size_t pcmSize = 0;
    size_t pcmPos = 0;
    uint64_t totalFrames = 0;
    uint64_t playedFrames = 0;
    uint64_t fadeInFrames = 0;
    uint64_t fadeOutFrames = 0;
    uint64_t fadeOutRemaining = 0;
    bool loop = false;
    bool decodeStopped = false; // 仅主线程访问 Tremor/WAV 解码状态
    std::atomic<bool> decoderEof{false};
    std::atomic<bool> finished{false};
    int id = -1;
    int volume = 255;
    // Tremor 只在主线程写 decodeScratch/pcmRing；SDL 音频线程只从
    // pcmRing 读到 mixScratch。两边不再同时进入解码器。
    SpscByteRing<512 * 1024> pcmRing;
    std::array<uint8_t, 64 * 1024> decodeScratch{};
    std::array<uint8_t, 64 * 1024> mixScratch{};

    ~StreamSe() {
        if (vorbisOpen) ov_clear(&vorbis);
    }
};

static size_t StreamRead(void* ptr, size_t size, size_t nmemb, void* datasource) {
    auto* s = static_cast<Audio::StreamSe*>(datasource);
    if (!s || size == 0 || nmemb == 0) return 0;
    const size_t remain = s->readPos <= s->data.size() ? s->data.size() - s->readPos : 0;
    const size_t items = std::min(nmemb, remain / size);
    const size_t bytes = items * size;
    if (bytes) {
        std::memcpy(ptr, s->data.data() + s->readPos, bytes);
        s->readPos += bytes;
    }
    return items;
}

static int StreamSeek(void* datasource, ogg_int64_t offset, int whence) {
    auto* s = static_cast<Audio::StreamSe*>(datasource);
    if (!s) return -1;
    ogg_int64_t base = 0;
    if (whence == SEEK_CUR) base = (ogg_int64_t)s->readPos;
    else if (whence == SEEK_END) base = (ogg_int64_t)s->data.size();
    else if (whence != SEEK_SET) return -1;
    const ogg_int64_t next = base + offset;
    if (next < 0 || (uint64_t)next > s->data.size()) return -1;
    s->readPos = (size_t)next;
    return 0;
}

static long StreamTell(void* datasource) {
    auto* s = static_cast<Audio::StreamSe*>(datasource);
    return s && s->readPos <= (size_t)LONG_MAX ? (long)s->readPos : -1L;
}

static bool InitWavStream(Audio::StreamSe* s) {
    if (!s || s->data.size() < 12 || std::memcmp(s->data.data(), "RIFF", 4) != 0 ||
        std::memcmp(s->data.data() + 8, "WAVE", 4) != 0)
        return false;
    bool formatOk = false;
    size_t p = 12;
    while (p + 8 <= s->data.size()) {
        const uint8_t* h = s->data.data() + p;
        const uint32_t n = ReadU32(h + 4);
        const size_t body = p + 8;
        if (body > s->data.size() || n > s->data.size() - body) return false;
        if (std::memcmp(h, "fmt ", 4) == 0 && n >= 16) {
            const uint8_t* f = s->data.data() + body;
            const uint16_t format = ReadU16(f);
            const uint16_t channels = ReadU16(f + 2);
            const uint32_t rate = ReadU32(f + 4);
            const uint16_t bits = ReadU16(f + 14);
            formatOk = format == 1 && channels == 2 && rate == 48000 && bits == 16;
        } else if (std::memcmp(h, "data", 4) == 0) {
            s->pcmBegin = body;
            s->pcmSize = n - (n % 4u);
        }
        p = body + n + (n & 1u);
    }
    if (!formatOk || !s->pcmSize) return false;
    s->kind = Audio::StreamSe::Kind::Wav;
    s->totalFrames = s->pcmSize / 4u;
    return true;
}

static bool InitOggStream(Audio::StreamSe* s) {
    if (!s || s->data.size() < 4 || std::memcmp(s->data.data(), "OggS", 4) != 0)
        return false;
    ov_callbacks cb{};
    cb.read_func = StreamRead;
    cb.seek_func = StreamSeek;
    cb.close_func = nullptr;
    cb.tell_func = StreamTell;
    s->readPos = 0;
    if (ov_open_callbacks(s, &s->vorbis, nullptr, 0, cb) != 0) return false;
    s->vorbisOpen = true;
    vorbis_info* info = ov_info(&s->vorbis, -1);
    if (!info || info->rate != 48000 || info->channels != 2) return false;
    const ogg_int64_t frames = ov_pcm_total(&s->vorbis, -1);
    if (frames <= 0) return false;
    s->kind = Audio::StreamSe::Kind::Ogg;
    s->totalFrames = (uint64_t)frames;
    return true;
}

static size_t ReadStreamPcm(Audio::StreamSe* s, uint8_t* dst, size_t wanted) {
    size_t done = 0;
    int retry = 0;
    while (done < wanted && !s->decodeStopped) {
        if (s->kind == Audio::StreamSe::Kind::Wav) {
            const size_t remain = s->pcmPos <= s->pcmSize ? s->pcmSize - s->pcmPos : 0;
            if (!remain) {
                if (s->loop) { s->pcmPos = 0; continue; }
                s->decodeStopped = true;
                break;
            }
            const size_t n = std::min(wanted - done, remain);
            std::memcpy(dst + done, s->data.data() + s->pcmBegin + s->pcmPos, n);
            s->pcmPos += n;
            done += n;
            continue;
        }

        int bitstream = 0;
        const long n = ov_read(&s->vorbis, reinterpret_cast<char*>(dst + done),
                               (int)std::min<size_t>(wanted - done, INT_MAX), &bitstream);
        if (n > 0) {
            done += (size_t)n;
            retry = 0;
        } else if (n == 0) {
            if (s->loop && ov_pcm_seek(&s->vorbis, 0) == 0) continue;
            s->decodeStopped = true;
        } else if (++retry >= 4) {
            s->decodeStopped = true;
        }
    }
    return done - (done % 4u);
}

static void PumpOneStreamDecoder(Audio::StreamSe* s, size_t targetBytes) {
    if (!s || s->finished.load(std::memory_order_acquire)) return;
    targetBytes = std::min(targetBytes, s->pcmRing.Size());
    while (!s->decodeStopped && s->pcmRing.Available() < targetBytes) {
        const size_t need = targetBytes - s->pcmRing.Available();
        size_t request = std::min({s->decodeScratch.size(), s->pcmRing.Free(), need});
        request -= request % 4u;
        if (!request) break;
        const size_t got = ReadStreamPcm(s, s->decodeScratch.data(), request);
        if (!got) break;
        const size_t written = s->pcmRing.Write(s->decodeScratch.data(), got);
        if (written != got) {
            s->decodeStopped = true;
            break;
        }
    }
    if (s->decodeStopped)
        s->decoderEof.store(true, std::memory_order_release);
}
#endif

Audio::~Audio() {
    Shutdown();
}

bool Audio::Init() {
    // 允许测试或未来的重新初始化路径安全复用同一实例。
    Shutdown();
    noAudio_ = std::getenv("WA2_NOAUDIO") != nullptr;
    if (noAudio_) return true;
#ifdef __SWITCH__
    // devkitPro SDL2 默认的 switch(audren)后端在长时间播放时可能陷入
    // 缓冲状态忙等。Switch 构建会链接项目私有的 switchout(audout)后端；
    // 强制指定名称，防止误用系统库时静默退回有问题的旧实现。
    SDL_setenv("SDL_AUDIODRIVER", "switchout", 1);
#endif
    const int mixerFlags = Mix_Init(MIX_INIT_OGG);
    mixerInitialized_ = true;
    if ((mixerFlags & MIX_INIT_OGG) == 0) {
        Log(LogLevel::Error, "audio: OGG decoder init failed: %s", Mix_GetError());
        Shutdown();
        return false;
    }
    if (Mix_OpenAudio(48000, MIX_DEFAULT_FORMAT, 2, 4096) != 0) {
        Log(LogLevel::Error, "audio: open failed: %s", Mix_GetError());
        Shutdown();
        return false;
    }
    deviceOpen_ = true;
    int freq = 0;
    int channels = 0;
    Uint16 format = 0;
    Mix_QuerySpec(&freq, &format, &channels);
    Log(LogLevel::Info, "audio: backend=%s freq=%d format=0x%x channels=%d buffer=4096",
        SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown",
        freq, (unsigned)format, channels);
    Mix_AllocateChannels(32);
    // 与 wa2-godot/原引擎一致保留 10 路 SE + 10 路语音；不要用位掩码
    // 把 8/9 号通道折回 0/1，否则后续脚本会停止并释放错误的声音。
    musicFinished_.store(false, std::memory_order_relaxed);
    g_audio.store(this, std::memory_order_release);
    Mix_HookMusicFinished(OnMusicFinished);
#ifdef __SWITCH__
    callbackStackBytes_.store(0, std::memory_order_relaxed);
    callbackStackReported_ = false;
#endif
    // 后混音效果链同时服务 Switch 的流式 SE 和全平台的电影 PCM。
    if (!Mix_RegisterEffect(MIX_CHANNEL_POST, StreamSePostEffect, nullptr, this)) {
        Log(LogLevel::Error, "audio: cannot register post mix effect: %s", Mix_GetError());
        Shutdown();
        return false;
    }
    postEffectRegistered_ = true;
    return true;
}

void Audio::Shutdown() {
    if (noAudio_) {
        noAudio_ = false;
        return;
    }
    if (!deviceOpen_ && !mixerInitialized_) return;
    Log(LogLevel::Info, "audio: shutdown begin open=%d mixer=%d",
        deviceOpen_ ? 1 : 0, mixerInitialized_ ? 1 : 0);
    // 先取消回调，再停止音乐；否则 Mix_HaltMusic 会再投递完成事件。
    Mix_HookMusicFinished(nullptr);
    g_audio.store(nullptr, std::memory_order_release);
    if (postEffectRegistered_) {
        Mix_UnregisterEffect(MIX_CHANNEL_POST, StreamSePostEffect);
        postEffectRegistered_ = false;
    }
    movieSrc_.store(nullptr, std::memory_order_release);
    if (movieSilenceChunk_) {
        Mix_HaltChannel(31);
        Mix_FreeChunk(movieSilenceChunk_);
        movieSilenceChunk_ = nullptr;
    }
    if (deviceOpen_) {
        StopAll();
        Log(LogLevel::Info, "audio: closing SDL_mixer device");
        Mix_CloseAudio();
        deviceOpen_ = false;
        Log(LogLevel::Info, "audio: SDL_mixer device closed");
    }
    if (mixerInitialized_) {
        Mix_Quit();
        mixerInitialized_ = false;
    }
    Log(LogLevel::Info, "audio: shutdown complete");
}

void Audio::EmergencyShutdown() {
    // 此路径只在 EngineThreadMain 没有运行到正常结束标记时使用。
    // 引擎对象仍位于尚未 threadClose 的工作线程栈上，但这里不
    // 解引用它；先取消所有指向 NRO/Audio 的回调，再回收设备线程。
    Log(LogLevel::Error, "audio: emergency shutdown begin");
    g_audio.store(nullptr, std::memory_order_release);
    Mix_HookMusicFinished(nullptr);
#ifdef __SWITCH__
    Mix_UnregisterAllEffects(MIX_CHANNEL_POST);
#endif
    int freq = 0;
    int channels = 0;
    Uint16 format = 0;
    if (Mix_QuerySpec(&freq, &format, &channels) > 0) {
        Mix_HaltMusic();
        Mix_HaltChannel(-1);
        Mix_CloseAudio();
    }
    Mix_Quit();
    Log(LogLevel::Error, "audio: emergency shutdown complete");
}

void Audio::SetVolumes(int bgm, int se, int voice) {
    if (noAudio_) return;
    bgmVol_ = std::clamp(bgm, 0, 255);
    const int seMaster = std::clamp(se, 0, 255);
    seVol_.store(seMaster, std::memory_order_release);
    voiceVol_ = std::clamp(voice, 0, 255);
    Mix_VolumeMusic(MixerVolume(bgmScriptVol_, bgmVol_));
    for (int i = 0; i < kSeChannels; i++)
        Mix_Volume(i, MixerVolume(se_[i].volume, seMaster));
    for (int i = 0; i < kVoiceChannels; i++)
        Mix_Volume(kVoiceBase + i, MixerVolume(voice_[i].volume, voiceVol_));
}

void Audio::NotifyMusicFinished() {
    musicFinished_.store(true, std::memory_order_release);
}

void Audio::Update() {
    if (noAudio_) return;
#ifdef __SWITCH__
    PumpStreamSeDecoders();
    const size_t callbackStack = callbackStackBytes_.load(std::memory_order_acquire);
    if (callbackStack && !callbackStackReported_) {
        callbackStackReported_ = true;
        Log(LogLevel::Info, "audio: callback thread stack=%.1f MiB",
            (double)callbackStack / (1024.0 * 1024.0));
    }
    RetireFinishedStreams();
#endif
    // Mix_Chunk 播放结束后 SDL_mixer 不替调用方释放数据。旧实现把每个
    // 声道最后一段 PCM 永久留到下次复用，长时间运行会持续占住大块堆。
    for (int i = 0; i < kSeChannels; ++i) {
        if (se_[i].chunk && !Mix_Playing(i)) {
            Mix_FreeChunk(se_[i].chunk);
            se_[i].chunk = nullptr;
            se_[i].id = -1;
            se_[i].loop = false;
        }
    }
    for (int i = 0; i < kVoiceChannels; ++i) {
        if (voice_[i].chunk && !Mix_Playing(kVoiceBase + i)) {
            Mix_FreeChunk(voice_[i].chunk);
            voice_[i].chunk = nullptr;
            voice_[i].id = -1;
            voice_[i].loop = false;
        }
    }
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

#ifdef __SWITCH__
void SDLCALL Audio::StreamSePostEffect(int channel, void* stream, int len, void* udata) {
    (void)channel;
    if (udata && stream && len > 0) {
        Audio* audio = static_cast<Audio*>(udata);
#ifdef __SWITCH__
        if (audio->callbackStackBytes_.load(std::memory_order_relaxed) == 0) {
            if (Thread* self = threadGetSelf()) {
                audio->callbackStackBytes_.store(self->stack_sz, std::memory_order_release);
            }
        }
        audio->MixStreamSe(stream, len);
#endif
        audio->MixMovieAudio(stream, len);
    }
}

void Audio::SetMovieAudioSource(MovieAudioSource* src) {
    if (noAudio_) return;
    SDL_LockAudio();
    movieSrc_.store(src, std::memory_order_release);
    if (src) {
        // 实时回调的块最大不超过 Mix_OpenAudio 的缓冲(4096 字节),
        // 这里一次分配到该上限之上,回调内就永远不再进堆。
        if (movieMixScratch_.size() < 8192) movieMixScratch_.resize(8192);
        // MIX_CHANNEL_POST 效果链只在"有通道在响"时被 SDL_mixer 调用。
        // 电影期间没有其它通道活动,用一段循环静音块占住 31 号通道,
        // 保证后混音回调持续执行,电影 PCM 才能进入输出。
        if (!movieSilenceChunk_) {
            // Mix_OpenAudio 用 4096 帧缓冲;1 秒静音足够任何回调块大小。
            movieSilence_.assign(48000 * 2 * sizeof(int16_t), 0);
            movieSilenceChunk_ = Mix_QuickLoad_RAW(movieSilence_.data(),
                                                   (Uint32)movieSilence_.size());
            if (movieSilenceChunk_) {
                Mix_VolumeChunk(movieSilenceChunk_, 0);
                Mix_PlayChannel(31, movieSilenceChunk_, -1);
            } else {
                Log(LogLevel::Warn, "audio: movie silence chunk failed: %s",
                    Mix_GetError());
            }
        }
    } else if (movieSilenceChunk_) {
        Mix_HaltChannel(31);
        Mix_FreeChunk(movieSilenceChunk_);
        movieSilenceChunk_ = nullptr;
        movieSilence_.clear();
        movieSilence_.shrink_to_fit();
    }
    SDL_UnlockAudio();
}

void Audio::MixMovieAudio(void* stream, int len) {
    if (noAudio_ || len <= 0) return;
    MovieAudioSource* src = movieSrc_.load(std::memory_order_acquire);
    if (!src) return;
    // 实时线程不做动态分配;scratch 在注册源时按设备缓冲上限预分配。
    if (movieMixScratch_.size() < (size_t)len) return;
    const size_t got = src->ReadMoviePcm(movieMixScratch_.data(), (size_t)len);
    if (got == 0) return;
    const int gain = src->MovieVolume255() * MIX_MAX_VOLUME / 255;
    if (gain <= 0) return;
    auto* dst = static_cast<int16_t*>(stream);
    const int16_t* pcm = reinterpret_cast<const int16_t*>(movieMixScratch_.data());
    const size_t samples = std::min(got, (size_t)len) / sizeof(int16_t);
    for (size_t i = 0; i < samples; ++i) {
        const int mixed = (int)dst[i] + (int)pcm[i] * gain / MIX_MAX_VOLUME;
        dst[i] = (int16_t)std::clamp(mixed, -32768, 32767);
    }
}

bool Audio::TryPlayStreamSe(int ch, int id, bool loop, int fadeInMs, int vol,
                            const std::string& name, std::vector<uint8_t>&& data) {
    if (ch < 0 || ch >= kSeChannels || data.empty()) return false;
    StreamSe* fresh = new (std::nothrow) StreamSe();
    if (!fresh) return false;
    fresh->data = std::move(data);
    fresh->id = id;
    fresh->volume = vol;
    fresh->loop = loop;
    fresh->fadeInFrames = (uint64_t)std::max(0, fadeInMs) * 48u;

    const bool ok = InitWavStream(fresh) || InitOggStream(fresh);
    if (!ok) {
        if (fresh->vorbisOpen) {
            ov_clear(&fresh->vorbis);
            fresh->vorbisOpen = false;
        }
        // 调用方还要走 SDL_mixer 的兼容解码路径，失败时必须把所有权还回去。
        data = std::move(fresh->data);
        delete fresh;
        return false;
    }

    // 在交给实时音频线程前预解码约 1.3 秒 PCM。解码失败则归还原始
    // 压缩数据，让调用方继续走 SDL_mixer 的兼容路径。
    PumpOneStreamDecoder(fresh, 256 * 1024);
    if (fresh->pcmRing.Available() == 0) {
        if (fresh->vorbisOpen) {
            ov_clear(&fresh->vorbis);
            fresh->vorbisOpen = false;
        }
        data = std::move(fresh->data);
        delete fresh;
        return false;
    }

    StreamSe* old = nullptr;
    SDL_LockAudio();
    old = streamSe_[ch];
    streamSe_[ch] = fresh;
    se_[ch].id = id;
    se_[ch].volume = vol;
    se_[ch].loop = loop;
    se_[ch].startMs = SDL_GetTicks();
    se_[ch].lengthMs = (int)std::min<uint64_t>(fresh->totalFrames * 1000u / 48000u,
                                               (uint64_t)INT_MAX);
    SDL_UnlockAudio();
    delete old;

    Log(LogLevel::Info,
        "audio: se %d streaming ch=%d decoder=engine ring=512 KiB source=%s "
        "compressed=%.1f MiB duration=%.1fs",
        id, ch, name.c_str(), (double)fresh->data.size() / (1024.0 * 1024.0),
        (double)fresh->totalFrames / 48000.0);
    return true;
}

void Audio::PumpStreamSeDecoders() {
    // 约 2 秒缓冲。每帧只补足缺口，不在高优先级 SDL 音频线程中调用
    // Tremor；短暂的主线程卡顿也不会立即造成音频欠载。
    for (int ch = 0; ch < kSeChannels; ++ch) {
        StreamSe* s = streamSe_[ch];
        if (s) PumpOneStreamDecoder(s, 384 * 1024);
    }
}

void Audio::MixStreamSe(void* stream, int len) {
    auto* dstBytes = static_cast<uint8_t*>(stream);
    const int seMaster = seVol_.load(std::memory_order_acquire);
    for (int ch = 0; ch < kSeChannels; ++ch) {
        StreamSe* s = streamSe_[ch];
        if (!s || s->finished.load(std::memory_order_acquire)) continue;

        size_t offset = 0;
        while (offset < (size_t)len && !s->finished.load(std::memory_order_relaxed)) {
            size_t request = std::min(s->mixScratch.size(), (size_t)len - offset);
            request -= request % 4u;
            const size_t got = s->pcmRing.Read(s->mixScratch.data(), request);
            if (!got) break;

            int16_t* dst = reinterpret_cast<int16_t*>(dstBytes + offset);
            const int16_t* src = reinterpret_cast<const int16_t*>(s->mixScratch.data());
            const size_t samples = got / sizeof(int16_t);
            const size_t frames = got / 4u;
            for (size_t i = 0; i < samples; ++i) {
                const uint64_t frame = s->playedFrames + i / 2u;
                int gain = MixerVolume(s->volume, seMaster);
                if (s->fadeInFrames && frame < s->fadeInFrames)
                    gain = (int)((uint64_t)gain * frame / s->fadeInFrames);
                if (s->fadeOutFrames) {
                    const uint64_t remaining = s->fadeOutRemaining > i / 2u
                        ? s->fadeOutRemaining - i / 2u : 0;
                    gain = (int)((uint64_t)gain * remaining / s->fadeOutFrames);
                }
                const int mixed = (int)dst[i] + (int)src[i] * gain / MIX_MAX_VOLUME;
                dst[i] = (int16_t)std::clamp(mixed, -32768, 32767);
            }
            s->playedFrames += frames;
            if (s->fadeOutFrames) {
                if (frames >= s->fadeOutRemaining) {
                    s->fadeOutRemaining = 0;
                    s->finished.store(true, std::memory_order_release);
                } else {
                    s->fadeOutRemaining -= frames;
                }
            }
            offset += got;
        }
        if (!s->finished.load(std::memory_order_relaxed) &&
            s->decoderEof.load(std::memory_order_acquire) &&
            s->pcmRing.Available() == 0) {
            s->finished.store(true, std::memory_order_release);
        }
    }
}

void Audio::RetireFinishedStreams() {
    StreamSe* retired[kSeChannels] = {};
    SDL_LockAudio();
    for (int i = 0; i < kSeChannels; ++i) {
        if (streamSe_[i] && streamSe_[i]->finished.load(std::memory_order_acquire)) {
            retired[i] = streamSe_[i];
            streamSe_[i] = nullptr;
            se_[i].loop = false;
            se_[i].id = -1;
        }
    }
    SDL_UnlockAudio();
    for (StreamSe* s : retired) delete s;
}

void Audio::StopStreamSe(int ch, int fadeMs) {
    if (ch < 0 || ch >= kSeChannels) return;
    StreamSe* retired = nullptr;
    SDL_LockAudio();
    StreamSe* s = streamSe_[ch];
    if (s && fadeMs > 0) {
        s->fadeOutFrames = (uint64_t)fadeMs * 48u;
        s->fadeOutRemaining = s->fadeOutFrames;
    } else if (s) {
        retired = s;
        streamSe_[ch] = nullptr;
        se_[ch].loop = false;
        se_[ch].id = -1;
    }
    SDL_UnlockAudio();
    delete retired;
}
#endif

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
    bgmId_ = -1;
}

bool Audio::PlayBgm(int id, bool loop, int vol, Res& res) {
    if (noAudio_) return false;
    FreeBgm();
    bgmLoop_ = loop;
    bgmId_ = id;
    bgmScriptVol_ = vol;
    Mix_VolumeMusic(MixerVolume(vol, bgmVol_));
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
        bgmId_ = -1;
        Log(LogLevel::Warn, "audio: bgm %d missing", id);
        return false;
    }
    Log(LogLevel::Info, "audio: bgm %d found %s", id, a.name.c_str());
    bgmAData_ = res.Load(a.name);
    SDL_RWops* rw = SDL_RWFromMem(bgmAData_.data(), (int)bgmAData_.size());
    bgmA_ = rw ? Mix_LoadMUS_RW(rw, 1) : nullptr;   // frees rw
    if (!bgmA_) {
        bgmId_ = -1;
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
            bgmB_ = rwb ? Mix_LoadMUS_RW(rwb, 1) : nullptr;
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

float Audio::PlayVoice(int label, int id, int chr, int ch, int volume,
                       bool loop, Res& res) {
    const float seconds = PlayVoiceFile(
        Res::VoiceName(label, id, chr & 0xFF), ch, volume, loop, res);
    if (seconds > 0.0f && ch >= 0 && ch < kVoiceChannels) voice_[ch].id = id;
    return seconds;
}

float Audio::PlayVoiceFile(const std::string& name, int ch, int volume,
                           bool loop, Res& res) {
    if (noAudio_) return 0;
    Chan* cp = VoiceChan(ch);
    if (!cp) return 0;
    Chan& c = *cp;
    if (c.chunk) { Mix_HaltChannel(kVoiceBase + ch); Mix_FreeChunk(c.chunk); c.chunk = nullptr; }
    std::vector<uint8_t> data = res.Load(name);
    if (data.empty()) {
        Log(LogLevel::Info, "audio: voice %s missing", name.c_str());
        return 0;
    }
    SDL_RWops* rw = SDL_RWFromMem(data.data(), (int)data.size());
    c.chunk = rw ? Mix_LoadWAV_RW(rw, 1) : nullptr;
    if (!c.chunk) return 0;
    Mix_Volume(kVoiceBase + ch, MixerVolume(volume, voiceVol_));
    Mix_PlayChannel(kVoiceBase + ch, c.chunk, loop ? -1 : 0);
    c.startMs = SDL_GetTicks();
    c.lengthMs = (int)((uint64_t)c.chunk->alen * 1000 / 192000);   // 48k 16bit 立体声
    c.loop = loop;
    c.id = -1;
    c.volume = volume;
    return c.lengthMs / 1000.0f;
}

void Audio::StopVoice(int fadeMs, int ch) {
    if (noAudio_) return;
    Chan* cp = VoiceChan(ch);
    if (!cp) return;
    Chan& c = *cp;
    if (fadeMs > 0) Mix_FadeOutChannel(kVoiceBase + ch, fadeMs);
    else Mix_HaltChannel(kVoiceBase + ch);
    if (c.chunk && fadeMs <= 0) { Mix_FreeChunk(c.chunk); c.chunk = nullptr; }
}

void Audio::SetVoiceVolume(int ch, int vol, int fadeMs) {
    if (noAudio_) return;
    if (!VoiceChan(ch)) return;
    (void)fadeMs;
    VoiceChan(ch)->volume = vol;
    Mix_Volume(kVoiceBase + ch, MixerVolume(vol, voiceVol_));
}

float Audio::VoiceRemaining(int ch) const {
    if (noAudio_) return 0;
    const Chan* cp = const_cast<Audio*>(this)->VoiceChan(ch);
    if (!cp) return 0;
    const Chan& c = *cp;
    if (!c.chunk || c.loop) return 0;
    int elapsed = (int)(SDL_GetTicks() - c.startMs);
    return elapsed < c.lengthMs ? (c.lengthMs - elapsed) / 1000.0f : 0;
}

bool Audio::PlaySe(int ch, int id, bool loop, int fadeInMs, int vol, Res& res) {
#ifdef WA2_DIAG_DISABLE_SE
    // 单变量诊断 D1：保留 BGM、语音、音频设备和其他引擎行为，
    // 只阻断新崩溃栈命中的 SE 资源读取与解码路径。
    (void)fadeInMs;
    (void)vol;
    (void)res;
    Log(LogLevel::Warn, "audio: diagnostic D1 skipped SE ch=%d id=%d loop=%d",
        ch, id, loop ? 1 : 0);
    return false;
#else
    if (noAudio_) return false;
    // 自动分配:找空闲通道
    if (ch < 0) {
        for (int i = 0; i < kSeChannels; i++) {
#ifdef __SWITCH__
            if (streamSe_[i]) continue;
#endif
            if (!Mix_Playing(i)) { ch = i; break; }
        }
        if (ch < 0) ch = 0;
    }
    Chan* cp = SeChan(ch);
    if (!cp) return false;
    Chan& c = *cp;
#ifdef __SWITCH__
    StopStreamSe(ch, 0);
#endif
    if (c.chunk) { Mix_HaltChannel(ch); Mix_FreeChunk(c.chunk); c.chunk = nullptr; }
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
#ifdef __SWITCH__
    // 9712/9713/9714 都是 276 秒音轨，脚本实际却以 loop=0 启动。
    // 基线只流式处理 loop=1，会把它们一次性展开为约 159 MiB PCM。
    // D2 只改变这一个判定：长 SE 不论 loop 均走既有流式路径。
#ifdef WA2_DIAG_STREAM_LONG_SE
    const bool streamLongSe = data.size() >= 2u * 1024u * 1024u;
#else
    const bool streamLongSe = loop && data.size() >= 2u * 1024u * 1024u;
#endif
    if (streamLongSe &&
        TryPlayStreamSe(ch, id, loop, fadeInMs, vol, loc.name, std::move(data)))
        return true;
#endif
    SDL_RWops* rw = SDL_RWFromMem(data.data(), (int)data.size());
    c.chunk = rw ? Mix_LoadWAV_RW(rw, 1) : nullptr;
    if (!c.chunk) return false;
    Mix_Volume(ch, MixerVolume(vol, seVol_.load(std::memory_order_acquire)));
    if (fadeInMs > 0)
        Mix_FadeInChannelTimed(ch, c.chunk, loop ? -1 : 0, fadeInMs, -1);
    else
        Mix_PlayChannelTimed(ch, c.chunk, loop ? -1 : 0, -1);
    c.startMs = SDL_GetTicks();
    c.lengthMs = (int)((uint64_t)c.chunk->alen * 1000 / 192000);
    c.loop = loop;
    c.id = id;
    c.volume = vol;
    return true;
#endif
}

void Audio::StopSe(int ch, int fadeMs) {
    if (noAudio_) return;
    Chan* cp = SeChan(ch);
    if (!cp) return;
#ifdef __SWITCH__
    if (streamSe_[ch]) { StopStreamSe(ch, fadeMs); return; }
#endif
    Chan& c = *cp;
    if (fadeMs > 0) Mix_FadeOutChannel(ch, fadeMs);
    else Mix_HaltChannel(ch);
    if (c.chunk && fadeMs <= 0) { Mix_FreeChunk(c.chunk); c.chunk = nullptr; }
}

void Audio::SetSeVolume(int ch, int vol, int fadeMs) {
    if (noAudio_) return;
    Chan* cp = SeChan(ch);
    if (!cp) return;
#ifdef __SWITCH__
    if (streamSe_[ch]) {
        SDL_LockAudio();
        if (streamSe_[ch]) streamSe_[ch]->volume = vol;
        SDL_UnlockAudio();
        return;
    }
#endif
    (void)fadeMs;
    cp->volume = vol;
    Mix_Volume(ch, MixerVolume(vol, seVol_.load(std::memory_order_acquire)));
}

float Audio::SeRemaining(int ch) const {
    if (noAudio_) return 0;
#ifdef __SWITCH__
    if (ch >= 0 && ch < kSeChannels) {
        SDL_LockAudio();
        StreamSe* s = streamSe_[ch];
            const float remaining = (!s || s->loop ||
                                     s->finished.load(std::memory_order_acquire)) ? 0.0f
            : (float)(s->totalFrames > s->playedFrames
                ? (s->totalFrames - s->playedFrames) / 48000.0 : 0.0);
        SDL_UnlockAudio();
        if (s) return remaining;
    }
#endif
    const Chan* cp = const_cast<Audio*>(this)->SeChan(ch);
    if (!cp) return 0;
    const Chan& c = *cp;
    if (!c.chunk || c.loop) return 0;
    int elapsed = (int)(SDL_GetTicks() - c.startMs);
    return elapsed < c.lengthMs ? (c.lengthMs - elapsed) / 1000.0f : 0;
}

std::vector<Audio::LoopSeState> Audio::LoopingSe() const {
    std::vector<LoopSeState> out;
    if (noAudio_) return out;
#ifdef __SWITCH__
    SDL_LockAudio();
#endif
    for (int i = 0; i < kSeChannels; ++i) {
        bool streaming = false;
#ifdef __SWITCH__
        streaming = streamSe_[i] &&
                    !streamSe_[i]->finished.load(std::memory_order_acquire);
#endif
        if ((streaming || (se_[i].chunk && Mix_Playing(i))) && se_[i].loop)
            out.push_back({i, se_[i].id, se_[i].volume});
    }
#ifdef __SWITCH__
    SDL_UnlockAudio();
#endif
    return out;
}

void Audio::StopAll() {
    if (noAudio_) return;
    FreeBgm();
    for (int i = 0; i < kSeChannels; i++) {
#ifdef __SWITCH__
        StopStreamSe(i, 0);
#endif
        if (se_[i].chunk) { Mix_HaltChannel(i); Mix_FreeChunk(se_[i].chunk); se_[i].chunk = nullptr; }
    }
    for (int i = 0; i < kVoiceChannels; i++) {
        if (voice_[i].chunk) { Mix_HaltChannel(kVoiceBase + i); Mix_FreeChunk(voice_[i].chunk); voice_[i].chunk = nullptr; }
    }
}

} // namespace wa2
