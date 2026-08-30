// test_core.cpp — 核心层无头自测(无 SDL 依赖)
//
// 覆盖:Shift-JIS 转换、PACK/LZSS 归档读取、脚本 VM 全流程
// (背景/立绘指令、文本、选项分支、旗标、脚本切换、存档回环)。
// 前置:先运行 tools/gen_demo.py 生成 out/Wa2Res/。
#include <cassert>
#include <cstdio>
#include <cstring>

#include "wa2/util.h"
#include "wa2/archive.h"
#include "wa2/res.h"
#include "wa2/script.h"
#include "wa2/funcs.h"
#include "wa2/sav.h"
#include "wa2/sjis.h"

using namespace wa2;

static int g_checks = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); return 1; } \
    g_checks++; } while (0)
#define CHECK_SOFT(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_failed++; } \
    g_checks++; } while (0)
static int g_failed = 0;

// ---------------- 无头宿主 ----------------
struct TestHost : Host {
    Res* res = nullptr;
    std::vector<int> gameFlags = std::vector<int>(kMaxGameFlags, 0);
    std::vector<uint8_t> sysFlags = std::vector<uint8_t>(kSysFlagCount, 0);

    // 脚本栈(墓地:Tick 执行中被替换的脚本,ApplyPending 时统一释放)
    std::vector<std::unique_ptr<Script>> stack;
    std::vector<std::unique_ptr<Script>> graveyard;
    std::vector<std::string> loaded;      // SLoad/SCall 记录
    bool titleCalled = false;
    int voiceLabel = 0;

    // 事件记录
    std::vector<std::string> events;
    std::vector<std::string> names;
    std::vector<std::string> messages;
    int bgCount = 0, charCount = 0, selectCount = 0, calenderCount = 0;
    int pickChoice = 0;                   // ShowSelect 时自动选择的选择号
    int waits = 0;

    Script* Active() { return stack.empty() ? nullptr : stack.back().get(); }
    void ApplyPending() override { graveyard.clear(); }

    // --- 流程 ---
    void SLoadScript(const std::string& name, int point) override {
        for (auto& s : stack) graveyard.push_back(std::move(s));
        stack.clear();
        auto s = std::make_unique<Script>();
        s->SetGameFlags(&gameFlags);
        s->Load(*res, name, point);
        stack.push_back(std::move(s));
        loaded.push_back("SLoad:" + name);
    }
    void SCallScript(const std::string& name, int point) override {
        auto s = std::make_unique<Script>();
        s->SetGameFlags(&gameFlags);
        s->Load(*res, name, point);
        stack.push_back(std::move(s));
        loaded.push_back("SCall:" + name);
    }
    void CallPoint(int point) override {
        if (Active()) { /* demo 未用:忽略 */ }
    }
    void GoTitle() override { titleCalled = true; }
    void PushInt(int v) override {
        if (Active()) Active()->PushInt(5, 3, v);
    }
    void PushFloat(float v) override {
        if (Active()) Active()->PushFloat(5, 4, v);
    }

    // --- 旗标 ---
    int  ReadSysFlag(int idx) override { return idx >= 0 && idx < kSysFlagCount ? sysFlags[idx] : 0; }
    void WriteSysFlag(int idx, int v) override { if (idx >= 0 && idx < kSysFlagCount) sysFlags[idx] = (uint8_t)v; }
    int  ReadGameFlag(int idx) override { return gameFlags[idx]; }
    void WriteGameFlag(int idx, int v) override { gameFlags[idx] = v; }

    // --- 文本 ---
    void ShowMessage(const std::string& text, int msgIdx, int mode, bool append) override {
        messages.push_back(text);
        events.push_back("MSG");
    }
    void EndMessage() override { events.push_back("ENDMSG"); }
    void SetName(const std::string& name) override { if (!name.empty()) names.push_back(name); }
    void WaitClick() override { waits++; }
    void HideWindow(int) override {}
    void SetDemoMode(bool v) override {}

    // --- 画面 ---
    void RenderImage(int id, int efc, bool keepChar, int type, int frame,
                     int offset, int x, int y, float sx, float sy) override {
        (void)efc; (void)offset; (void)x; (void)y; (void)sx; (void)sy;
        std::string path = type == 1 ? Res::CgName(id) : Res::BgName(id, 0);
        CHECK_SOFT(res->Exists(path), (std::string("bg resource missing: ") + path).c_str());
        bgCount++;
    }
    void AddChar(int id, int no, int pos) override { charCount++; }
    void UpdateChar(int) override {}
    void RemoveChar(int) override {}
    void ColorFade(int, int, int, int) override { events.push_back("FADE"); }
    void ShowCalender(int y, int m, int d, int dow) override { calenderCount++; }

    // --- 选项 ---
    void AddSelectItem(const std::string& text, int, int, int) override { selectCount++; }
    void ShowSelect() override {
        // 自动选择:与真机点击行为一致——把选择号(0 起始)写回参数栈顶的变量
        Script* s = Active();
        if (s && !s->args.empty()) {
            Val v;
            v.kind = Val::Int;
            v.i = pickChoice;
            s->SetVar(s->args.back(), v);
        }
    }

    // --- 音频(记录即可) ---
    void PlayBgm(int id, bool loop, int vol) override { events.push_back("BGM"); }
    void StopBgm(int) override {}
    void SetVoiceLabel(int label) override { voiceLabel = label; }
    void PlayVoice(int, int, int, bool, int) override {}
    void WaitVoice(int) override { waits++; }
    void StopVoice(int, int) override {}
    void PlaySe(int ch, int id, bool, int, int) override { events.push_back("SE"); }
    void StopSe(int, int) override {}
    void WaitSe(int) override { waits++; }

    // --- 定时 ---
    void WaitMs(float ms) override { waits++; }
    void StartTimer() override {}
    int  ElapsedTimerMs() override { return 0; }
};

// ---------------- 用例 ----------------
static int TestSjis() {
    // 「春希」= CP932 8F 74 8A F3 → UTF-8 E6 98 A5 E5 B8 8C(以 Python cp932 编码校验)
    const uint8_t sjis[] = {0x8F, 0x74, 0x8A, 0xF3};
    std::string u8 = sjis::ToUtf8(sjis, sizeof(sjis));
    CHECK(u8 == "\xE6\x98\xA5\xE5\xB8\x8C", "sjis haruki->utf8");
    // 半角假名 0xB1 = ｱ (U+FF71)
    const uint8_t hk[] = {0xB1};
    CHECK(sjis::ToUtf8(hk, 1) == "\xEF\xBD\xB1", "sjis halfwidth katakana");
    // ASCII 直通
    const uint8_t ascii[] = "abc123";
    CHECK(sjis::ToUtf8(ascii, 6) == "abc123", "sjis ascii passthrough");
    return 0;
}

static int TestArchiveLzss() {
    // 归档内脚本经过 LZSS:直接对比 Python 端打包时的原文件
    auto bnr = Archive::LoadFile("9001.bnr");
    CHECK(!bnr.empty(), "lzss 9001.bnr decompressed");
    CHECK(bnr.size() > 12 && ReadU32(bnr.data()) == 0x5243534C, "bnr magic LSCR");
    // 尾部 4 个字应为 sload 的 call+flush:[4,0,0,8](与 Python 端打包结果一致)
    CHECK(bnr.size() >= 1520, "bnr full length");
    const uint8_t* tail = bnr.data() + bnr.size() - 16;
    CHECK(ReadU32(tail) == 4 && ReadU32(tail + 4) == 0, "bnr tail call words");
    return 0;
}

// 推进脚本直到事件数增长或脚本结束(自动点击)
static int Pump(TestHost& host, int maxTicks = 20000) {
    int lastEvents = (int)host.messages.size() + (int)host.events.size();
    for (int i = 0; i < maxTicks; i++) {
        host.ApplyPending();
        Script* s = host.Active();
        if (!s) return -1;   // 栈空
        if (s->exitFlag()) return 0;
        s->Tick(host);
        // Tick 每次只跑到第一个让出点;测试里无限供给"点击/定时完成"
        int now = (int)host.messages.size() + (int)host.events.size();
        if (now != lastEvents) return 1;
        lastEvents = now;
    }
    return 2;
}

static int TestScriptRun() {
    Res res;
    res.SetDataDir("out/Wa2Res");
    res.ScanArchives();

    TestHost host;
    host.res = &res;
    auto s = std::make_unique<Script>();
    s->SetGameFlags(&host.gameFlags);
    CHECK(s->Load(res, "9001", 0), "load 9001");
    host.stack.push_back(std::move(s));

    // 前半:跑到选项前
    for (int i = 0; i < 50 && host.messages.size() < 6; i++) {
        if (Pump(host) <= 0) break;
    }
    CHECK(host.bgCount >= 1, "bg rendered");
    CHECK(host.charCount >= 2, "chars added");
    CHECK(host.names.size() >= 2, "names set");
    CHECK(host.messages.size() >= 6, "messages shown");
    CHECK(!host.events.empty() && host.events[0] == "BGM", "bgm played first");

    // 推进到选项出现并选择
    for (int i = 0; i < 200 && host.selectCount < 2; i++) Pump(host);
    CHECK(host.selectCount == 2, "two select items");
    host.pickChoice = 0;   // 选第一项
    for (int i = 0; i < 200 && host.sysFlags[101] != 1; i++) Pump(host);
    CHECK(host.gameFlags[100] == 0, "choice result written to global var");
    CHECK(host.sysFlags[101] == 1, "branch A flag set");
    CHECK(host.sysFlags[102] == 0, "branch B flag untouched");

    // 9001 结束 → SLoad 9003
    for (int i = 0; i < 500; i++) {
        Script* s2 = host.Active();
        if (s2 && s2->exitFlag()) break;
        Pump(host);
    }
    bool switched = false;
    for (const auto& l : host.loaded) if (l == "SLoad:9003") switched = true;
    CHECK(switched, "SLoad 9003");
    CHECK(host.calenderCount == 1, "calender shown once");
    CHECK(host.titleCalled, "GoTitle called");
    return 0;
}

static int TestSaveRoundtrip() {
    // 存档回环:序列化后再反序列化,状态一致
    SaveData sav;
    sav.Reset();
    sav.meta.chapter = "9001";
    sav.meta.preview = "テスト";
    sav.gameFlags[5] = 42;
    sav.sysFlags[900] = 7;
    ByteBuf buf;
    sav.Save(buf);

    SaveData sav2;
    sav2.Reset();
    ByteReader in(buf.data().data(), buf.data().size());
    CHECK(sav2.Load(in), "saveload magic+fields");
    CHECK(sav2.meta.chapter == "9001", "saveload chapter");
    CHECK(sav2.gameFlags[5] == 42, "saveload gameflag");
    CHECK(sav2.sysFlags[900] == 7, "saveload sysflag");
    return 0;
}

int main(int argc, char** argv) {
    LogSetFile("test_out.log");
    printf("== WA2-ns core tests ==\n");

    if (TestSjis() != 0) return 1;
    printf("[ok] sjis conversion (%d checks)\n", g_checks);

    // 归档环境
    Res res;
    res.SetDataDir(argc > 1 ? argv[1] : "out/Wa2Res");
    res.ScanArchives();
    if (!Archive::Has("9001.bnr")) {
        printf("SKIP: run tools/gen_demo.py first (out/Wa2Res not found)\n");
        return 2;
    }
    if (TestArchiveLzss() != 0) return 1;
    printf("[ok] archive + lzss (%d checks)\n", g_checks);

    if (TestScriptRun() != 0) return 1;
    printf("[ok] script vm full run (%d checks)\n", g_checks);

    if (TestSaveRoundtrip() != 0) return 1;
    printf("[ok] save roundtrip (%d checks)\n", g_checks);

    printf("ALL TESTS PASSED (%d checks, %d failed)\n", g_checks, g_failed);
    LogFlush();
    return g_failed == 0 ? 0 : 1;
}
