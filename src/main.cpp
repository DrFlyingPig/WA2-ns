// main.cpp — 入口
#include "wa2/engine.h"
#include "wa2/util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

static std::string WideToUtf8(const wchar_t* text) {
    if (!text || !*text) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    out.pop_back();
    return out;
}

static std::string ExeDirectoryUtf8() {
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    if (n == 0 || n >= path.size()) return {};
    std::wstring full(path.data(), n);
    size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    full.resize(slash);
    return WideToUtf8(full.c_str());
}
#endif

#if defined(__SWITCH__) && defined(WA2_WITH_DEMO)
#include <switch.h>
#include <switch/runtime/devices/romfs_dev.h>
#endif

int main(int argc, char** argv) {
#if defined(__SWITCH__) && defined(WA2_WITH_DEMO)
    if (R_FAILED(romfsInit())) {
        // romfs 挂载失败时仍可运行(sdmc 数据模式)
        wa2::Log(wa2::LogLevel::Warn, "romfs mount failed");
    }
#endif
#ifdef __SWITCH__
    wa2::LogSetFile("sdmc:/wa2/wa2.log");
#else
    wa2::LogSetFile("wa2.log");
#endif
    wa2::Log(wa2::LogLevel::Info, "WA2-ns starting");

    std::string dataDir;
#ifdef _WIN32
    // Windows 的 main/argv 使用当前代码页，中文或日文目录会在传给 UTF-8 文件层前损坏。
    // 直接读取 UTF-16 命令行，再统一转换成引擎内部 UTF-8。
    int wideArgc = 0;
    wchar_t** wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv && wideArgc > 1) {
        dataDir = WideToUtf8(wideArgv[1]);
    }
    if (wideArgv) LocalFree(wideArgv);

    // 双击 EXE 时没有 argv：从 EXE 同目录的 wa2-data.txt 读取真实 PC 资源目录。
    // 文件只保存一行 UTF-8 路径；引擎和游戏资源仍完全分离，之后可直接改路径。
    if (dataDir.empty()) {
        const std::string exeDir = ExeDirectoryUtf8();
        std::vector<uint8_t> raw = wa2::ReadFileAll(wa2::PathJoin(exeDir, "wa2-data.txt"));
        if (!raw.empty()) {
            size_t off = raw.size() >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf ? 3 : 0;
            dataDir = wa2::Trim(std::string(raw.begin() + off, raw.end()));
        }
        // 便携模式：若把资源放到 EXE 旁的 Wa2Res，也无需配置文件。
        if (dataDir.empty() && wa2::FileExists(wa2::PathJoin(wa2::PathJoin(exeDir, "Wa2Res"), "game.ini")))
            dataDir = wa2::PathJoin(exeDir, "Wa2Res");
    }
#else
    if (argc > 1) dataDir = argv[1];
#endif

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
#if defined(__SWITCH__) && defined(WA2_WITH_DEMO)
    romfsExit();
#endif
    return rc;
}
