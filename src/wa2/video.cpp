// video.cpp — FFmpeg ASF/WMV3/WMA2 解码 + SDL2 纹理/混音输出
#include "video.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <algorithm>
#include <cmath>
#include <vector>

#ifdef WA2_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace wa2 {

struct VideoPlayer::Impl {
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    bool playing = false;
    double duration = 0.0;
    uint32_t startedMs = 0;
    int width = 0, height = 0;

#ifdef WA2_HAS_FFMPEG
    AVFormatContext* format = nullptr;
    AVCodecContext* videoCodec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int videoStream = -1;
    bool sentEof = false;
    bool decoderEof = false;
    bool pendingFrame = false;
    double pendingPts = 0.0;
    int64_t decodedFrames = 0;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> audioPcm;
    Mix_Chunk* audioChunk = nullptr;
#endif
};

VideoPlayer::~VideoPlayer() { Shutdown(); }

bool VideoPlayer::Init(SDL_Renderer* renderer) {
    if (!p_) p_ = new Impl();
    p_->renderer = renderer;
    return renderer != nullptr;
}

#ifdef WA2_HAS_FFMPEG

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

static bool AppendAudioFrame(SwrContext* swr, AVCodecContext* codec, AVFrame* frame,
                             std::vector<uint8_t>* pcm) {
    const int outRate = 48000;
    const int outChannels = 2;
    int outSamples = (int)av_rescale_rnd(
        swr_get_delay(swr, codec->sample_rate) + frame->nb_samples,
        outRate, codec->sample_rate, AV_ROUND_UP);
    if (outSamples <= 0) return true;
    const size_t old = pcm->size();
    pcm->resize(old + (size_t)outSamples * outChannels * sizeof(int16_t));
    uint8_t* dst[1] = {pcm->data() + old};
    int made = swr_convert(swr, dst, outSamples,
                           (const uint8_t**)frame->extended_data, frame->nb_samples);
    if (made < 0) {
        pcm->resize(old);
        return false;
    }
    pcm->resize(old + (size_t)made * outChannels * sizeof(int16_t));
    return true;
}

static bool DecodeAllAudio(const std::string& path, std::vector<uint8_t>* pcm) {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    AVChannelLayout stereo{};
    bool ok = false;

    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(fmt, nullptr) < 0) goto done;
    {
        int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (stream < 0) goto done;
        codec = OpenDecoder(fmt, stream);
        if (!codec || codec->sample_rate <= 0) goto done;
        av_channel_layout_default(&stereo, 2);
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000,
                                &codec->ch_layout, codec->sample_fmt, codec->sample_rate,
                                0, nullptr) < 0 || !swr || swr_init(swr) < 0) goto done;
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!packet || !frame) goto done;

        while (av_read_frame(fmt, packet) >= 0) {
            if (packet->stream_index == stream && avcodec_send_packet(codec, packet) >= 0) {
                while (avcodec_receive_frame(codec, frame) >= 0) {
                    if (!AppendAudioFrame(swr, codec, frame, pcm)) goto done;
                    av_frame_unref(frame);
                }
            }
            av_packet_unref(packet);
        }
        avcodec_send_packet(codec, nullptr);
        while (avcodec_receive_frame(codec, frame) >= 0) {
            if (!AppendAudioFrame(swr, codec, frame, pcm)) goto done;
            av_frame_unref(frame);
        }
        ok = !pcm->empty();
    }

done:
    av_channel_layout_uninit(&stereo);
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    if (fmt) avformat_close_input(&fmt);
    return ok;
}

static bool DecodeNextVideoFrame(VideoPlayer::Impl* p) {
    while (!p->decoderEof) {
        int got = avcodec_receive_frame(p->videoCodec, p->frame);
        if (got == 0) {
            int64_t pts = p->frame->best_effort_timestamp;
            AVRational tb = p->format->streams[p->videoStream]->time_base;
            if (pts != AV_NOPTS_VALUE) p->pendingPts = pts * av_q2d(tb);
            else {
                AVRational fps = av_guess_frame_rate(p->format,
                                                     p->format->streams[p->videoStream], nullptr);
                double rate = fps.num && fps.den ? av_q2d(fps) : 30.0;
                p->pendingPts = p->decodedFrames / std::max(1.0, rate);
            }
            ++p->decodedFrames;
            p->pendingFrame = true;
            return true;
        }
        if (got == AVERROR_EOF) {
            p->decoderEof = true;
            break;
        }
        if (got != AVERROR(EAGAIN)) {
            p->decoderEof = true;
            break;
        }

        if (p->sentEof) {
            p->decoderEof = true;
            break;
        }
        bool sent = false;
        while (!sent) {
            int r = av_read_frame(p->format, p->packet);
            if (r < 0) {
                avcodec_send_packet(p->videoCodec, nullptr);
                p->sentEof = true;
                sent = true;
            } else {
                if (p->packet->stream_index == p->videoStream) {
                    int sr = avcodec_send_packet(p->videoCodec, p->packet);
                    sent = sr >= 0 || sr == AVERROR(EAGAIN);
                }
                av_packet_unref(p->packet);
            }
        }
    }
    return false;
}

static void UploadPendingFrame(VideoPlayer::Impl* p) {
    uint8_t* dst[4] = {p->pixels.data(), nullptr, nullptr, nullptr};
    int stride[4] = {p->width * 4, 0, 0, 0};
    sws_scale(p->sws, p->frame->data, p->frame->linesize, 0, p->height, dst, stride);
    SDL_UpdateTexture(p->texture, nullptr, p->pixels.data(), p->width * 4);
    p->pendingFrame = false;
    av_frame_unref(p->frame);
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
    p_->format = nullptr;
    if (avformat_open_input(&p_->format, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(p_->format, nullptr) < 0) {
        Log(LogLevel::Warn, "video: cannot open %s", path.c_str());
        Stop();
        return false;
    }
    p_->videoStream = av_find_best_stream(p_->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    p_->videoCodec = OpenDecoder(p_->format, p_->videoStream);
    if (p_->videoStream < 0 || !p_->videoCodec) {
        Log(LogLevel::Warn, "video: no supported video stream in %s", path.c_str());
        Stop();
        return false;
    }
    p_->width = p_->videoCodec->width;
    p_->height = p_->videoCodec->height;
    p_->duration = p_->format->duration > 0
        ? p_->format->duration / (double)AV_TIME_BASE : 0.0;
    p_->frame = av_frame_alloc();
    p_->packet = av_packet_alloc();
    p_->sws = sws_getContext(p_->width, p_->height, p_->videoCodec->pix_fmt,
                            p_->width, p_->height, AV_PIX_FMT_BGRA,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
    p_->texture = SDL_CreateTexture(p_->renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, p_->width, p_->height);
    p_->pixels.resize((size_t)p_->width * p_->height * 4);
    if (!p_->frame || !p_->packet || !p_->sws || !p_->texture) {
        Log(LogLevel::Warn, "video: decoder/texture setup failed for %s", path.c_str());
        Stop();
        return false;
    }

    // 影片音轨不经过游戏归档层，直接从同一 ASF 文件解码为 Mixer 格式。
    if (DecodeAllAudio(path, &p_->audioPcm)) {
        p_->audioChunk = Mix_QuickLoad_RAW(p_->audioPcm.data(), (Uint32)p_->audioPcm.size());
        if (p_->audioChunk) {
            Mix_VolumeChunk(p_->audioChunk,
                            std::clamp(volume255, 0, 255) * MIX_MAX_VOLUME / 255);
            Mix_PlayChannel(31, p_->audioChunk, 0);
        }
    } else {
        Log(LogLevel::Warn, "video: audio decode failed for %s", path.c_str());
    }

    p_->sentEof = p_->decoderEof = p_->pendingFrame = false;
    p_->decodedFrames = 0;
    p_->startedMs = SDL_GetTicks();
    p_->playing = true;
    Log(LogLevel::Info, "video: playing %s (%dx%d, %.3fs, pcm=%u)", path.c_str(),
        p_->width, p_->height, p_->duration, (unsigned)p_->audioPcm.size());
    return true;
#endif
}

void VideoPlayer::Update() {
    if (!p_ || !p_->playing) return;
#ifdef WA2_HAS_FFMPEG
    const double elapsed = (SDL_GetTicks() - p_->startedMs) / 1000.0;
    if (!p_->pendingFrame) DecodeNextVideoFrame(p_);
    // 丢弃已经落后的帧，保留最接近当前时钟的一帧。
    while (p_->pendingFrame && p_->pendingPts <= elapsed + 0.010) {
        UploadPendingFrame(p_);
        if (!DecodeNextVideoFrame(p_)) break;
    }
    if (p_->decoderEof && !p_->pendingFrame &&
        (p_->duration <= 0.0 || elapsed >= p_->duration - 0.03)) Stop();
#endif
}

void VideoPlayer::Render() {
    if (!p_ || !p_->playing || !p_->texture) return;
    int outW = 1280, outH = 720;
    const float scale = std::min(outW / (float)p_->width, outH / (float)p_->height);
    SDL_Rect dst{(outW - (int)(p_->width * scale)) / 2,
                 (outH - (int)(p_->height * scale)) / 2,
                 (int)(p_->width * scale), (int)(p_->height * scale)};
    SDL_RenderCopy(p_->renderer, p_->texture, nullptr, &dst);
}

void VideoPlayer::Stop() {
    if (!p_) return;
#ifdef WA2_HAS_FFMPEG
    Mix_HaltChannel(31);
    if (p_->audioChunk) { Mix_FreeChunk(p_->audioChunk); p_->audioChunk = nullptr; }
    p_->audioPcm.clear();
    if (p_->texture) { SDL_DestroyTexture(p_->texture); p_->texture = nullptr; }
    sws_freeContext(p_->sws); p_->sws = nullptr;
    av_frame_free(&p_->frame);
    av_packet_free(&p_->packet);
    avcodec_free_context(&p_->videoCodec);
    if (p_->format) avformat_close_input(&p_->format);
    p_->videoStream = -1;
    p_->sentEof = p_->decoderEof = p_->pendingFrame = false;
    p_->pixels.clear();
#endif
    p_->playing = false;
    p_->duration = 0.0;
}

void VideoPlayer::Shutdown() {
    if (!p_) return;
    Stop();
    delete p_;
    p_ = nullptr;
}

bool VideoPlayer::Playing() const { return p_ && p_->playing; }
double VideoPlayer::Duration() const { return p_ ? p_->duration : 0.0; }

} // namespace wa2
