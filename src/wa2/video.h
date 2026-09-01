// video.h — PC/Switch 共用影片接口；FFmpeg 直接读取零售版 ASF(.pak)
#pragma once

#include <string>

struct SDL_Renderer;

namespace wa2 {

class VideoPlayer {
public:
    struct Impl;
    VideoPlayer() = default;
    ~VideoPlayer();

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
};

} // namespace wa2
