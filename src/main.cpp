// main.cpp — 入口
#include "wa2/engine.h"
#include "wa2/util.h"

#include <atomic>
#include <cstdio>

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

#ifdef __SWITCH__
#include <switch.h>
#endif

#if defined(__SWITCH__) && defined(WA2_WITH_DEMO)
#include <switch/runtime/devices/romfs_dev.h>
#endif

static int RunApplication(int argc, char** argv) {
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
#ifdef WA2_DIAG_CHARACTER_LIFECYCLE
    wa2::Log(wa2::LogLevel::Info,
             "build: F1 character desired-list lifecycle enabled (P2B baseline)");
#endif
#ifdef __SWITCH__
    if (Thread* self = threadGetSelf()) {
#ifdef WA2_DIAG_PARALLEL_FRAMEBUFFER
        wa2::Log(wa2::LogLevel::Info,
                 "runtime: engine thread=libnx-native stack=%.1f MiB core=%u",
                 (double)self->stack_sz / (1024.0 * 1024.0),
                 (unsigned)svcGetCurrentProcessorNumber());
#else
        wa2::Log(wa2::LogLevel::Info,
                 "runtime: engine thread=libnx-native stack=%.1f MiB",
                 (double)self->stack_sz / (1024.0 * 1024.0));
#endif
    }
#endif

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
        const bool initialized = engine.Init(dataDir);
        if (!initialized) {
            wa2::Log(wa2::LogLevel::Error, "engine init failed");
            wa2::LogFlush();
            rc = 1;
        } else {
            engine.Run();
        }
        wa2::Log(wa2::LogLevel::Info, "runtime: engine shutdown requested init=%d",
                 initialized ? 1 : 0);
        engine.Shutdown();
        wa2::Log(wa2::LogLevel::Info, "runtime: engine shutdown returned");
    }
#if defined(__SWITCH__) && defined(WA2_WITH_DEMO)
    romfsExit();
#endif
    return rc;
}

#ifdef __SWITCH__
namespace {
constexpr size_t kEngineThreadStack = 4u * 1024u * 1024u;
constexpr int kEngineThreadCore = -2;

struct EngineThreadContext {
    int argc = 0;
    char** argv = nullptr;
    int result = 1;
    std::atomic<bool> completed{false};
};

void EngineThreadMain(void* opaque) {
    auto* context = static_cast<EngineThreadContext*>(opaque);
    context->result = RunApplication(context->argc, context->argv);
    context->completed.store(true, std::memory_order_release);
}
} // namespace

int main(int argc, char** argv) {
    // HBLoader 的 NRO 入口线程只有约 128 KiB 栈。真实数据下，中文字体、
    // SDL 软件渲染和存档场景重建会叠加较深的调用链；旧版就在这条入口
    // 线程上运行整个 Engine，Atmosphere 报告显示其异常展开栈已损坏。
    // 入口线程只保留启动/等待职责，游戏主体改在 libnx 原生大栈线程运行。
    EngineThreadContext context{};
    context.argc = argc;
    context.argv = argv;
    Thread engineThread{};
    Result rc = threadCreate(&engineThread, EngineThreadMain, &context, nullptr,
                             kEngineThreadStack, 0x2c, kEngineThreadCore);
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "WA2-ns: engine threadCreate failed: 0x%x\n", rc);
        return 1;
    }
    rc = threadStart(&engineThread);
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "WA2-ns: engine threadStart failed: 0x%x\n", rc);
        threadClose(&engineThread);
        return 1;
    }
    rc = threadWaitForExit(&engineThread);
    const bool workerCompleted = context.completed.load(std::memory_order_acquire);
    if (R_SUCCEEDED(rc) && !workerCompleted) {
        // 不能让 hbloader 在 SDL 回调线程仍存活时卸载 NRO。
        // 必须在 threadClose 释放 Engine/Audio 所在的工作线程栈之前
        // 取消回调并 join 音频线程。
        wa2::Log(wa2::LogLevel::Error,
                 "runtime: engine worker exited without completion marker");
        wa2::Audio::EmergencyShutdown();
        SDL_Quit();
        wa2::LogFlush();
    }
    const Result closeRc = threadClose(&engineThread);
    if (R_FAILED(rc) || R_FAILED(closeRc) || !workerCompleted) {
        std::fprintf(stderr, "WA2-ns: engine thread join failed: 0x%x/0x%x\n",
                     rc, closeRc);
        return 1;
    }
    return context.result;
}
#else
int main(int argc, char** argv) {
    return RunApplication(argc, argv);
}
#endif
