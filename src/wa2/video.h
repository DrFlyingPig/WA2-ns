// video.h — PC/Switch 共用影片接口；FFmpeg 直接读取零售版 ASF(.pak)
#pragma once

#include <string>

struct SDL_Renderer;

namespace wa2 {

class Audio;
class Gfx;

class VideoPlayer {
public:
    struct Impl;
    // audio 仅用于把影片音轨接入 Audio 的后混音回调;传空则静音播放。
    VideoPlayer() = default;
    ~VideoPlayer();

    void BindAudio(Audio* audio);
    void BindGfx(Gfx* gfx);   // 视频帧直接写 Gfx 的 softwareFrame(安全直通)
    bool Init(SDL_Renderer* renderer);
    void Shutdown();
    bool Play(const std::string& path, int volume255);
    void Stop();
    void Update();
    void Render();
    // 在引擎 Render() 之后、Present() 之前调用:把最近缓存的视频帧直写
    // Gfx 的 softwareFrame(省去 UpdateTexture+RenderCopy 两层全屏拷贝)。
    void PresentVideoFrame();
    bool Playing() const;
    double Duration() const;

private:
    Impl* p_ = nullptr;
    Audio* audio_ = nullptr;
};

} // namespace wa2
