// main.cpp — 入口
#include "wa2/engine.h"
#include "wa2/util.h"

#ifdef __SWITCH__
#include <switch.h>
#include <switch/runtime/devices/romfs_dev.h>
#endif

int main(int argc, char** argv) {
#ifdef __SWITCH__
    if (R_FAILED(romfsInit())) {
        // romfs 挂载失败时仍可运行(sdmc 数据模式)
        wa2::Log(wa2::LogLevel::Warn, "romfs mount failed");
    }
    wa2::LogSetFile("sdmc:/wa2/wa2.log");
#else
    wa2::LogSetFile("wa2.log");
#endif
    wa2::Log(wa2::LogLevel::Info, "WA2-ns starting");

    std::string dataDir;
    if (argc > 1) dataDir = argv[1];

    int rc = 0;
    {
        wa2::Engine engine;
        if (!engine.Init(dataDir)) {
            wa2::Log(wa2::LogLevel::Error, "engine init failed");
            wa2::LogFlush();
            rc = 1;
        } else {
            engine.Run();
            engine.Shutdown();
        }
    }
#ifdef __SWITCH__
    romfsExit();
#endif
    return rc;
}
