// video.h — PC/Switch 共用影片接口；FFmpeg 直接读取零售版 ASF(.pak)
#pragma once

#include <string>

struct SDL_Renderer;

namespace wa2 {

class Audio;

class VideoPlayer {
public:
    struct Impl;
    // audio 仅用于把影片音轨接入 Audio 的后混音回调;传空则静音播放。
    VideoPlayer() = default;
    ~VideoPlayer();

    void BindAudio(Audio* audio) { audio_ = audio; }
    bool Init(SDL_Renderer* renderer);
    void Shutdown();
    bool Play(const std::string& path, int volume255);
    void Stop();
    void Update();
    void Render();
    bool Playing() const;
    double Duration() const;

private:
    Impl* p_ = nullptr;
    Audio* audio_ = nullptr;
};

} // namespace wa2
