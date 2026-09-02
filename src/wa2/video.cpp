// video.cpp — FFmpeg ASF/WMV3/WMA2 解码 + SDL2 纹理/后混音输出
//
// WMV3/VC-1 解码器没有帧级多线程能力,Switch 上 720p 软解的并行度来自
// 流水线:专用解码线程负责解复用+解码到帧队列,引擎线程只做 sws_scale
// 色彩转换和纹理上传。音轨同样由解码线程流式重采样为 48kHz 立体声 S16
// 写入 SPSC 环形缓冲,SDL 音频线程经 Audio 的后混音回调(MIX_CHANNEL_POST)
// 实时拉取。PC 与 Switch 共用这条路径,只有帧队列深度不同。
#include "video.h"
#include "audio.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
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

// 帧队列深度。队列持有解码器输出帧(YUV420P 720p 每帧约 1.4MB),
// 3 帧约 100ms 缓冲;引擎线程 sws_scale 后立即归还。
static constexpr int kVideoFrameQueueDepth = 3;

struct TimedFrame {
    AVFrame* frame = nullptr;
    double pts = 0.0;
};

class FrameQueue {
public:
    // closed 且空时返回 false。
    bool Pop(TimedFrame* out) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (items_.empty() && !closed_)
            notEmpty_.wait(lock);
        if (items_.empty()) return false;
        *out = items_.front();
        items_.pop_front();
        notFull_.notify_one();
        return true;
    }
    // 不出队地看队首;空返回 false。
    bool Peek(TimedFrame* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) return false;
        *out = items_.front();
        return true;
    }
    void PopFront() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) return;
        items_.pop_front();
        notFull_.notify_one();
    }
    // 返回 false 表示队列已关闭,生产者应退出并释放帧。
    bool Push(TimedFrame v) {
        std::unique_lock<std::mutex> lock(mutex_);
        while ((int)items_.size() >= depth_ && !closed_)
            notFull_.wait(lock);
        if (closed_) {
            av_frame_free(&v.frame);
            return false;
        }
        items_.push_back(v);
        notEmpty_.notify_one();
        return true;
    }
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }
    // Close 之后清空并释放剩余帧。
    void Flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (TimedFrame& tf : items_) {
            AVFrame* f = tf.frame;
            av_frame_free(&f);
        }
        items_.clear();
    }
    // 已关闭且取空:解码侧不会再有新帧。
    bool ClosedEmpty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && items_.empty();
    }

private:
    std::mutex mutex_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<TimedFrame> items_;
    int depth_ = kVideoFrameQueueDepth;
    bool closed_ = false;
};

static double FramePts(AVFormatContext* fmt, int stream, AVFrame* frame,
                       int64_t decodedFrames) {
    AVStream* st = fmt->streams[stream];
    int64_t pts = frame->best_effort_timestamp;
    if (pts != AV_NOPTS_VALUE) return pts * av_q2d(st->time_base);
    AVRational fps = av_guess_frame_rate(fmt, st, nullptr);
    const double rate = (fps.num && fps.den) ? av_q2d(fps) : 30.0;
    return (double)decodedFrames / std::max(1.0, rate);
}

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
    bool playing = false;
    double duration = 0.0;
    uint32_t startedMs = 0;
    int width = 0, height = 0;
    Audio* audio = nullptr;

#ifdef WA2_HAS_FFMPEG
    // ---- 解码线程状态(Stop 中 join 之后才可安全销毁) ----
    AVFormatContext* format = nullptr;
    AVCodecContext* videoCodec = nullptr;
    AVCodecContext* audioCodec = nullptr;
    SwrContext* audioSwr = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    std::thread decodeThread;
    FrameQueue frames;
    std::atomic<bool> quit_{false};
    SpscByteRing<kMovieAudioRingBytes>* audioRing = nullptr;
    std::atomic<int> audioVolume255_{255};
    std::atomic<bool> audioEof_{false};
    std::atomic<size_t> audioFedBytes_{0};
    int64_t decodedFrames = 0;
    // ---- 引擎线程状态 ----
    SwsContext* sws = nullptr;
    AVFrame* pending = nullptr;   // 正在展示的帧(从队列取出,展示后释放)
    double pendingPts = 0.0;
    std::vector<uint8_t> pixels;
    std::string playingPath;
    std::unique_ptr<MovieAudioBridge> movieBridge;
#endif
};

VideoPlayer::~VideoPlayer() { Shutdown(); }

#ifdef WA2_HAS_FFMPEG

// 解码线程主体:解复用+解码视频帧入队,音轨重采样写入环形缓冲。
// 由 Play() 在所有状态就绪、音频源注册完成后启动。
static void DecodeThread(VideoPlayer::Impl* p) {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> audioScratch;
    bool videoEof = false;
    bool audioEof = false;

    if (packet && frame) {
        while (!p->quit_.load(std::memory_order_relaxed)) {
            // 音频环形缓冲剩余空间不足一秒时,先等音频消费,避免无界
            // 等待——被 quit_ 打断后由外层统一清理。
            if (!audioEof && p->audioStream >= 0 &&
                p->audioRing->Free() < 48000 * 2 * 2) {
                SDL_Delay(4);
                continue;
            }
            if (videoEof && audioEof) break;

            int r = av_read_frame(p->format, packet);
            if (r < 0) {
                avcodec_send_packet(p->videoCodec, nullptr);
                if (p->audioCodec && p->audioStream >= 0)
                    avcodec_send_packet(p->audioCodec, nullptr);
                videoEof = audioEof = true;
            } else if (packet->stream_index == p->videoStream) {
                if (avcodec_send_packet(p->videoCodec, packet) >= 0) {
                    while (!p->quit_.load(std::memory_order_relaxed)) {
                        int got = avcodec_receive_frame(p->videoCodec, frame);
                        if (got == AVERROR(EAGAIN)) break;
                        if (got < 0) { videoEof = true; break; }
                        TimedFrame tf;
                        tf.pts = FramePts(p->format, p->videoStream, frame,
                                          p->decodedFrames++);
                        tf.frame = av_frame_clone(frame);
                        av_frame_unref(frame);
                        if (!tf.frame) continue;
                        if (!p->frames.Push(std::move(tf))) {
                            videoEof = true;   // 队列已关(Stop)
                            break;
                        }
                    }
                }
            } else if (packet->stream_index == p->audioStream) {
                if (avcodec_send_packet(p->audioCodec, packet) >= 0) {
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
                    }
                }
            }
            av_packet_unref(packet);
        }
    }

    // 解码侧标记音轨结束;引擎线程据 fed-drained 判断是否播完。
    p->audioEof_.store(true, std::memory_order_release);
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    p->frames.Close();   // 队列关闭后引擎线程 Pop 会在取空后返回 false
}

#endif // WA2_HAS_FFMPEG

bool VideoPlayer::Init(SDL_Renderer* renderer) {
    if (!p_) p_ = new Impl();
    p_->renderer = renderer;
    return renderer != nullptr;
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

static bool OpenMovie(VideoPlayer::Impl* p, const std::string& path) {
    if (avformat_open_input(&p->format, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(p->format, nullptr) < 0) {
        Log(LogLevel::Warn, "video: cannot open %s", path.c_str());
        return false;
    }
    p->videoStream = av_find_best_stream(p->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    p->videoCodec = OpenDecoder(p->format, p->videoStream);
    if (p->videoStream < 0 || !p->videoCodec) {
        Log(LogLevel::Warn, "video: no supported video stream in %s", path.c_str());
        return false;
    }
    p->width = p->videoCodec->width;
    p->height = p->videoCodec->height;
    p->duration = p->format->duration > 0
        ? p->format->duration / (double)AV_TIME_BASE : 0.0;
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
    if (p->format) avformat_close_input(&p->format);
    p->videoStream = p->audioStream = -1;
    p->decodedFrames = 0;
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

    p_->pixels.resize((size_t)p_->width * p_->height * 4);
    p_->sws = sws_getContext(p_->width, p_->height, p_->videoCodec->pix_fmt,
                             p_->width, p_->height, AV_PIX_FMT_BGRA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    p_->texture = SDL_CreateTexture(p_->renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, p_->width, p_->height);
    if (!p_->sws || !p_->texture) {
        Log(LogLevel::Warn, "video: texture/scale setup failed for %s", path.c_str());
        CloseMovieCodecs(p_);
        return false;
    }

    if (!p_->audioRing) p_->audioRing = new SpscByteRing<kMovieAudioRingBytes>();
    p_->audioVolume255_.store(std::clamp(volume255, 0, 255), std::memory_order_relaxed);
    p_->pending = nullptr;
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
    p_->decodeThread = std::thread(DecodeThread, p_);
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
    TimedFrame tf;
    while (p_->frames.Peek(&tf) && tf.pts <= elapsed + 0.010) {
        if (p_->pending) av_frame_free(&p_->pending);
        p_->frames.Pop(&tf);
        p_->pending = tf.frame;
        p_->pendingPts = tf.pts;
        uint8_t* dst[4] = {p_->pixels.data(), nullptr, nullptr, nullptr};
        int stride[4] = {p_->width * 4, 0, 0, 0};
        sws_scale(p_->sws, p_->pending->data, p_->pending->linesize, 0, p_->height,
                  dst, stride);
        SDL_UpdateTexture(p_->texture, nullptr, p_->pixels.data(), p_->width * 4);
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
    if (!p_ || !p_->playing || !p_->texture) return;
    const int outW = 1280, outH = 720;
    const float scale = std::min(outW / (float)p_->width, outH / (float)p_->height);
    SDL_Rect dst{(outW - (int)(p_->width * scale)) / 2,
                 (outH - (int)(p_->height * scale)) / 2,
                 (int)(p_->width * scale), (int)(p_->height * scale)};
    SDL_RenderCopy(p_->renderer, p_->texture, nullptr, &dst);
}

void VideoPlayer::Stop() {
    if (!p_) return;
#ifdef WA2_HAS_FFMPEG
    // 先把音频源从 Audio 撤下、让解码线程退出,再 join;避免回调期间
    // 环形缓冲被销毁。
    if (p_->audio && p_->playing)
        p_->audio->SetMovieAudioSource(nullptr);
    if (p_->playing) {
        p_->quit_.store(true, std::memory_order_relaxed);
        if (p_->decodeThread.joinable()) p_->decodeThread.join();
    }
    if (p_->pending) av_frame_free(&p_->pending);
    p_->frames.Flush();
    if (p_->texture) { SDL_DestroyTexture(p_->texture); p_->texture = nullptr; }
    sws_freeContext(p_->sws); p_->sws = nullptr;
    CloseMovieCodecs(p_);
    p_->pixels.clear();
    p_->pixels.shrink_to_fit();
    p_->playing = false;
    p_->duration = 0.0;
    p_->playingPath.clear();
#endif
}

void VideoPlayer::Shutdown() {
    if (!p_) return;
    Stop();
#ifdef WA2_HAS_FFMPEG
    p_->movieBridge.reset();
    delete p_->audioRing;
    p_->audioRing = nullptr;
#endif
    delete p_;
    p_ = nullptr;
}

bool VideoPlayer::Playing() const { return p_ && p_->playing; }
double VideoPlayer::Duration() const { return p_ ? p_->duration : 0.0; }

} // namespace wa2
