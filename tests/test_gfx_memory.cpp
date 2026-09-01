// test_gfx_memory.cpp -- 用真资源复现 1002 脚本的背景/立绘加载并验证 Switch LRU 预算。
#include "wa2/gfx.h"
#include "wa2/res.h"
#include "wa2/util.h"

#include <SDL2/SDL.h>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_gfx_memory <WA2 data dir>\n");
        return 2;
    }

    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_setenv("SDL_RENDER_DRIVER", "software", 1);
    // 与 Switch 正式版一致：验证 8 MiB 预算下真实背景、对话框和立绘
    // 仍能组成完整当前帧工作集，同时旧场景会被及时淘汰。
    SDL_setenv("WA2_TEXTURE_CACHE_MB", "8", 1);
    wa2::LogSetFile("out/gfx_memory.log");

    wa2::Res res;
    res.SetDataDir(argv[1]);
    res.ScanArchives();

    wa2::Gfx gfx;
    const std::string font = wa2::PathJoin(argv[1], "font.ttf");
    if (!gfx.Init(font)) return 3;
    if (res.UsesPatchFont() && !gfx.EnablePatchFont(res)) {
        gfx.Shutdown();
        return 4;
    }

    auto require = [&](const char* name) {
        wa2::Tex* tex = gfx.Get(name, res, "");
        if (!tex) std::fprintf(stderr, "failed to load %s\n", name);
        return tex;
    };

    // 先留下两个已经不用的旧背景，再进入日志中发生 OOM 的 b100401 场景。
    for (const char* bg : {"b000000.tga", "b100101.tga", "b100401.tga"}) {
        gfx.Clear();
        wa2::Tex* tex = require(bg);
        if (!tex) { gfx.Shutdown(); return 5; }
        gfx.DrawTexture(tex, 0, 0, 1280, 720, 1.0f);
        gfx.Present();
    }

    const char* chars[] = {
        "tak001103.tga", "tak001107.tga", "tak001105.tga",
        "tak001104.tga", "tak001106.tga", "tak001111.tga",
        "tak001101.tga",
    };
    for (const char* character : chars) {
        gfx.Clear();
        wa2::Tex* bg = require("b100401.tga");
        wa2::Tex* window = require("sys_00001.tga");
        wa2::Tex* frame = require("sys_00000.tga");
        wa2::Tex* actor = require(character);
        if (!bg || !window || !frame || !actor) {
            gfx.Shutdown();
            return 6;
        }
        gfx.DrawTexture(bg, 0, 0, 1280, 720, 1.0f);
        gfx.DrawTexture(actor, 256, 0, 0, 0, 1.0f);
        gfx.DrawTexture(window, 16, 505, 1248, 208, 0.5f);
        gfx.DrawTexture(frame, 16, 505, 1248, 208, 1.0f);
        gfx.Present();
        if (gfx.CachedTextureBytes() > gfx.TextureCacheBudget()) {
            std::fprintf(stderr, "cache exceeded budget: %zu > %zu\n",
                         gfx.CachedTextureBytes(), gfx.TextureCacheBudget());
            gfx.Shutdown();
            return 7;
        }
    }

    std::printf("GFX MEMORY PASSED (cache=%zu bytes, textures=%zu, budget=%zu bytes)\n",
                gfx.CachedTextureBytes(), gfx.CachedTextureCount(),
                gfx.TextureCacheBudget());
    gfx.Shutdown();
    return 0;
}
