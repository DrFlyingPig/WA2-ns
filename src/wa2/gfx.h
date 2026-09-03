// gfx.h — SDL2 渲染器 + 字体文本(平台层)
#pragma once

#include "wa2.h"
#include "res.h"
#include "scene.h"

#include <unordered_set>

struct SDL_Renderer;
struct SDL_Surface;
struct SDL_Window;
struct SDL_Texture;
#ifdef __SWITCH__
struct Framebuffer;
#endif
#include <SDL2/SDL_gamecontroller.h>
#include <SDL2/SDL_ttf.h>   // 提供公开类型 TTF_Font(跨 SDL2 版本)

namespace wa2 {

struct Tex {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    size_t bytes = 0;              // 解码纹理的保守估算值，用于 Switch LRU 预算
    uint64_t lastUsedFrame = 0;    // 当前帧使用过的纹理禁止被同帧淘汰
};

// UTF-8 文本行:逐字显示用的字符切分
struct TextRun {
    std::vector<std::string> chars;   // 逐字符(UTF-8)
    std::string raw;
    static TextRun Split(const std::string& utf8);   // 按 \k 切段在引擎层做
};

class Gfx {
public:
    bool Init(const std::string& fontPath);   // 1280x720 虚拟分辨率窗口
    bool EnablePatchFont(Res& res);           // CK-GAL fon.pak 字形图集
    void Shutdown();

    // SDL 不会因为初始化 GAMECONTROLLER 子系统就自动打开设备。
    // 启动和热插拔时由引擎显式管理，保证 Switch Joy-Con/Pro 手柄产生 Controller 事件。
    bool OpenController(int deviceIndex);
    void CloseController(int instanceId);
    bool IsControllerInstance(int instanceId) const;

    // ---- 纹理 ----
    Tex* Get(const std::string& lowerName, Res& res, const std::string& effectMode);
    void ClearCache();
    void Release(const std::string& lowerName);
    Tex* LoadMask(const std::string& lowerName, Res& res);   // 灰度掩码(R 纹理)
    size_t CachedTextureBytes() const { return cacheBytes_; }
    size_t CachedTextureCount() const { return cache_.size(); }
    size_t TextureCacheBudget() const { return cacheBudgetBytes_; }

    // ---- 帧 ----
    void Clear();
    void Present();
    // 视频直通:播放期间把 BGRA 帧直接 swizzle 进 framebuffer,跳过
    // SDL 纹理更新 + softwareFrame blit 两层全屏拷贝。返回 false 表示
    // 当前环境不可直通(调用方回退常规路径)。调用后本帧不再需要 Present。
    bool PresentVideoFrameDirect(const uint8_t* bgra, int pitch);
    SDL_Texture* CaptureScreen();   // 屏幕快照(过渡起始画面);用完需 ReleaseSnapshot
    void ReleaseSnapshot(SDL_Texture* t);
    // ---- 掩码溶解过渡(CPU 域混合,Switch/PC 通用)----
    // 抓取当前帧像素(RGBA8888, 1280x720)。Switch 直接复制 softwareFrame_,
    // PC 走一次 RenderReadPixels(仅过渡开始时调用一次)。
    bool CaptureFramePixels(std::vector<uint8_t>& out);
    // 按 mask+progress 混合:dst[i] = old*(1-a) + new*a,
    // a = clamp((threshold - mask)*8, 0..255)/255,threshold = progress*255。
    // frame/old 均为 RGBA8888 1280x720;mask 为 8-bit 灰度(mw×mh)。
    static void BlendMaskPixels(uint8_t* frame, const uint8_t* oldPix,
                                const uint8_t* mask, int maskW, int maskH,
                                float progress);
    // 把混合结果送回渲染管线:Switch 写回 softwareFrame_(Present 前),
    // PC 上传临时纹理并 RenderCopy。
    void PresentBlendedFrame(const std::vector<uint8_t>& blended);
    bool SaveScreenshot(const std::string& path);
    void DrawTexture(Tex* t, int x, int y, int w, int h, float alpha);
    // 绘制图集中的一个矩形区域。原版标题/系统 UI 都保存在 T0100、sys_* 图集中。
    void DrawTextureRegion(Tex* t, int srcX, int srcY, int srcW, int srcH,
                           int x, int y, int w, int h, float alpha);
    void DrawTextureFit(Tex* t, float cx, float cy, float sx, float sy, float alpha);
    void FillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // ---- 文本(UTF-8,内嵌 TTF)----
    int  TextWidth(const std::string& utf8, int size);
    // 绘制一行;maxW>0 时返回被截断的字符数之前的位置(-1 表示全部)
    void DrawText(const std::string& utf8, int x, int y, int size,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    // 逐字绘制(打字机):maxChars=显示的字符数;返回实际绘制的字符数
    int  DrawTextTyped(const std::string& utf8, int x, int y, int size, int maxChars,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    int  LineHeight(int size) const;

    SDL_Renderer* renderer() { return renderer_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
#ifdef __SWITCH__
    // 真机使用 libnx 原生 framebuffer 显示；renderer_ 是只在内存里
    // 合成 1280x720 画面的 SDL software renderer，不再经过 Mesa。
    SDL_Surface* softwareFrame_ = nullptr;
    Framebuffer* nativeFramebuffer_ = nullptr;
#endif
    TTF_Font* font_ = nullptr;   // SDL_ttf 公开类型
    std::map<std::string, Tex> cache_;   // std::map:不重哈希,Get 返回的指针在插入/删除后仍稳定(否则悬垂 UAF)
    std::unordered_set<std::string> missing_;   // 解码失败的资源:本会话不再每帧重试
    size_t cacheBytes_ = 0;
    size_t cacheBudgetBytes_ = 0;
    size_t textureLoadCount_ = 0;
    uint64_t frameSerial_ = 1;
    Tex snapshotTex_;
    SDL_Texture* snapshot_ = nullptr;
    // PC 掩码混合结果的临时纹理(过渡期间复用,过渡结束释放)。
    SDL_Texture* blendTex_ = nullptr;
    SDL_Texture* patchFontBody_ = nullptr;
    SDL_Texture* patchFontShadow_ = nullptr;
    std::vector<SDL_GameController*> controllers_;
    std::vector<SDL_Joystick*> joysticks_;

    size_t EvictUnused(size_t incomingBytes, bool allUnused);
};

// 应用调色板 LUT(EffectMode):data 为 256/768/1280 字节调色板文件
void ApplyPaletteLUT(std::vector<uint8_t>& rgba, const std::vector<uint8_t>& lut);

} // namespace wa2
