// gfx.cpp — SDL2 渲染器实现(纹理缓存 / 过渡 / 字形缓存文本)
#include "gfx.h"
#include "util.h"
#include "sjis.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>
#endif

namespace wa2 {

static void LogHeapStats(const char* stage) {
#ifdef __SWITCH__
    const struct mallinfo mi = mallinfo();
    u64 heapSize = 0;
    if (R_FAILED(svcGetInfo(&heapSize, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0)))
        heapSize = 0;
    Log(LogLevel::Info,
        "mem: %s heap-used=%.1f MiB allocator-free=%.1f MiB heap-region=%.1f MiB",
        stage,
        (double)mi.uordblks / (1024.0 * 1024.0),
        (double)mi.fordblks / (1024.0 * 1024.0),
        (double)heapSize / (1024.0 * 1024.0));
#else
    (void)stage;
#endif
}

std::string g_fontPath;   // 外部字体路径(定义放最前,GetFont 需在此处可见)

// ---------------- UTF-8 切分 ----------------
static size_t Utf8Next(const std::string& s, size_t i) {
    if (i >= s.size()) return i;
    uint8_t c = (uint8_t)s[i];
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return std::min(i + n, s.size());
}
static uint32_t Utf8Decode(const std::string& s, size_t i, size_t* next) {
    size_t j = Utf8Next(s, i);
    *next = j;
    uint32_t cp = 0;
    if (j - i == 1) cp = (uint8_t)s[i];
    else if (j - i == 2) cp = ((s[i] & 0x1F) << 6) | (s[i + 1] & 0x3F);
    else if (j - i == 3) cp = ((s[i] & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
    else cp = ((s[i] & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
    return cp;
}

// ---------------- 调色板 LUT ----------------
static void ApplyPaletteLUTBytes(uint8_t* rgba, size_t size,
                                 const std::vector<uint8_t>& lut) {
    if (lut.size() != 256 && lut.size() != 768 && lut.size() != 1280) return;
    for (size_t i = 0; i + 3 < size; i += 4) {
        int gray = (77 * rgba[i] + 151 * rgba[i + 1] + 28 * rgba[i + 2]) >> 8;
        if (lut.size() == 256) {
            rgba[i] = rgba[i + 1] = rgba[i + 2] = lut[gray];
        } else if (lut.size() == 768) {
            rgba[i] = lut[gray];
            rgba[i + 1] = lut[256 + gray];
            rgba[i + 2] = lut[512 + gray];
        } else { // 1280
            rgba[i] = lut[256 + gray];
            rgba[i + 1] = lut[512 + gray];
            rgba[i + 2] = lut[768 + gray];
        }
    }
}

// ---------------- 字体 ----------------
struct Glyph {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};

static std::map<int, TTF_Font*> g_fonts;
static std::map<int, std::map<uint32_t, Glyph>> g_glyphs;

static TTF_Font* GetFont(int size) {
    auto it = g_fonts.find(size);
    if (it != g_fonts.end()) return it->second;
    // 以 48px 打开,由 TTF_SetFontSize 缩放不可靠,直接按尺寸各开一份
    TTF_Font* f = TTF_OpenFont(g_fontPath.c_str(), size);
    if (!f) {
        Log(LogLevel::Error, "font: open size %d failed: %s", size, TTF_GetError());
        return nullptr;
    }
    g_fonts[size] = f;
    return f;
}

// ---------------- Gfx ----------------
bool Gfx::Init(const std::string& fontPath) {
    g_fontPath = fontPath;

#ifdef __SWITCH__
    // HBMenu 小程序模式的可用内存远小于 title takeover。纹理只保留当前
    // 场景的工作集，给 TGA 解码、软件 framebuffer 和音频留下明确余量。
    cacheBudgetBytes_ = 8u * 1024u * 1024u;
#else
    cacheBudgetBytes_ = 256u * 1024u * 1024u;
#endif
    if (const char* mb = SDL_getenv("WA2_TEXTURE_CACHE_MB")) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(mb, &end, 10);
        if (end != mb && value >= 4 && value <= 1024)
            cacheBudgetBytes_ = (size_t)value * 1024u * 1024u;
    }

#ifdef __SWITCH__
    // 图形显示直接交给 libnx framebuffer；不要初始化 SDL Switch 视频
    // 后端，否则 SDL_CreateWindow 会建立 EGL/Mesa/Nouveau 上下文。
    const Uint32 initFlags = SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER;
#else
    const Uint32 initFlags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER;
#endif
    if (SDL_Init(initFlags) != 0) {
        Log(LogLevel::Error, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (TTF_Init() != 0) {
        Log(LogLevel::Error, "TTF_Init failed: %s", TTF_GetError());
        return false;
    }
#ifdef __SWITCH__
    // Atmosphère 的重复报告都落在 Mesa/Nouveau shader compiler。这里用
    // SDL software renderer 合成到普通 RGBA Surface，再由 libnx 官方
    // Framebuffer API 双缓冲送屏，运行时不再加载任何 OpenGL shader。
    softwareFrame_ = SDL_CreateRGBSurfaceWithFormat(
        0, kVirtualW, kVirtualH, 32, SDL_PIXELFORMAT_RGBA32);
    if (!softwareFrame_) {
        Log(LogLevel::Error, "Create software framebuffer failed: %s", SDL_GetError());
        return false;
    }
    renderer_ = SDL_CreateSoftwareRenderer(softwareFrame_);
    if (!renderer_) {
        Log(LogLevel::Error, "Create software renderer failed: %s", SDL_GetError());
        return false;
    }
    nativeFramebuffer_ = (Framebuffer*)std::calloc(1, sizeof(Framebuffer));
    if (!nativeFramebuffer_) {
        Log(LogLevel::Error, "Allocate native framebuffer state failed");
        return false;
    }
    Result fbRc = framebufferCreate(nativeFramebuffer_, nwindowGetDefault(),
                                    kVirtualW, kVirtualH,
                                    PIXEL_FORMAT_RGBA_8888, 2);
    if (R_SUCCEEDED(fbRc)) fbRc = framebufferMakeLinear(nativeFramebuffer_);
    if (R_FAILED(fbRc)) {
        Log(LogLevel::Error, "Create libnx framebuffer failed: 0x%08x", (unsigned)fbRc);
        if (nativeFramebuffer_->has_init) framebufferClose(nativeFramebuffer_);
        std::free(nativeFramebuffer_);
        nativeFramebuffer_ = nullptr;
        return false;
    }
    Log(LogLevel::Info,
        "gfx: backend=software display=libnx-framebuffer buffers=2 video=none");
#else
    const int flags = SDL_WINDOW_SHOWN;
    window_ = SDL_CreateWindow("WA2-ns", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               kVirtualW, kVirtualH, flags);
    if (!window_) { Log(LogLevel::Error, "CreateWindow failed: %s", SDL_GetError()); return false; }
    renderer_ = SDL_CreateRenderer(window_, -1,
                                   SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        Log(LogLevel::Warn, "CreateRenderer(accel|vsync) failed, falling back: %s", SDL_GetError());
        renderer_ = SDL_CreateRenderer(window_, -1, 0);
    }
    if (!renderer_) { Log(LogLevel::Error, "CreateRenderer failed: %s", SDL_GetError()); return false; }
    // 记录实际后端与能力(真机上用 wa2.log 判断是否走了 GPU/GLES)
    SDL_RendererInfo ri; SDL_zero(ri);
    if (SDL_GetRendererInfo(renderer_, &ri) == 0) {
        Log(LogLevel::Info, "gfx: backend=%s accelerated=%d vsync=%d video=%s",
            (ri.name && *ri.name) ? ri.name : "(?)",
            (ri.flags & SDL_RENDERER_ACCELERATED) ? 1 : 0,
            (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? 1 : 0,
            SDL_GetCurrentVideoDriver());
    }
#endif
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    SDL_GameControllerEventState(SDL_ENABLE);
    SDL_JoystickEventState(SDL_ENABLE);
    const int joystickCount = SDL_NumJoysticks();
    Log(LogLevel::Info, "input: SDL reports %d joystick(s)", joystickCount);
    for (int i = 0; i < joystickCount; ++i) OpenController(i);

    if (!GetFont(32)) return false;   // 字体必须可用
#ifdef __SWITCH__
    Log(LogLevel::Info, "gfx: initialized (libnx-software), texture cache budget=%zu MiB",
        cacheBudgetBytes_ / (1024u * 1024u));
#else
    Log(LogLevel::Info, "gfx: initialized (%s), texture cache budget=%zu MiB",
        SDL_GetCurrentVideoDriver(), cacheBudgetBytes_ / (1024u * 1024u));
#endif
    LogHeapStats("after gfx init");
    return true;
}

bool Gfx::OpenController(int deviceIndex) {
    if (deviceIndex < 0 || deviceIndex >= SDL_NumJoysticks()) return false;

    const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    if (instance >= 0 && SDL_GameControllerFromInstanceID(instance)) return true;

    for (SDL_Joystick* joystick : joysticks_) {
        if (SDL_JoystickInstanceID(joystick) == instance) return true;
    }

    if (!SDL_IsGameController(deviceIndex)) {
        // 某些 Switch SDL2 打包没有内置 GameController mapping，仍要打开
        // 原始 Joystick；Engine 内有 libnx 按钮序号的兼容映射。
        SDL_Joystick* joystick = SDL_JoystickOpen(deviceIndex);
        if (!joystick) {
            Log(LogLevel::Error, "input: opening raw joystick %d failed: %s",
                deviceIndex, SDL_GetError());
            return false;
        }
        joysticks_.push_back(joystick);
        Log(LogLevel::Warn, "input: opened raw joystick device=%d instance=%d name=%s",
            deviceIndex, (int)SDL_JoystickInstanceID(joystick),
            SDL_JoystickName(joystick) ? SDL_JoystickName(joystick) : "(?)");
        return true;
    }

    SDL_GameController* controller = SDL_GameControllerOpen(deviceIndex);
    if (!controller) {
        Log(LogLevel::Error, "input: opening controller %d failed: %s",
            deviceIndex, SDL_GetError());
        return false;
    }

    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    const SDL_JoystickID openedInstance = SDL_JoystickInstanceID(joystick);
    controllers_.push_back(controller);
    Log(LogLevel::Info, "input: opened controller device=%d instance=%d name=%s",
        deviceIndex, (int)openedInstance,
        SDL_GameControllerName(controller) ? SDL_GameControllerName(controller) : "(?)");
    return true;
}

void Gfx::CloseController(int instanceId) {
    for (auto it = controllers_.begin(); it != controllers_.end(); ++it) {
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(*it);
        if (SDL_JoystickInstanceID(joystick) != (SDL_JoystickID)instanceId) continue;
        Log(LogLevel::Info, "input: closed controller instance=%d", instanceId);
        SDL_GameControllerClose(*it);
        controllers_.erase(it);
        return;
    }
    for (auto it = joysticks_.begin(); it != joysticks_.end(); ++it) {
        if (SDL_JoystickInstanceID(*it) != (SDL_JoystickID)instanceId) continue;
        Log(LogLevel::Info, "input: closed raw joystick instance=%d", instanceId);
        SDL_JoystickClose(*it);
        joysticks_.erase(it);
        return;
    }
}

void ApplyPaletteLUT(std::vector<uint8_t>& rgba, const std::vector<uint8_t>& lut) {
    ApplyPaletteLUTBytes(rgba.data(), rgba.size(), lut);
}

bool Gfx::IsControllerInstance(int instanceId) const {
    for (SDL_GameController* controller : controllers_) {
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
        if (SDL_JoystickInstanceID(joystick) == (SDL_JoystickID)instanceId) return true;
    }
    return false;
}

// CK-GAL 的原图集是 80x96 个 40px 单元，整张 RGBA 纹理需要约 47 MiB。
// 实际只绘制每格中央的 28px 本体（阴影为 32px），初始化时无损紧密重排，
// 可把常驻显存降到约一半；像素不缩放、不降色深，字形清晰度保持不变。
static SDL_Texture* LoadPackedPatchAtlas(SDL_Renderer* renderer, Res& res,
                                         const std::string& rawName,
                                         int cropX, int cropY, int cellW, int cellH,
                                         int* outSourceW, int* outSourceH,
                                         int* outPackedW, int* outPackedH) {
    std::vector<uint8_t> data = res.Load(rawName);
    if (data.empty()) return nullptr;
    SDL_RWops* rw = SDL_RWFromMem(data.data(), (int)data.size());
    SDL_Surface* decoded = IMG_LoadTyped_RW(rw, 1, "TGA");
    if (!decoded) return nullptr;
    if (outSourceW) *outSourceW = decoded->w;
    if (outSourceH) *outSourceH = decoded->h;

    if (decoded->w <= cropX || decoded->h <= cropY ||
        decoded->w % 40 != 0 || decoded->h % 40 != 0) {
        SDL_FreeSurface(decoded);
        SDL_SetError("invalid patch-font atlas grid");
        return nullptr;
    }

    SDL_Surface* surf = SDL_ConvertSurfaceFormat(decoded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(decoded);
    if (!surf) return nullptr;

    const int columns = surf->w / 40;
    const int rows = surf->h / 40;
    const int packedW = columns * cellW;
    const int packedH = rows * cellH;
    if (outPackedW) *outPackedW = packedW;
    if (outPackedH) *outPackedH = packedH;

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, packedW, packedH);
    if (!tex) {
        SDL_FreeSurface(surf);
        return nullptr;
    }

    const size_t rowPitch = (size_t)packedW * 4u;
    std::vector<uint8_t> packedRow(rowPitch * (size_t)cellH);
    if (SDL_MUSTLOCK(surf) && SDL_LockSurface(surf) != 0) {
        SDL_DestroyTexture(tex);
        SDL_FreeSurface(surf);
        return nullptr;
    }
    bool uploadOk = true;
    for (int gy = 0; gy < rows && uploadOk; ++gy) {
        for (int y = 0; y < cellH; ++y) {
            uint8_t* dst = packedRow.data() + rowPitch * (size_t)y;
            for (int gx = 0; gx < columns; ++gx) {
                const uint8_t* src = (const uint8_t*)surf->pixels +
                    (size_t)(gy * 40 + cropY + y) * (size_t)surf->pitch +
                    (size_t)(gx * 40 + cropX) * 4u;
                std::memcpy(dst + (size_t)gx * (size_t)cellW * 4u,
                            src, (size_t)cellW * 4u);
            }
        }
        SDL_Rect dst{0, gy * cellH, packedW, cellH};
        uploadOk = SDL_UpdateTexture(tex, &dst, packedRow.data(), (int)rowPitch) == 0;
    }
    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
    SDL_FreeSurface(surf);
    if (!uploadOk) {
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

bool Gfx::EnablePatchFont(Res& res) {
    if (patchFontBody_) SDL_DestroyTexture(patchFontBody_);
    if (patchFontShadow_) SDL_DestroyTexture(patchFontShadow_);
    patchFontBody_ = patchFontShadow_ = nullptr;

    // 归档名本身是 CP932；Archive 索引保留原始名字字节。
    const std::string body = std::string("\x96\x7b\x91\xcc" "80.tga");   // 本体80.tga
    const std::string shadow = std::string("\x91\xdc\x89\x65" "80.tga"); // 袋影80.tga
    int w = 0, h = 0, packedW = 0, packedH = 0;
    patchFontBody_ = LoadPackedPatchAtlas(renderer_, res, body, 4, 4, 28, 28,
                                          &w, &h, &packedW, &packedH);
#ifdef __SWITCH__
    // 阴影图集紧密重排后仍需约 30 MiB；正文已有清晰边缘，Switch 上不为
    // 这层装饰牺牲场景纹理余量（也避免字体看起来过粗）。
    patchFontShadow_ = nullptr;
#else
    patchFontShadow_ = LoadPackedPatchAtlas(renderer_, res, shadow, 2, 2, 32, 32,
                                            nullptr, nullptr, nullptr, nullptr);
#endif
    if (!patchFontBody_ || w != 3200 || h != 3840) {
        Log(LogLevel::Error, "gfx: Chinese font atlas missing/invalid (%dx%d): %s",
            w, h, IMG_GetError());
        if (patchFontBody_) { SDL_DestroyTexture(patchFontBody_); patchFontBody_ = nullptr; }
        if (patchFontShadow_) { SDL_DestroyTexture(patchFontShadow_); patchFontShadow_ = nullptr; }
        return false;
    }
    Log(LogLevel::Info,
        "gfx: Chinese patch font enabled (source=%dx%d packed=%dx%d %.1f MiB, shadow=%d)",
        w, h, packedW, packedH,
        (double)((size_t)packedW * (size_t)packedH * 4u) / (1024.0 * 1024.0),
        patchFontShadow_ ? 1 : 0);
    LogHeapStats("after patch font");
    return true;
}

void Gfx::Shutdown() {
    ClearCache();
    for (auto& [size, f] : g_fonts) TTF_CloseFont(f);
    g_fonts.clear();
    for (auto& [size, m] : g_glyphs)
        for (auto& [cp, g] : m) if (g.tex) SDL_DestroyTexture(g.tex);
    g_glyphs.clear();
    if (patchFontBody_) { SDL_DestroyTexture(patchFontBody_); patchFontBody_ = nullptr; }
    if (patchFontShadow_) { SDL_DestroyTexture(patchFontShadow_); patchFontShadow_ = nullptr; }
    if (snapshot_) { SDL_DestroyTexture(snapshot_); snapshot_ = nullptr; }
    for (SDL_GameController* controller : controllers_) SDL_GameControllerClose(controller);
    controllers_.clear();
    for (SDL_Joystick* joystick : joysticks_) SDL_JoystickClose(joystick);
    joysticks_.clear();
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
#ifdef __SWITCH__
    if (softwareFrame_) { SDL_FreeSurface(softwareFrame_); softwareFrame_ = nullptr; }
    if (nativeFramebuffer_) {
        if (nativeFramebuffer_->has_init) framebufferClose(nativeFramebuffer_);
        std::free(nativeFramebuffer_);
        nativeFramebuffer_ = nullptr;
    }
#endif
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    TTF_Quit();
    SDL_Quit();
}

static bool IsOutOfMemoryError(const char* error) {
    if (!error) return false;
    std::string lower(error);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return lower.find("out of memory") != std::string::npos ||
           lower.find("not enough memory") != std::string::npos;
}

static size_t EstimateTextureBytes(const std::vector<uint8_t>& data,
                                   const std::string& lowerName) {
    // WA2 的 TGA 均为未压缩后的归档条目。直接读 18 字节头即可在 IMG
    // 再分配 Surface 之前腾出旧纹理，避免解码峰值把 applet 堆顶满。
    if (lowerName.size() >= 4 &&
        lowerName.compare(lowerName.size() - 4, 4, ".tga") == 0 &&
        data.size() >= 18) {
        const size_t w = (size_t)data[12] | ((size_t)data[13] << 8);
        const size_t h = (size_t)data[14] | ((size_t)data[15] << 8);
        if (w && h && w <= 16384 && h <= 16384) return w * h * 4u;
    }
    return std::max<size_t>(data.size(), 256u * 1024u);
}

static SDL_Surface* DecodeImage(const std::vector<uint8_t>& data,
                                const std::string& lowerName) {
    SDL_RWops* rw = SDL_RWFromMem((void*)data.data(), (int)data.size());
    SDL_Surface* surf = rw ? IMG_Load_RW(rw, 1) : nullptr;
    if (surf) return surf;

    // 自动识别失败(TGA 无魔数头)时,按扩展名强制调用对应解码器。
    const size_t dot = lowerName.rfind('.');
    if (dot == std::string::npos) return nullptr;
    std::string type = lowerName.substr(dot + 1);
    for (char& c : type) c = (char)std::toupper((unsigned char)c);
    SDL_RWops* typed = SDL_RWFromMem((void*)data.data(), (int)data.size());
    return typed ? IMG_LoadTyped_RW(typed, 1, type.c_str()) : nullptr;
}

size_t Gfx::EvictUnused(size_t incomingBytes, bool allUnused) {
    size_t freed = 0;
    size_t count = 0;
    while (allUnused || cacheBytes_ + incomingBytes > cacheBudgetBytes_) {
        auto victim = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.lastUsedFrame == frameSerial_) continue;
            if (victim == cache_.end() ||
                it->second.lastUsedFrame < victim->second.lastUsedFrame)
                victim = it;
        }
        if (victim == cache_.end()) break;
        const size_t bytes = victim->second.bytes;
        if (victim->second.tex) SDL_DestroyTexture(victim->second.tex);
        cacheBytes_ = bytes > cacheBytes_ ? 0 : cacheBytes_ - bytes;
        freed += bytes;
        ++count;
        cache_.erase(victim);
    }
    if (count) {
        Log(LogLevel::Info,
            "gfx: evicted %zu texture(s), freed=%.1f MiB cache=%.1f/%.1f MiB",
            count, (double)freed / (1024.0 * 1024.0),
            (double)cacheBytes_ / (1024.0 * 1024.0),
            (double)cacheBudgetBytes_ / (1024.0 * 1024.0));
    }
    return freed;
}

Tex* Gfx::Get(const std::string& lowerName, Res& res, const std::string& effectMode) {
    // cache_ 为 std::map:节点地址稳定；LRU 不淘汰当前帧已经 Get 的节点，
    // 因此 RenderAdv 中先取得 window/frame 再绘制也不会留下悬垂指针。
    auto it = cache_.find(lowerName);
    if (it != cache_.end()) {
        it->second.lastUsedFrame = frameSerial_;
        return &it->second;
    }
    if (missing_.count(lowerName)) return nullptr;   // 本会话已确认失败,不再每帧重试

    std::vector<uint8_t> data = res.Load(lowerName);
    if (data.empty()) {
        missing_.insert(lowerName);
        Log(LogLevel::Warn, "gfx: missing %s", lowerName.c_str());
        return nullptr;
    }
    const size_t incomingBytes = EstimateTextureBytes(data, lowerName);
    EvictUnused(incomingBytes, false);

    SDL_Surface* surf = DecodeImage(data, lowerName);
    if (!surf) {
        std::string error = IMG_GetError() ? IMG_GetError() : "unknown error";
        if (IsOutOfMemoryError(error.c_str()) && EvictUnused(0, true) > 0) {
            Log(LogLevel::Warn, "gfx: decode %s hit OOM; retrying after LRU purge",
                lowerName.c_str());
            surf = DecodeImage(data, lowerName);
            if (!surf) error = IMG_GetError() ? IMG_GetError() : error;
        }
        if (!surf) {
            // OOM 是瞬时资源压力，绝不能记入 missing_；否则即使下一帧释放了
            // 内存，该背景/立绘仍会永久消失。
            if (!IsOutOfMemoryError(error.c_str())) missing_.insert(lowerName);
            Log(LogLevel::Warn,
                "gfx: decode %s failed: %s (cache=%.1f/%.1f MiB, retryable=%d)",
                lowerName.c_str(), error.c_str(),
                (double)cacheBytes_ / (1024.0 * 1024.0),
                (double)cacheBudgetBytes_ / (1024.0 * 1024.0),
                IsOutOfMemoryError(error.c_str()) ? 1 : 0);
            if (IsOutOfMemoryError(error.c_str())) LogHeapStats("texture decode OOM");
            return nullptr;
        }
    }
    // 调色板 LUT(仅 32 位 RGBA 路径)
    if (!effectMode.empty()) {
        std::vector<uint8_t> lut = res.Load(ToLower(effectMode));
        if (!lut.empty() && surf->format->BytesPerPixel == 4) {
            SDL_PixelFormat* f = surf->format;
            if (SDL_ISPIXELFORMAT_ALPHA(f->format)) {
                uint8_t* px = (uint8_t*)surf->pixels;
                ApplyPaletteLUTBytes(px, (size_t)surf->pitch * (size_t)surf->h, lut);
            }
        }
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    if (!tex && IsOutOfMemoryError(SDL_GetError())) {
        LogHeapStats("texture upload OOM");
    }
    if (!tex && IsOutOfMemoryError(SDL_GetError()) && EvictUnused(0, true) > 0) {
        Log(LogLevel::Warn, "gfx: upload %s hit OOM; retrying after LRU purge",
            lowerName.c_str());
        tex = SDL_CreateTextureFromSurface(renderer_, surf);
    }
    Tex t;
    t.w = surf->w;
    t.h = surf->h;
    t.tex = tex;
    t.bytes = (size_t)surf->pitch * (size_t)surf->h;
    t.lastUsedFrame = frameSerial_;
    SDL_FreeSurface(surf);
    if (!tex) {
        Log(LogLevel::Warn, "gfx: upload %s failed: %s", lowerName.c_str(), SDL_GetError());
        return nullptr;
    }
    cache_[lowerName] = t;
    cacheBytes_ += t.bytes;
    ++textureLoadCount_;
    Log(LogLevel::Info, "gfx: loaded %s (%dx%d cache=%.1f/%.1f MiB)",
        lowerName.c_str(), t.w, t.h,
        (double)cacheBytes_ / (1024.0 * 1024.0),
        (double)cacheBudgetBytes_ / (1024.0 * 1024.0));
    if ((textureLoadCount_ % 32u) == 0) LogHeapStats("after 32 texture loads");
    return &cache_[lowerName];
}

Tex* Gfx::LoadMask(const std::string& lowerName, Res& res) {
    return Get(lowerName, res, "");   // BMP 以 RGBA 进缓存即可
}

void Gfx::ClearCache() {
    for (auto& [k, t] : cache_) if (t.tex) SDL_DestroyTexture(t.tex);
    cache_.clear();
    cacheBytes_ = 0;
    missing_.clear();
}

void Gfx::Release(const std::string& lowerName) {
    auto it = cache_.find(lowerName);
    if (it != cache_.end()) {
        if (it->second.tex) SDL_DestroyTexture(it->second.tex);
        cacheBytes_ = it->second.bytes > cacheBytes_ ? 0 : cacheBytes_ - it->second.bytes;
        cache_.erase(it);
    }
}

void Gfx::Clear() {
    if (++frameSerial_ == 0) {
        frameSerial_ = 1;
        for (auto& [name, texture] : cache_) texture.lastUsedFrame = 0;
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
}

void Gfx::Present() {
#ifdef __SWITCH__
    if (!softwareFrame_ || !nativeFramebuffer_) return;
    SDL_RenderPresent(renderer_); // software renderer 的提交点（不调用 GPU）
    u32 stride = 0;
    uint8_t* dst = (uint8_t*)framebufferBegin(nativeFramebuffer_, &stride);
    if (!dst) {
        static bool logged = false;
        if (!logged) {
            Log(LogLevel::Error, "gfx: framebufferBegin returned null");
            logged = true;
        }
        return;
    }
    const uint8_t* src = (const uint8_t*)softwareFrame_->pixels;
    const size_t rowBytes = (size_t)kVirtualW * 4u;
    for (int y = 0; y < kVirtualH; ++y) {
        std::memcpy(dst + (size_t)y * stride,
                    src + (size_t)y * (size_t)softwareFrame_->pitch,
                    rowBytes);
    }
    framebufferEnd(nativeFramebuffer_);
#else
    SDL_RenderPresent(renderer_);
#endif
}

SDL_Texture* Gfx::CaptureScreen() {
#ifdef __SWITCH__
    // GLES2 的 RenderReadPixels 会为每次转场创建大块读回/暂存缓冲；真机日志
    // 已证明它在长时间运行后先 OOM，随后令 Mesa 后台线程崩溃。Switch 使用
    // Engine 的纯绘制淡入回退，不再做任何 GPU→CPU→GPU 往返。
    static bool logged = false;
    if (!logged) {
        Log(LogLevel::Info, "gfx: framebuffer readback disabled on Switch; using low-memory fade");
        logged = true;
    }
    return nullptr;
#else
    if (!snapshot_) {
        snapshot_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                                      SDL_TEXTUREACCESS_STREAMING, kVirtualW, kVirtualH);
    }
    // 从当前后缓冲读回画面
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, kVirtualW, kVirtualH, 32, SDL_PIXELFORMAT_RGBA8888);
    if (!s) { Log(LogLevel::Error, "GFX CaptureScreen CreateSurface fail: %s", SDL_GetError()); return snapshot_; }
    if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGBA8888, s->pixels, s->pitch) != 0)
        Log(LogLevel::Error, "GFX RenderReadPixels fail: %s", SDL_GetError());
    if (SDL_UpdateTexture(snapshot_, nullptr, s->pixels, s->pitch) != 0)
        Log(LogLevel::Error, "GFX UpdateTexture fail: %s", SDL_GetError());
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(snapshot_, SDL_BLENDMODE_BLEND);
    return snapshot_;
#endif
}

void Gfx::ReleaseSnapshot(SDL_Texture* t) { (void)t; /* 复用,不销毁 */ }

bool Gfx::SaveScreenshot(const std::string& path) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(
        0, kVirtualW, kVirtualH, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return false;
    const bool ok = SDL_RenderReadPixels(renderer_, nullptr, s->format->format,
                                         s->pixels, s->pitch) == 0 &&
                    SDL_SaveBMP(s, path.c_str()) == 0;
    SDL_FreeSurface(s);
    return ok;
}

void Gfx::DrawTexture(Tex* t, int x, int y, int w, int h, float alpha) {
    if (!t || !t->tex) return;
    SDL_Rect dst{x, y, w > 0 ? w : t->w, h > 0 ? h : t->h};
    // TEMP PROBE: 非法 rect 检测
    if (dst.w <= 0 || dst.h <= 0 || dst.w > 16384 || dst.h > 16384 ||
        dst.x < -16384 || dst.y < -16384 || dst.x > 16384 || dst.y > 16384)
        Log(LogLevel::Error, "GFX bad rect DrawTexture x=%d y=%d w=%d h=%d tex=%p",
            dst.x, dst.y, dst.w, dst.h, (void*)t->tex);
    SDL_SetTextureAlphaMod(t->tex, (uint8_t)(alpha * 255));
    if (SDL_RenderCopy(renderer_, t->tex, nullptr, &dst) != 0)
        Log(LogLevel::Error, "GFX RenderCopy fail: %s", SDL_GetError());
}

void Gfx::DrawTextureRegion(Tex* t, int srcX, int srcY, int srcW, int srcH,
                            int x, int y, int w, int h, float alpha) {
    if (!t || !t->tex || srcW <= 0 || srcH <= 0) return;
    SDL_Rect src{srcX, srcY, srcW, srcH};
    SDL_Rect dst{x, y, w > 0 ? w : srcW, h > 0 ? h : srcH};
    SDL_SetTextureAlphaMod(t->tex, (uint8_t)std::clamp(alpha * 255.0f, 0.0f, 255.0f));
    if (SDL_RenderCopy(renderer_, t->tex, &src, &dst) != 0)
        Log(LogLevel::Error, "GFX RenderCopy(region) fail: %s", SDL_GetError());
}

void Gfx::DrawTextureFit(Tex* t, float cx, float cy, float sx, float sy, float alpha) {
    if (!t || !t->tex) return;
    SDL_Rect dst;
    dst.w = (int)(t->w * sx);
    dst.h = (int)(t->h * sy);
    dst.x = (int)(cx - dst.w / 2.0f);
    dst.y = (int)(cy - dst.h / 2.0f);
    // TEMP PROBE: 非法 rect 检测(含 NaN/Inf 转 int 溢出)
    if (dst.w <= 0 || dst.h <= 0 || dst.w > 16384 || dst.h > 16384 ||
        dst.x < -16384 || dst.y < -16384 || dst.x > 16384 || dst.y > 16384 ||
        std::isnan(cx) || std::isnan(cy) || std::isnan(sx) || std::isnan(sy))
        Log(LogLevel::Error, "GFX bad rect DrawTextureFit x=%d y=%d w=%d h=%d sx=%f sy=%f tex=%p",
            dst.x, dst.y, dst.w, dst.h, sx, sy, (void*)t->tex);
    SDL_SetTextureAlphaMod(t->tex, (uint8_t)(alpha * 255));
    if (SDL_RenderCopy(renderer_, t->tex, nullptr, &dst) != 0)
        Log(LogLevel::Error, "GFX RenderCopy(fit) fail: %s", SDL_GetError());
}

void Gfx::FillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SDL_Rect rc{x, y, w, h};
    // TEMP PROBE: 非法 rect 检测
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
        x < -16384 || y < -16384 || x > 16384 || y > 16384)
        Log(LogLevel::Error, "GFX bad rect FillRect x=%d y=%d w=%d h=%d", x, y, w, h);
    SDL_SetRenderDrawColor(renderer_, r, g, b, a);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    if (SDL_RenderFillRect(renderer_, &rc) != 0)
        Log(LogLevel::Error, "GFX RenderFillRect fail: %s", SDL_GetError());
}

// ---------------- 文本 ----------------
// g_fontPath 已在 namespace wa2 顶部定义

static Glyph& GetGlyph(uint32_t cp, int size, SDL_Renderer* ren) {
    static Glyph empty;
    auto& m = g_glyphs[size];
    auto it = m.find(cp);
    if (it != m.end()) return it->second;
    TTF_Font* f = GetFont(size);
    if (!f) return empty;
    // UTF-8 编码单字
    char buf[8];
    int n = 0;
    if (cp < 0x80) buf[n++] = (char)cp;
    else if (cp < 0x800) { buf[n++] = 0xC0 | (cp >> 6); buf[n++] = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) {
        buf[n++] = 0xE0 | (cp >> 12); buf[n++] = 0x80 | ((cp >> 6) & 0x3F); buf[n++] = 0x80 | (cp & 0x3F);
    } else {
        buf[n++] = 0xF0 | (cp >> 18); buf[n++] = 0x80 | ((cp >> 12) & 0x3F);
        buf[n++] = 0x80 | ((cp >> 6) & 0x3F); buf[n++] = 0x80 | (cp & 0x3F);
    }
    SDL_Surface* s = TTF_RenderUTF8_Blended(f, std::string(buf, n).c_str(), {255, 255, 255, 255});
    if (!s) { m[cp] = Glyph(); return m[cp]; }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, s);
    Glyph g;
    g.w = s->w;
    g.h = s->h;
    g.tex = tex;
    SDL_FreeSurface(s);
    m[cp] = g;
    return m[cp];
}

int Gfx::LineHeight(int size) const { return size + size / 4; }

int Gfx::TextWidth(const std::string& utf8, int size) {
    int w = 0;
    for (size_t i = 0; i < utf8.size();) {
        size_t next;
        uint32_t cp = Utf8Decode(utf8, i, &next);
        if (cp == '<') {   // 忽略 <X> 标记
            size_t close = utf8.find('>', next);
            i = close != std::string::npos ? close + 1 : next;
            continue;
        }
        if (cp >= sjis::kPatchFontCodeBase && cp <= sjis::kPatchFontCodeBase + 0xFFFF) {
            w += size;
            i = next;
            continue;
        }
        Glyph& g = GetGlyph(cp, size, renderer_);
        w += g.w ? g.w : size;
        i = next;
    }
    return w;
}

int Gfx::DrawTextTyped(const std::string& utf8, int x, int y, int size, int maxChars,
                       uint8_t r, uint8_t g8, uint8_t b, uint8_t a) {
    int cx = x, drawn = 0;
    for (size_t i = 0; i < utf8.size() && (maxChars < 0 || drawn < maxChars);) {
        size_t next;
        uint32_t cp = Utf8Decode(utf8, i, &next);
        if (cp == '\n') {
            cx = x;
            y += LineHeight(size);
            i = next;
            drawn++;
            continue;
        }
        if (cp == '<') {
            size_t close = utf8.find('>', next);
            i = close != std::string::npos ? close + 1 : next;
            continue;
        }
        if (cp >= sjis::kPatchFontCodeBase && cp <= sjis::kPatchFontCodeBase + 0xFFFF) {
            int slot = sjis::PatchFontSlot((uint16_t)(cp - sjis::kPatchFontCodeBase));
            if (slot >= 0 && patchFontBody_) {
                const int gx = slot % 80, gy = slot / 80;
                if (patchFontShadow_) {
                    SDL_Rect srcShadow{gx * 32, gy * 32, 32, 32};
                    int pad = std::max(1, size * 2 / 28);
                    SDL_Rect dstShadow{cx - pad, y - pad, size + pad * 2, size + pad * 2};
                    SDL_SetTextureColorMod(patchFontShadow_, 38, 38, 38);
                    SDL_SetTextureAlphaMod(patchFontShadow_, a);
                    SDL_RenderCopy(renderer_, patchFontShadow_, &srcShadow, &dstShadow);
                }
                SDL_Rect srcBody{gx * 28, gy * 28, 28, 28};
                SDL_Rect dstBody{cx, y, size, size};
                SDL_SetTextureColorMod(patchFontBody_, r, g8, b);
                SDL_SetTextureAlphaMod(patchFontBody_, a);
                SDL_RenderCopy(renderer_, patchFontBody_, &srcBody, &dstBody);
            }
            cx += size;
            i = next;
            drawn++;
            continue;
        }
        if (cp == ' ') { cx += size / 2; i = next; drawn++; continue; }
        Glyph& g = GetGlyph(cp, size, renderer_);
        if (g.tex) {
            SDL_Rect dst{cx, y, g.w, g.h};
            SDL_SetTextureColorMod(g.tex, r, g8, b);
            SDL_SetTextureAlphaMod(g.tex, a);
            SDL_RenderCopy(renderer_, g.tex, nullptr, &dst);
            cx += g.w;
        } else {
            cx += size;
        }
        i = next;
        drawn++;
    }
    return drawn;
}

void Gfx::DrawText(const std::string& utf8, int x, int y, int size,
                   uint8_t r, uint8_t g8, uint8_t b, uint8_t a) {
    DrawTextTyped(utf8, x, y, size, -1, r, g8, b, a);
}

} // namespace wa2
