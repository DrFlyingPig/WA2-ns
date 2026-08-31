// gfx.cpp — SDL2 渲染器实现(纹理缓存 / 过渡 / 字形缓存文本)
#include "gfx.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

namespace wa2 {

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
void ApplyPaletteLUT(std::vector<uint8_t>& rgba, const std::vector<uint8_t>& lut) {
    if (lut.size() != 256 && lut.size() != 768 && lut.size() != 1280) return;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
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

static std::map<int, _TTF_Font*> g_fonts;
static std::map<int, std::map<uint32_t, Glyph>> g_glyphs;

static _TTF_Font* GetFont(int size) {
    auto it = g_fonts.find(size);
    if (it != g_fonts.end()) return it->second;
    // 以 48px 打开,由 TTF_SetFontSize 缩放不可靠,直接按尺寸各开一份
    extern std::string g_fontPath;
    _TTF_Font* f = TTF_OpenFont(g_fontPath.c_str(), size);
    if (!f) {
        Log(LogLevel::Error, "font: open size %d failed: %s", size, TTF_GetError());
        return nullptr;
    }
    g_fonts[size] = f;
    return f;
}

// ---------------- Gfx ----------------
bool Gfx::Init(const std::string& fontPath) {
    extern std::string g_fontPath;
    g_fontPath = fontPath;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        Log(LogLevel::Error, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (TTF_Init() != 0) {
        Log(LogLevel::Error, "TTF_Init failed: %s", TTF_GetError());
        return false;
    }
    int flags = SDL_WINDOW_SHOWN;
#ifdef __SWITCH__
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    window_ = SDL_CreateWindow("WA2-ns", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               kVirtualW, kVirtualH, flags);
    if (!window_) { Log(LogLevel::Error, "CreateWindow failed: %s", SDL_GetError()); return false; }
    // 优先请求硬件加速(带 VSYNC);失败则记下原因退回软件,便于真机诊断后端
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
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    if (!GetFont(32)) return false;   // 字体必须可用
    Log(LogLevel::Info, "gfx: initialized (%s)", SDL_GetCurrentVideoDriver());
    return true;
}

void Gfx::Shutdown() {
    ClearCache();
    for (auto& [size, f] : g_fonts) TTF_CloseFont(f);
    g_fonts.clear();
    for (auto& [size, m] : g_glyphs)
        for (auto& [cp, g] : m) if (g.tex) SDL_DestroyTexture(g.tex);
    g_glyphs.clear();
    if (snapshot_) { SDL_DestroyTexture(snapshot_); snapshot_ = nullptr; }
    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    TTF_Quit();
    SDL_Quit();
}

Tex* Gfx::Get(const std::string& lowerName, Res& res, const std::string& effectMode) {
    auto it = cache_.find(lowerName);
    if (it != cache_.end()) return &it->second;

    std::vector<uint8_t> data = res.Load(lowerName);
    if (data.empty()) {
        Log(LogLevel::Warn, "gfx: missing %s", lowerName.c_str());
        return nullptr;
    }
    SDL_RWops* rw = SDL_RWFromMem(data.data(), (int)data.size());
    SDL_Surface* surf = IMG_Load_RW(rw, 1);
    if (!surf) {
        Log(LogLevel::Warn, "gfx: decode %s failed: %s", lowerName.c_str(), IMG_GetError());
        return nullptr;
    }
    // 调色板 LUT(仅 32 位 RGBA 路径)
    if (!effectMode.empty()) {
        std::vector<uint8_t> lut = res.Load(ToLower(effectMode));
        if (!lut.empty() && surf->format->BytesPerPixel == 4) {
            SDL_PixelFormat* f = surf->format;
            if (SDL_ISPIXELFORMAT_ALPHA(f->format)) {
                uint8_t* px = (uint8_t*)surf->pixels;
                std::vector<uint8_t> rgba(px, px + surf->pitch * surf->h);
                ApplyPaletteLUT(rgba, lut);
                memcpy(px, rgba.data(), rgba.size());
            }
        }
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    Tex t;
    t.w = surf->w;
    t.h = surf->h;
    t.tex = tex;
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;
    cache_[lowerName] = t;
    return &cache_[lowerName];
}

Tex* Gfx::LoadMask(const std::string& lowerName, Res& res) {
    return Get(lowerName, res, "");   // BMP 以 RGBA 进缓存即可
}

void Gfx::ClearCache() {
    for (auto& [k, t] : cache_) if (t.tex) SDL_DestroyTexture(t.tex);
    cache_.clear();
}

void Gfx::Release(const std::string& lowerName) {
    auto it = cache_.find(lowerName);
    if (it != cache_.end()) {
        if (it->second.tex) SDL_DestroyTexture(it->second.tex);
        cache_.erase(it);
    }
}

void Gfx::Clear() {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
}

void Gfx::Present() {
    SDL_RenderPresent(renderer_);
}

SDL_Texture* Gfx::CaptureScreen() {
    if (!snapshot_) {
        snapshot_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                                      SDL_TEXTUREACCESS_TARGET, kVirtualW, kVirtualH);
    }
    // 从当前后缓冲读回画面
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, kVirtualW, kVirtualH, 32, SDL_PIXELFORMAT_RGBA8888);
    SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGBA8888, s->pixels, s->pitch);
    SDL_UpdateTexture(snapshot_, nullptr, s->pixels, s->pitch);
    SDL_FreeSurface(s);
    SDL_SetTextureBlendMode(snapshot_, SDL_BLENDMODE_BLEND);
    return snapshot_;
}

void Gfx::ReleaseSnapshot(SDL_Texture* t) { (void)t; /* 复用,不销毁 */ }

void Gfx::DrawTexture(Tex* t, int x, int y, int w, int h, float alpha) {
    if (!t || !t->tex) return;
    SDL_Rect dst{x, y, w > 0 ? w : t->w, h > 0 ? h : t->h};
    SDL_SetTextureAlphaMod(t->tex, (uint8_t)(alpha * 255));
    SDL_RenderCopy(renderer_, t->tex, nullptr, &dst);
}

void Gfx::DrawTextureFit(Tex* t, float cx, float cy, float sx, float sy, float alpha) {
    if (!t || !t->tex) return;
    SDL_Rect dst;
    dst.w = (int)(t->w * sx);
    dst.h = (int)(t->h * sy);
    dst.x = (int)(cx - dst.w / 2.0f);
    dst.y = (int)(cy - dst.h / 2.0f);
    SDL_SetTextureAlphaMod(t->tex, (uint8_t)(alpha * 255));
    SDL_RenderCopy(renderer_, t->tex, nullptr, &dst);
}

void Gfx::FillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SDL_Rect rc{x, y, w, h};
    SDL_SetRenderDrawColor(renderer_, r, g, b, a);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(renderer_, &rc);
}

// ---------------- 文本 ----------------
std::string g_fontPath;

static Glyph& GetGlyph(uint32_t cp, int size, SDL_Renderer* ren) {
    static Glyph empty;
    auto& m = g_glyphs[size];
    auto it = m.find(cp);
    if (it != m.end()) return it->second;
    _TTF_Font* f = GetFont(size);
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
        if (cp == '<') {
            size_t close = utf8.find('>', next);
            i = close != std::string::npos ? close + 1 : next;
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
