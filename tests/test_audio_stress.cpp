// test_audio_stress.cpp -- 用真实 WA2 BGM 反复换曲，覆盖 SDL_mixer finished-hook 与 Halt/Free 交错路径。
#include "wa2/audio.h"
#include "wa2/res.h"
#include "wa2/util.h"

#include <SDL2/SDL.h>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_audio_stress <WA2 data dir>\n");
        return 2;
    }

    // 测试不占用真实声卡，但保留 SDL 音频线程和 mixer 回调。
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    wa2::LogSetFile("out/audio_stress.log");

    wa2::Res res;
    res.SetDataDir(argv[1]);
    res.ScanArchives();

    wa2::Audio audio;
    if (!audio.Init()) return 3;

    const int ids[] = {31, 8, 3, 1};
    int plays = 0;
    for (int round = 0; round < 12; ++round) {
        for (int id : ids) {
            if (!audio.PlayBgm(id, true, 220, res)) {
                std::fprintf(stderr, "failed to play BGM %d at round %d\n", id, round);
                audio.Shutdown();
                return 4;
            }
            ++plays;
            // 压力测试不等待整段前奏：模拟音频线程只投递 finished，
            // 再由主线程 Update 启动 B 段，随后下一轮立即停止并释放它。
            audio.NotifyMusicFinished();
            audio.Update();
            // 让 dummy 音频线程真正进入混音，主线程同时消费完成事件。
            for (int frame = 0; frame < 8; ++frame) {
                SDL_Delay(4);
                audio.Update();
            }
        }
    }

    audio.StopAll();
    audio.Shutdown();
    std::printf("AUDIO STRESS PASSED (%d BGM switches)\n", plays);
    return 0;
}
