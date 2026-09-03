// video.cpp — FFmpeg ASF/WMV3/WMA2 解码 + SDL2 纹理/后混音输出
//
// WMV3/VC-1 解码器没有帧级多线程能力,Switch 上 720p 软解的并行度来自
// 流水线:专用解码线程负责解复用+解码到帧队列,引擎线程只做 sws_scale
// 色彩转换和纹理上传。音轨同样由解码线程流式重采样为 48kHz 立体声 S16
// 写入 SPSC 环形缓冲,SDL 音频线程经 Audio 的后混音回调(MIX_CHANNEL_POST)
// 实时拉取。PC 与 Switch 共用这条路径,只有帧队列深度不同。
#include "video.h"
#include "audio.h"
#include "gfx.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#ifdef WA2_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include "audio_ring.h"
#endif

namespace wa2 {

#ifdef WA2_HAS_FFMPEG

// 影片音轨环形缓冲:48000Hz 立体声 S16 下约 5.3 秒,足以吸收解码线程
// 与音频设备时钟间的抖动;满时解码线程短暂等待,不丢样本。
static constexpr size_t kMovieAudioRingBytes = 512 * 1024;

// 帧队列深度。解码线程交付 BGRA 成品帧(720p 每帧 3.7MB),
// 引擎线程只做 SDL_UpdateTexture(memcpy)——曾把 sws 放在引擎线程,
// 引擎每秒 31 圈里只消化得动 4-8 帧,播放器被迫跳帧追赶时钟。
static constexpr int kVideoFrameQueueDepth = 4;

struct TimedFrame {
    AVFrame* frame = nullptr;      // WA2_HAS_FFMPEG 下由解码线程持有/释放
    std::vector<uint8_t> bgra;     // BGRA 成品像素(width×height×4)
    double pts = 0.0;
};

// 纯原子 SPSC 帧环:生产者=SDL 解码线程,消费者=引擎(libnx native)线程。
// 曾用 mutex+condition_variable 的实现,真机上引擎线程与解码线程对队列
// 状态的认知分裂(解码侧 Push 阻塞、消费侧 Peek 永远为空)——libnx 的
// native 线程与 pthread 线程混用条件变量不可靠。音频的 SpscByteRing 已
// 证明原子序号 SPSC 在本机跨线程可靠,这里沿用同一模式。
// 约定:closed 后生产者退出;消费者读空后自旋等待(消费线程有 60fps 节拍,
// 不会忙等浪费;解码线程队满时同样自旋并以 SDL_Delay 让出)。
class FrameQueue {
public:
    // 生产者:队满时按队首剩余时长等待;返回 false 表示已关闭。
    // 解码线程持有所需时钟(播放起点),由调用方换算剩余时间。
    bool Push(TimedFrame v, int waitMsHint) {
        for (;;) {
            const uint64_t w = write_.load(std::memory_order_relaxed);
            const uint64_t r = read_.load(std::memory_order_acquire);
            if ((int)(w - r) >= kVideoFrameQueueDepth) {
                if (closed_.load(std::memory_order_acquire)) {
                    return false;   // bgra vector 由 TimedFrame 自身析构
                }
                // 队满 = 已超前 ≥4 帧;按调用方的节奏提示休眠(waitMsHint
                // 由"队首领先时钟的余量"换算),1ms 下限兜底。
                int ms = waitMsHint > 1 ? waitMsHint : 1;
                if (ms > 100) ms = 100;
                SDL_Delay((Uint32)ms);
                continue;
            }
            slots_[w % kVideoFrameQueueDepth] = v;
            write_.store(w + 1, std::memory_order_release);
            return true;
        }
    }
    // 消费者:空时返回 false(不等待;引擎线程每帧都会再来)。
    bool Peek(TimedFrame* out) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;
        *out = slots_[r % kVideoFrameQueueDepth];
        return true;
    }
    void PopFront() { read_.fetch_add(1, std::memory_order_release); }
    void Close() { closed_.store(true, std::memory_order_release); }
    // 丢弃所有未消费帧并释放;仅 Stop 时与生产者互斥后调用。
    void Flush() {
        const uint64_t w = write_.load(std::memory_order_acquire);
        uint64_t r = read_.load(std::memory_order_relaxed);
        // 槽位 vector 由未来 Push 的 move 赋值覆盖;Stop 后不再有 Push,
        // 残留随 Impl 的有意泄漏一同回收。
        read_.store(w, std::memory_order_release);
    }
    // 已关闭且取空:解码侧不会再有新帧。
    bool ClosedEmpty() {
        return closed_.load(std::memory_order_acquire) &&
               read_.load(std::memory_order_acquire) ==
                   write_.load(std::memory_order_relaxed);
    }

private:
    alignas(64) TimedFrame slots_[kVideoFrameQueueDepth]{};
    alignas(64) std::atomic<uint64_t> read_{0};
    alignas(64) std::atomic<uint64_t> write_{0};
    std::atomic<bool> closed_{false};
};

// 帧时间轴:每个文件只允许一种轴。逐帧自适应会在"时间戳有效/无效"
// 的混合流上(如 mv010 部分 best_effort_timestamp>0、部分=0)产生两条
// 速率不同的轴交替,时间轴整体膨胀,队首永远超前时钟、消费被饿死。
// 首帧探测:时间戳轴可用则整流用它,否则整流用帧号/钳制帧率。
// 播放开始时由 Play 复位(引擎线程);探测结果存在 Impl,解码线程读写。
static AVCodecContext* OpenDecoder(AVFormatContext* fmt, int stream) {
    if (!fmt || stream < 0) return nullptr;
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;
    if (avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

#endif // WA2_HAS_FFMPEG

#ifdef WA2_HAS_FFMPEG
class MovieAudioBridge;
#endif

struct VideoPlayer::Impl {
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int texW = 0, texH = 0;   // 复用判定;视频纹理跨播放保留
    bool playing = false;
    double duration = 0.0;
    uint32_t startedMs = 0;
    int width = 0, height = 0;
    Audio* audio = nullptr;
    Gfx* gfx = nullptr;   // 安全直通:视频帧直写 softwareFrame

#ifdef WA2_HAS_FFMPEG
    // ---- 解码线程状态(Stop 中 join 之后才可安全销毁) ----
    // 用项目自己的 fopen 打开 sdmc:/...（已被资源加载证明可用），再包装成
    // FFmpeg AVIOContext;绕开 FFmpeg file 协议在 Switch newlib 上对 drive
    // 路径的 open() 失败。
    FILE* file = nullptr;
    long fileSize = 0;   // 打开时缓存;AVSEEK_SIZE 直接返回
    std::vector<uint8_t> avioBuf;
    AVIOContext* avio = nullptr;
    AVFormatContext* format = nullptr;
    AVCodecContext* videoCodec = nullptr;
    AVCodecContext* audioCodec = nullptr;
    SwrContext* audioSwr = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    // 用 SDL 线程而不是 std::thread:Switch(newlib+libnx)上 pthread 默认
    // 栈只有 128KB,FFmpeg 解 720p WMV3 会栈溢出;SDL 线程栈已被
    // make_stable_sdl2 无条件提到 1MiB,且协作式退出。
    SDL_Thread* decodeThread = nullptr;
    FrameQueue frames;
    std::atomic<bool> quit_{false};

    SpscByteRing<kMovieAudioRingBytes>* audioRing = nullptr;
    std::atomic<int> audioVolume255_{255};
    std::atomic<bool> audioEof_{false};
    std::atomic<size_t> audioFedBytes_{0};
    int64_t decodedFrames = 0;
    // 帧时间轴:有效帧率×帧号(绝对不用流时间戳,两平台 time_base 不同)。
    bool ptsFpsKnown = false;
    double ptsFps = 30.0;
    // ---- 引擎线程状态 ----
    SwsContext* sws = nullptr;
    double pendingPts = 0.0;
    // Switch 直通:最近一次到期的 BGRA 帧(引擎直接提交 framebuffer)。
    std::vector<uint8_t> lastFrame;
    std::string playingPath;
    std::unique_ptr<MovieAudioBridge> movieBridge;
#endif
};

VideoPlayer::~VideoPlayer() { Shutdown(); }

#ifdef WA2_HAS_FFMPEG

// 帧时间轴:统一用"有效帧率 × 帧号"。绝对不用流的时间戳——同一文件
// 的 time_base 在 PC(1/1000)与 Switch 真机(实测 1/100)上不同,pts 轴
// 比墙钟快 10 倍,队首永远超前时钟、消费被饿死(ptsdelta 1.3-1.7 实测)。
// 帧率取 avg_frame_rate/r_frame_rate 的有效值;ASF 的 1000/1 占位与
// 无效值一律按 30fps(WA2 视频只有 24/30fps,24fps 的流 r_frame_rate
// 会正确给出 24/1)。
static double FramePts(VideoPlayer::Impl* p, AVFrame* frame) {
    (void)frame;
    if (!p->ptsFpsKnown) {
        p->ptsFpsKnown = true;
        AVStream* st = p->format->streams[p->videoStream];
        AVRational fr = st->avg_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
        double rate = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 30.0;
        if (rate <= 1.0 || rate > 120.0) rate = 30.0;
        p->ptsFps = rate;
        Log(LogLevel::Info, "video: pts axis fps=%.3f (avg=%d/%d r=%d/%d)",
            rate, st->avg_frame_rate.num, st->avg_frame_rate.den,
            st->r_frame_rate.num, st->r_frame_rate.den);
    }
    return (double)(p->decodedFrames++) / p->ptsFps;
}

// 解码线程主体:解复用+解码视频帧入队,音轨重采样写入环形缓冲。
// 由 Play() 在所有状态就绪、音频源注册完成后启动。SDL 线程约定返回 int。
static int SDLCALL DecodeThread(void* opaque) {
    VideoPlayer::Impl* p = static_cast<VideoPlayer::Impl*>(opaque);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> audioScratch;
    // 音频包暂存队列:音频 ring 满(消费节流)时音频包先攒在这里,在视频
    // Push 自旋的间隙里泵入音频解码器。曾原地 SDL_Delay(4) 等消费——
    // 同线程的音频节流把视频解码一起堵死,供给断流造成播放卡顿。
    std::vector<AVPacket*> pendingAudio;
    bool audioDrainEof = false;
    bool videoEof = false;
    bool audioEof = false;
    uint32_t lastStatMs = SDL_GetTicks();
    int64_t statPackets = 0, statFrames = 0, statAudioBytes = 0;

    // 把暂存的音频包泵进音频解码器(受 ring 水位与退出标志约束)。
    auto PumpAudioPackets = [&]() {
        while (!pendingAudio.empty() &&
               !p->quit_.load(std::memory_order_relaxed)) {
            if (p->audioRing->Free() < 48000 * 2 * 2) return;   // ring 满,下次再泵
            AVPacket* ap = pendingAudio.front();
            if (avcodec_send_packet(p->audioCodec, ap) < 0) {
                av_packet_free(&ap);
                pendingAudio.erase(pendingAudio.begin());
                continue;
            }
            av_packet_free(&ap);
            pendingAudio.erase(pendingAudio.begin());
            // 尽量收干解码器输出写入 ring。
            while (!p->quit_.load(std::memory_order_relaxed)) {
                int got = avcodec_receive_frame(p->audioCodec, frame);
                if (got == AVERROR(EAGAIN)) break;
                if (got < 0) { audioEof = true; return; }
                const int outSamples = (int)av_rescale_rnd(
                    swr_get_delay(p->audioSwr, p->audioCodec->sample_rate) +
                        frame->nb_samples,
                    48000, p->audioCodec->sample_rate, AV_ROUND_UP);
                const size_t need = (size_t)std::max(outSamples, 0) * 2 * sizeof(int16_t);
                if (audioScratch.size() < need) audioScratch.resize(need);
                uint8_t* dst[1] = {audioScratch.data()};
                int made = swr_convert(p->audioSwr, dst, outSamples,
                                       (const uint8_t**)frame->extended_data,
                                       frame->nb_samples);
                av_frame_unref(frame);
                if (made <= 0) continue;
                const size_t bytes = (size_t)made * 2 * sizeof(int16_t);
                size_t written = 0;
                while (written < bytes) {
                    if (p->quit_.load(std::memory_order_relaxed)) return;
                    const size_t w = p->audioRing->Write(audioScratch.data() + written,
                                                         bytes - written);
                    written += w;
                    if (written < bytes) return;   // ring 又满,等下轮
                    statAudioBytes += (int64_t)w;
                }
                p->audioFedBytes_.fetch_add(written, std::memory_order_release);
            }
        }
    };

    if (packet && frame) {
        while (!p->quit_.load(std::memory_order_relaxed)) {
            // 每秒输出一次解码心跳:真机上用它区分"解码线程死了"与"渲染没画"。
            {
                const uint32_t nowMs = SDL_GetTicks();
                if (nowMs - lastStatMs >= 1000) {
                    Log(LogLevel::Info,
                        "video: decode alive pkts=%lld frames=%lld audioB=%lld eof=%d/%d",
                        (long long)statPackets, (long long)statFrames,
                        (long long)statAudioBytes, (int)videoEof, (int)audioEof);
                    lastStatMs = nowMs;
                    statPackets = statFrames = statAudioBytes = 0;
                }
            }
            // 音频环形缓冲剩余空间不足一秒时,不再原地等待——曾让音频
            // 的消费节流把同线程的视频解码一起堵死(队列空、供给断流,
            // 播放卡顿)。改为:本循环只解视频包;音频包暂存,在视频
            // Push 的自旋间隙里泵入音频解码器(见 PushAudioPump)。

            int r = av_read_frame(p->format, packet);
            ++statPackets;
            if (r < 0) {
                char err[128];
                av_strerror(r, err, sizeof(err));
                Log(LogLevel::Warn, "video: read_frame end err=%d (%s) pkts=%lld",
                    r, err, (long long)statPackets);
                avcodec_send_packet(p->videoCodec, nullptr);
                if (p->audioCodec && p->audioStream >= 0)
                    avcodec_send_packet(p->audioCodec, nullptr);
                videoEof = audioEof = true;
                audioDrainEof = true;
            } else if (packet->stream_index == p->videoStream) {
                if (avcodec_send_packet(p->videoCodec, packet) >= 0) {
                    while (!p->quit_.load(std::memory_order_relaxed)) {
                        int got = avcodec_receive_frame(p->videoCodec, frame);
                        if (got == AVERROR(EAGAIN)) break;
                        if (got < 0) { videoEof = true; break; }
                        TimedFrame tf;
                        tf.pts = FramePts(p, frame);
                        // sws 在解码线程完成(引擎线程只 memcpy 上屏)。
                        tf.bgra.resize((size_t)p->width * p->height * 4);
                        uint8_t* dst[4] = {tf.bgra.data(), nullptr, nullptr, nullptr};
                        int stride[4] = {p->width * 4, 0, 0, 0};
                        sws_scale(p->sws, frame->data, frame->linesize, 0,
                                  p->height, dst, stride);
                        av_frame_unref(frame);
                        ++statFrames;
                        // 节奏控制(在 Push 之前直接休眠——曾把 waitMs 只
                        // 传给 Push 的队满自旋,而队列深度 4 几乎不会满,
                        // 节奏控制从未生效,解码 60fps 暴冲把引擎线程饿到
                        // 4-8 帧/秒):帧领先时钟超过 2 帧就循环休眠到
                        // lead ≤66ms。休眠以 8ms 为步长,期间持续泵暂存
                        // 的音频包——音频供给与视频节奏互不牺牲。
                        {
                            double clock =
                                (double)(SDL_GetTicks() - p->startedMs) / 1000.0;
                            double lead = tf.pts - clock;
                            while (lead > 0.066 &&
                                   !p->quit_.load(std::memory_order_relaxed)) {
                                SDL_Delay(8);
                                PumpAudioPackets();
                                clock = (double)(SDL_GetTicks() - p->startedMs) / 1000.0;
                                lead = tf.pts - clock;
                            }
                        }
                        if (!p->frames.Push(std::move(tf), 1)) {
                            videoEof = true;   // 队列已关(Stop)
                            break;
                        }
                    }
                }
            } else if (packet->stream_index == p->audioStream) {
                // ring 满:暂存音频包,在视频 Push 间隙泵入。不阻塞视频。
                if (p->audioRing->Free() < 48000 * 2 * 2) {
                    AVPacket* stash = av_packet_clone(packet);
                    if (stash) pendingAudio.push_back(stash);
                } else if (avcodec_send_packet(p->audioCodec, packet) >= 0) {
                    while (!p->quit_.load(std::memory_order_relaxed)) {
                        int got = avcodec_receive_frame(p->audioCodec, frame);
                        if (got == AVERROR(EAGAIN)) break;
                        if (got < 0) { audioEof = true; break; }
                        // 流式重采样:按 swr 延迟+本帧样本数预留空间。
                        const int outSamples = (int)av_rescale_rnd(
                            swr_get_delay(p->audioSwr, p->audioCodec->sample_rate) +
                                frame->nb_samples,
                            48000, p->audioCodec->sample_rate, AV_ROUND_UP);
                        const size_t need = (size_t)std::max(outSamples, 0) * 2 * sizeof(int16_t);
                        if (audioScratch.size() < need) audioScratch.resize(need);
                        uint8_t* dst[1] = {audioScratch.data()};
                        int made = swr_convert(p->audioSwr, dst, outSamples,
                                               (const uint8_t**)frame->extended_data,
                                               frame->nb_samples);
                        av_frame_unref(frame);
                        if (made <= 0) continue;
                        const size_t bytes = (size_t)made * 2 * sizeof(int16_t);
                        // 写满环形缓冲前分块等待,期间检查退出标志。
                        size_t written = 0;
                        while (written < bytes) {
                            if (p->quit_.load(std::memory_order_relaxed)) break;
                            const size_t w = p->audioRing->Write(audioScratch.data() + written,
                                                                 bytes - written);
                            written += w;
                            if (written < bytes) SDL_Delay(2);
                        }
                        p->audioFedBytes_.fetch_add(written, std::memory_order_release);
                        statAudioBytes += (int64_t)written;
                    }
                }
            }
            av_packet_unref(packet);
            // 每轮循环泵一次暂存音频包,保持音频供给。
            PumpAudioPackets();
            // 双流都 EOF(且暂存音频泵完)后必须退出循环——曾因重构遗漏
            // 此退出,EOF 后线程空转、队列永不 Close、视频永不结束、脚本
            // 卡死在 wait_.movie(真机实测视频播完不返回游戏)。
            if (videoEof && audioEof &&
                (pendingAudio.empty() || p->quit_.load(std::memory_order_relaxed))) {
                break;
            }
        }
    }

    // 解码侧标记音轨结束;引擎线程据 fed-drained 判断是否播完。
    p->audioEof_.store(true, std::memory_order_release);
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    for (AVPacket* ap : pendingAudio) av_packet_free(&ap);
    p->frames.Close();   // 队列关闭后引擎线程 Pop 会在取空后返回 false
    return 0;
}

#endif // WA2_HAS_FFMPEG

#ifdef WA2_HAS_FFMPEG
// ffmpeg 日志转接到项目 Log:默认实现走 fprintf(stderr),在 Switch 的
// hbloader 环境下 stderr 的行为不可控,统一收口。
static void SDLCALL wa2_av_log(void* /*avcl*/, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_INFO) return;
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, vl);
    if (n <= 0) return;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    Log(level <= AV_LOG_ERROR ? LogLevel::Warn : LogLevel::Info, "ffmpeg: %s", buf);
}
#endif

bool VideoPlayer::Init(SDL_Renderer* renderer) {
    if (!p_) p_ = new Impl();
    p_->renderer = renderer;
    p_->audio = audio_;
#ifdef WA2_HAS_FFMPEG
    av_log_set_callback(wa2_av_log);
#endif
    return renderer != nullptr;
}

// engine.cpp 先 Init(renderer) 再 BindAudio(&audio_),所以 Impl::audio
// 必须在这里同步。曾因漏赋值导致音频回调从未挂上、解码线程被音频环形
// 缓冲反压卡死(画面冻结在首帧)。
void VideoPlayer::BindAudio(Audio* audio) {
    audio_ = audio;
    if (p_) p_->audio = audio;
}

void VideoPlayer::BindGfx(Gfx* gfx) {
    if (p_) p_->gfx = gfx;
}

#ifdef WA2_HAS_FFMPEG

// MovieAudioSource:由 Audio 后混音回调在 SDL 音频线程调用。
// 注意 ReadMoviePcm 依赖的 Impl 在 Stop()/Shutdown() join 解码线程、
// 注销音频源之后才销毁,回调期间指针稳定。
class MovieAudioBridge : public MovieAudioSource {
public:
    explicit MovieAudioBridge(VideoPlayer::Impl* impl) : impl_(impl) {}
    size_t ReadMoviePcm(void* dst, size_t bytes) override {
        return impl_->audioRing->Read(dst, bytes);
    }
    int MovieVolume255() const override {
        return impl_->audioVolume255_.load(std::memory_order_relaxed);
    }
private:
    VideoPlayer::Impl* impl_;
};

// FFmpeg file 协议在 Switch 上用 open("sdmc:/...") 失败,但项目自己的
// fopen("sdmc:/...") 已被资源加载证明可用。这里用一个 FILE* 后端 + 自定义
// AVIO 回调,绕开 file 协议,直接在已有的 fopen 上喂给 libavformat。
// data 即 Impl*,其 FILE* 由 OpenMovie 持有。
// AVIO 回调诊断:open_input 期间记录前几次读取,真机崩溃时定位回调内问题。
static std::atomic<int> g_avioCalls{0};
static int AvioRead(void* opaque, uint8_t* buf, int size) {
    VideoPlayer::Impl* p = static_cast<VideoPlayer::Impl*>(opaque);
    if (g_avioCalls.fetch_add(1, std::memory_order_relaxed) < 4) {
        Log(LogLevel::Info, "video: avio read #%d size=%d", g_avioCalls.load(std::memory_order_relaxed), size);
    }
    if (!p->file) return AVERROR(EIO);
    if (size <= 0) return 0;
    // 循环读满:sdmc 的 fread 可能够短读;返回 0 会被 avio 当作流结束,
    // 曾导致 IC 视频在第 64 个包处提前 EOF(画面无输出)。
    size_t total = 0;
    while (total < (size_t)size) {
        const size_t got = fread(buf + total, 1, (size_t)size - total, p->file);
        if (got == 0) {
            if (ferror(p->file)) return AVERROR(EIO);
            if (!feof(p->file)) {
                Log(LogLevel::Warn,
                    "video: avio read0 feof=0 ferror=0 pos=%ld want=%d got=%zu",
                    ftell(p->file), size, total);
            }
            break;   // 真正的 EOF(或无法继续读)
        }
        total += got;
    }
    return (int)total;
}

static int64_t AvioSeek(void* opaque, int64_t offset, int whence) {
    VideoPlayer::Impl* p = static_cast<VideoPlayer::Impl*>(opaque);
    if (!p->file) return AVERROR(EIO);
    // avio 的 whence 是 AVSEEK_SET(0)/AVSEEK_CUR(1)/AVSEEK_END(2),可能
    // 再按位或 AVSEEK_SIZE/AVSEEK_FORCE;AVSEEK_SIZE 只查询大小,不移动。
    if (whence & AVSEEK_SIZE) {
        // 打开时缓存的大小;sdmc 上反复 fseek(END) 的行为不可靠。
        return p->fileSize;
    }
    const int base = whence & 0xFFFF;
    const int stdioWhence = (base == SEEK_END) ? SEEK_END
                          : (base == SEEK_CUR) ? SEEK_CUR
                          : SEEK_SET;
    // 边界钳制:负偏移/超文件尾的 seek 一律失败。sdmc 的 fseek 对越界
    // 请求的行为未定义,静默成功会让 avio 认为位置正确,读到错位数据。
    long target = (long)offset;
    if (stdioWhence != SEEK_SET) {
        const long cur = ftell(p->file);
        if (cur < 0) return AVERROR(EIO);
        target = stdioWhence == SEEK_CUR ? cur + (long)offset
                                         : p->fileSize + (long)offset;
    }
    if (target < 0 || (p->fileSize > 0 && target > p->fileSize))
        return AVERROR(EINVAL);
    if (fseek(p->file, target, SEEK_SET) != 0) return AVERROR(EIO);
    const long pos = ftell(p->file);
    if (pos != target) {
        // sdmc 上 fseek 报告成功但位置不对:当作失败,让 avio 走顺序读。
        return AVERROR(EIO);
    }
    return pos;
}

// 用项目 fopen 打开影片文件并包装成 AVIO。返回的 AVIO 挂到 format->pb。
static bool OpenMovieViaFopen(VideoPlayer::Impl* p, const std::string& path) {
    p->file = fopen(path.c_str(), "rb");
    if (!p->file) {
        Log(LogLevel::Warn, "video: fopen failed for %s", path.c_str());
        return false;
    }
    // 打开时确定文件大小并缓存:AVSEEK_SIZE 直接返回,不再每次 fseek(END)。
    if (fseek(p->file, 0, SEEK_END) == 0) {
        p->fileSize = ftell(p->file);
        fseek(p->file, 0, SEEK_SET);
    } else {
        p->fileSize = 0;
    }
    Log(LogLevel::Info, "video: fopen ok %s size=%ld", path.c_str(), p->fileSize);
    if (p->fileSize <= 0) {
        Log(LogLevel::Warn, "video: file size query failed for %s", path.c_str());
    }
    // AVIO 缓冲约 256KB(720p 视频包足够,避免频繁回调)。
    if (p->avioBuf.empty()) p->avioBuf.resize(256 * 1024);
    p->avio = avio_alloc_context(p->avioBuf.data(), (int)p->avioBuf.size(),
                                 0, p, AvioRead, nullptr, AvioSeek);
    if (!p->avio) {
        fclose(p->file);
        p->file = nullptr;
        return false;
    }
    Log(LogLevel::Info, "video: fopen+avio ok %s", path.c_str());
    return true;
}

static bool OpenMovie(VideoPlayer::Impl* p, const std::string& path) {
    // 用自定义 AVIO 而非 paths:Switch 上 FFmpeg 的 file 协议 open() 认不了
    // sdmc:/,而 fopen 可以。PC 上同样走 fopen(路径可用),行为一致。
    if (!OpenMovieViaFopen(p, path)) return false;
    p->format = avformat_alloc_context();
    if (!p->format) {
        Log(LogLevel::Warn, "video: alloc format context failed");
        return false;
    }
    p->format->pb = p->avio;
    Log(LogLevel::Info, "video: open_input begin");
    g_avioCalls.store(0, std::memory_order_relaxed);
    // 显式指定 asf demuxer:跳过 av_probe_input_buffer 的反复扩缓冲/seek
    // 探测;真机在 open_input 内崩溃仅发生于 IC 子目录视频,先收窄路径。
    AVInputFormat* ifmt = const_cast<AVInputFormat*>(av_find_input_format("asf"));
    if (avformat_open_input(&p->format, "", ifmt, nullptr) < 0) {
        Log(LogLevel::Warn, "video: open_input failed for %s", path.c_str());
        return false;
    }
    Log(LogLevel::Info, "video: find_stream_info begin");
    if (avformat_find_stream_info(p->format, nullptr) < 0) {
        Log(LogLevel::Warn, "video: find_stream_info failed for %s", path.c_str());
        return false;
    }
    p->videoStream = av_find_best_stream(p->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    Log(LogLevel::Info, "video: best_stream=%d", p->videoStream);
    p->videoCodec = OpenDecoder(p->format, p->videoStream);
    if (p->videoStream < 0 || !p->videoCodec) {
        Log(LogLevel::Warn, "video: no supported video stream in %s", path.c_str());
        return false;
    }
    p->width = p->videoCodec->width;
    p->height = p->videoCodec->height;
    p->duration = p->format->duration > 0
        ? p->format->duration / (double)AV_TIME_BASE : 0.0;
    Log(LogLevel::Info, "video: open ok %dx%d %.2fs", p->width, p->height, p->duration);
    return true;
}

static bool OpenMovieAudio(VideoPlayer::Impl* p) {
    p->audioStream = av_find_best_stream(p->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (p->audioStream < 0) return false;
    p->audioCodec = OpenDecoder(p->format, p->audioStream);
    if (!p->audioCodec || p->audioCodec->sample_rate <= 0) return false;
    AVChannelLayout stereo{};
    av_channel_layout_default(&stereo, 2);
    const int rc = swr_alloc_set_opts2(&p->audioSwr,
                                        &stereo, AV_SAMPLE_FMT_S16, 48000,
                                        &p->audioCodec->ch_layout,
                                        p->audioCodec->sample_fmt,
                                        p->audioCodec->sample_rate,
                                        0, nullptr);
    if (rc < 0 || !p->audioSwr || swr_init(p->audioSwr) < 0) {
        av_channel_layout_uninit(&stereo);
        Log(LogLevel::Warn, "video: audio resampler init failed");
        return false;
    }
    av_channel_layout_uninit(&stereo);
    return true;
}

static void CloseMovieCodecs(VideoPlayer::Impl* p) {
    swr_free(&p->audioSwr);
    avcodec_free_context(&p->audioCodec);
    avcodec_free_context(&p->videoCodec);
    if (p->format) {
        // 手动挂的 pb 不会被 avformat_close_input 释放,需先断开再关;断言 pb 就是我们自己的 avio。
        if (p->format->pb == p->avio) p->format->pb = nullptr;
        avformat_close_input(&p->format);
    }
    if (p->avio) {
        // avio_alloc_context 的 buffer 由调用方(avioBuf vector)拥有,
        // avio_context_free 只释放 AVIOContext 结构体本身,不碰 buffer。
        avio_context_free(&p->avio);
    }
    p->avio = nullptr;
    p->avioBuf.clear();
    p->avioBuf.shrink_to_fit();
    if (p->file) { fclose(p->file); p->file = nullptr; }
    p->videoStream = p->audioStream = -1;
    p->decodedFrames = 0;
    p->ptsFpsKnown = false;
    p->audioEof_.store(false, std::memory_order_relaxed);
}

#endif // WA2_HAS_FFMPEG

bool VideoPlayer::Play(const std::string& path, int volume255) {
    Stop();
    if (!p_ || !p_->renderer) return false;
#ifndef WA2_HAS_FFMPEG
    (void)path; (void)volume255;
    Log(LogLevel::Warn, "video: FFmpeg support not built");
    return false;
#else
    if (!OpenMovie(p_, path)) {
        CloseMovieCodecs(p_);
        return false;
    }
    if (!OpenMovieAudio(p_)) {
        Log(LogLevel::Warn, "video: audio stream unavailable in %s", path.c_str());
        // 无声电影照样播放。
    }

    // POINT(最近邻):A57 上 720p 色彩转换比 BILINEAR 快约一半,视频观感
    // 差异可忽略;BILINEAR 在跳帧卡顿面前毫无意义。
    // 目标必须是 RGBA:Switch 上视频帧经 memcpy 直写 softwareFrame_
    // (SDL_PIXELFORMAT_RGBA32);曾用 BGRA,红蓝通道互换、画面整体偏蓝。
    p_->sws = sws_getContext(p_->width, p_->height, p_->videoCodec->pix_fmt,
                             p_->width, p_->height, AV_PIX_FMT_RGBA,
                             SWS_POINT, nullptr, nullptr, nullptr);
    Log(LogLevel::Info, "video: sws ok");
    // 纹理复用:同尺寸重播直接沿用;销毁+重建 3.7MB STREAMING 纹理在
    // 第二次播放时引入大幅引擎线程停顿(实测第二次开头 draws/s 掉到 1)。
    if (p_->texture &&
        (p_->texW != p_->width || p_->texH != p_->height)) {
        SDL_DestroyTexture(p_->texture);
        p_->texture = nullptr;
    }
    if (!p_->texture) {
        p_->texture = SDL_CreateTexture(p_->renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        p_->width, p_->height);
        p_->texW = p_->width;
        p_->texH = p_->height;
    }
    // 清掉复用纹理里上一场视频的残留像素:新视频第一帧到期前,Render
    // 会先画上纹理,若不清会闪现上个视频的最后一帧/开头帧。
    {
        std::vector<uint8_t> black((size_t)p_->width * p_->height * 4, 0);
        SDL_UpdateTexture(p_->texture, nullptr, black.data(), p_->width * 4);
    }
    Log(LogLevel::Info, "video: texture ok");
    if (!p_->sws || !p_->texture) {
        Log(LogLevel::Warn, "video: texture/scale setup failed for %s", path.c_str());
        CloseMovieCodecs(p_);
        return false;
    }

    if (!p_->audioRing) p_->audioRing = new SpscByteRing<kMovieAudioRingBytes>();
    // 清掉上一场视频留在环形缓冲里的 PCM:ring 跨播放复用,不 Reset 的话
    // 新视频开头会先放出上一场的音频残留。
    p_->audioRing->Reset();
    // 同理清掉残留的上一帧画面:PresentVideoFrame 会把 lastFrame 直写上屏,
    // 新视频首帧到期前若不清,会先闪现上一场的最后一帧。
    {
        std::vector<uint8_t>().swap(p_->lastFrame);
    }
    p_->audioVolume255_.store(std::clamp(volume255, 0, 255), std::memory_order_relaxed);
    p_->pendingPts = 0.0;
    p_->quit_.store(false, std::memory_order_relaxed);
    p_->playingPath = path;
    p_->startedMs = SDL_GetTicks();
    p_->playing = true;

    // 先注册音频源(占住静音通道保证后混音回调持续运行),再启动解码线程;
    // 这样解码线程开始写 ring 时消费者已经就位。
    p_->movieBridge = std::make_unique<MovieAudioBridge>(p_);
    if (p_->audio) {
        p_->audio->SetMovieAudioSource(p_->movieBridge.get());
    }
    // 4MiB:WMV3/VC1 软解调用链深,devkitPro 默认 1MiB 不够(真机上首帧后
// 解码线程即失联,画面冻结)。引擎线程本身也用 4MiB。
    p_->decodeThread = SDL_CreateThreadWithStackSize(DecodeThread, "wa2movie",
                                                      4u * 1024u * 1024u, p_);
    if (!p_->decodeThread) {
        Log(LogLevel::Error, "video: cannot start decode thread: %s", SDL_GetError());
        if (p_->audio) p_->audio->SetMovieAudioSource(nullptr);
        CloseMovieCodecs(p_);
        p_->playing = false;
        return false;
    }
    Log(LogLevel::Info, "video: playing %s (%dx%d, %.3fs, ring=%zu)",
        path.c_str(), p_->width, p_->height, p_->duration, kMovieAudioRingBytes);
    return true;
#endif
}

void VideoPlayer::Update() {
    if (!p_ || !p_->playing) return;
#ifdef WA2_HAS_FFMPEG
    const double elapsed = (SDL_GetTicks() - p_->startedMs) / 1000.0;

    // 落后太多直接跳帧;队首未到时刻则展示帧已在纹理中,无事可做。
    // 到期帧先缓存到 lastFrame;直写 softwareFrame 由 PresentVideoFrame
    // 在引擎 Render() 之后调用——Render 开头的 Clear() 会清掉本帧
    // softwareFrame,写在 Update 里必被清成黑屏(真机已实测)。
    // 消费策略:队列驱动。队列里有什么就展示什么(每圈至多 2 帧),
    // pts 只用于丢弃严重超前的帧(>0.5s,暴冲保护)。
    // 依赖 pts 到期判据的旧逻辑曾把大幅超前的帧卡死在队首,后续帧
    // 全部堵死,直到时钟爬过它——真机上表现为周期性卡顿。
    TimedFrame tf;
    int consumed = 0;
    while (consumed < 2 && p_->frames.Peek(&tf)) {
        if (tf.pts > elapsed + 0.5) break;   // 严重超前:等时钟
        p_->frames.PopFront();
        p_->pendingPts = tf.pts;
#ifndef __SWITCH__
        // PC:无 softwareFrame,走纹理 + Render 常规 blit。
        SDL_UpdateTexture(p_->texture, nullptr, tf.bgra.data(), p_->width * 4);
#else
        // Switch:缓存最近帧,PresentVideoFrame 在 Render 后直写 softwareFrame。
        if (p_->lastFrame.empty()) p_->lastFrame.resize((size_t)p_->width * p_->height * 4);
        std::memcpy(p_->lastFrame.data(), tf.bgra.data(), p_->lastFrame.size());
#endif
        ++consumed;
    }
    // 消费心跳:每秒一次,确认引擎线程的帧消费与上屏是否推进。
    {
        static uint32_t lastBeat = 0;
        static int64_t lastConsumed = 0;
        static int64_t totalConsumed = 0;
        if (elapsed < 0.05f) {   // 新一次播放:复位跨播放残留的基准。
            lastBeat = 0;
            lastConsumed = totalConsumed;
        }
        totalConsumed += consumed;
        const uint32_t nowMs = SDL_GetTicks();
        if (nowMs - lastBeat >= 1000) {
            TimedFrame head;
            const bool has = p_->frames.Peek(&head);
            // pts 步进:上一秒 pending 到现在的增量/消费数。健康值≈0.033
            // (30fps);偏离即时间轴与墙钟不等速——队首超前的真凶。
            static double lastPending = 0.0;
            const double ptsDelta = p_->pendingPts - lastPending;
            lastPending = p_->pendingPts;
            Log(LogLevel::Info,
                "video: consume elapsed=%.2f consumed/s=%lld queue=%d head_pts=%.3f lead=%.3f ptsdelta=%.3f",
                elapsed, (long long)(totalConsumed - lastConsumed),
                has ? 1 : 0, has ? head.pts : -1.0,
                has ? head.pts - elapsed : 0.0, ptsDelta);
            lastBeat = nowMs;
            lastConsumed = totalConsumed;
        }
    }

    // 播放结束:视频队列关闭取空 + 音轨消费完毕(或无音轨)。
    const bool audioDone = (p_->audioStream < 0) ||
                           (p_->audioEof_.load(std::memory_order_acquire) &&
                            p_->audioRing->Available() == 0);
    TimedFrame last;
    const bool videoDone = !p_->frames.Peek(&last) &&
                           p_->frames.ClosedEmpty();
    if (videoDone && audioDone) {
        if (p_->duration <= 0.0 || elapsed >= p_->duration - 0.25) {
            Stop();
            return;
        }
    }
#endif
}

void VideoPlayer::Render() {
    // Switch:视频帧由 PresentVideoFrame 在引擎 Render() 之后直写
    // softwareFrame——Render 开头的 Clear() 会清空它,直写必须在其后,
    // 这里不做任何绘制。PC:走纹理 blit(SDL 自动处理格式转换)。
    if (!p_ || !p_->playing) return;
#ifndef __SWITCH__
    if (!p_->texture) return;
    const int outW = 1280, outH = 720;
    const float scale = std::min(outW / (float)p_->width, outH / (float)p_->height);
    SDL_Rect dst{(outW - (int)(p_->width * scale)) / 2,
                 (outH - (int)(p_->height * scale)) / 2,
                 (int)(p_->width * scale), (int)(p_->height * scale)};
    SDL_RenderCopy(p_->renderer, p_->texture, nullptr, &dst);
#endif
    {
        static uint32_t lastBeat = 0;
        static uint64_t draws = 0;
        ++draws;
        const uint32_t nowMs = SDL_GetTicks();
        if (nowMs - lastBeat >= 1000) {
#ifdef WA2_HAS_FFMPEG
            Log(LogLevel::Info, "video: render draws/s=%llu pending_pts=%.3f",
                (unsigned long long)(draws), p_->pendingPts);
#else
            Log(LogLevel::Info, "video: render draws/s=%llu",
                (unsigned long long)(draws));
#endif
            lastBeat = nowMs;
            draws = 0;
        }
    }
}

void VideoPlayer::PresentVideoFrame() {
    if (!p_ || !p_->playing || !p_->gfx) return;
#ifdef WA2_HAS_FFMPEG
    if (p_->lastFrame.empty()) return;
    p_->gfx->PresentVideoFrameDirect(p_->lastFrame.data(), p_->width * 4);
#endif
}

void VideoPlayer::Stop() {
    if (!p_) return;
#ifdef WA2_HAS_FFMPEG
    Log(LogLevel::Info, "video: stop begin playing=%d", p_->playing ? 1 : 0);
    // 先把音频源从 Audio 撤下、让解码线程退出,再 join;避免回调期间
    // 环形缓冲被销毁。
    if (p_->audio && p_->playing)
        p_->audio->SetMovieAudioSource(nullptr);
    if (p_->playing) {
        p_->quit_.store(true, std::memory_order_relaxed);
        // 解码线程可能正阻塞在 Push 的队满等待上;先关队列唤醒它,再等线程。
        p_->frames.Close();
        if (p_->decodeThread) {
            int status;
            SDL_WaitThread(p_->decodeThread, &status);
            p_->decodeThread = nullptr;
            Log(LogLevel::Info, "video: decode thread joined status=%d", status);
        }
    }
    p_->frames.Flush();
    // 纹理保留复用:销毁 3.7MB STREAMING 纹理会在下一次 Play 重建时造成
    // 引擎线程长停顿(第二次播放实测 draws/s 掉到 1)。
    // Shutdown 路径随 Impl 的有意泄漏一同回收。
    sws_freeContext(p_->sws); p_->sws = nullptr;
    CloseMovieCodecs(p_);
    p_->playing = false;
    p_->duration = 0.0;
    p_->playingPath.clear();
    Log(LogLevel::Info, "video: stop complete");
#endif
}

void VideoPlayer::Shutdown() {
    if (!p_) return;
    Stop();
    // 有意不释放 p_(进程退出前最后一次调用):真机上 delete Impl 在
    // audout 会话与 SDL 线程全部关停后仍触发 hbl User Break(大气层崩溃,
    // 嫌疑为 libnx pthread 条件变量析构)。所有真实资源——解码器、AVIO、
    // 文件、纹理、解码线程——已在 Stop() 释放,剩余 STL 容器由进程退出
    // 回收,泄漏量固定且微小。
    Log(LogLevel::Info, "video: shutdown complete (impl intentionally leaked)");
    p_ = nullptr;
}

bool VideoPlayer::Playing() const { return p_ && p_->playing; }
double VideoPlayer::Duration() const { return p_ ? p_->duration : 0.0; }

} // namespace wa2
