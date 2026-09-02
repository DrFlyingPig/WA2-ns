// test_core.cpp — 核心层无头自测(无 SDL 依赖)
//
// 覆盖:Shift-JIS 转换、PACK/LZSS 归档读取、脚本 VM 全流程
// (背景/立绘指令、文本、选项分支、旗标、脚本切换、存档回环)。
// 前置:先运行 tools/gen_demo.py 生成 out/Wa2Res/。
#include <cassert>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "wa2/util.h"
#include "wa2/archive.h"
#include "wa2/audio_ring.h"
#include "wa2/character_state.h"
#include "wa2/res.h"
#include "wa2/script.h"
#include "wa2/funcs.h"
#include "wa2/sav.h"
#include "wa2/special_mode.h"
#include "wa2/sjis.h"
#ifdef WA2_DIAG_TEXT_FIT
#include "wa2/text_layout.h"
#endif

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
    int voicePlayCount = 0;
    int voicePlayLabel = -1, voicePlayId = -1, voicePlayChr = -1;
    int voicePlayVolume = -1, voicePlayChannel = -1;
    bool voicePlayLoop = false;
    int charAddId = -1, charAddNo = -1, charAddPos = -1;
    int charUpdateCount = 0, charUpdateFrames = -1;
    int charRemoveCount = 0, charRemoveId = -1;

    // 事件记录
    std::vector<std::string> events;
    std::vector<std::string> names;
    std::vector<std::string> messages;
    int bgCount = 0, charCount = 0, selectCount = 0, calenderCount = 0;
    int pickChoice = 0;                   // ShowSelect 时自动选择的选择号
    int waits = 0;
    float lastWaitMs = -1.0f;
    int elapsedTimerMs = 0;
    int replayMode = 0;
    int lastPushedInt = -1;

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
        lastPushedInt = v;
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
    void AddChar(int id, int no, int pos) override {
        charCount++;
        charAddId = id; charAddNo = no; charAddPos = pos;
    }
    int ReplayMode() const override { return replayMode; }
    void UpdateChar(int frames) override {
        charUpdateCount++;
        charUpdateFrames = frames;
    }
    void RemoveChar(int id) override {
        charRemoveCount++;
        charRemoveId = id;
    }
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
    int CurrentVoiceLabel() const override { return voiceLabel; }
    void PlayVoice(int label, int id, int chr, int volume, bool loop, int channel) override {
        voicePlayCount++;
        voicePlayLabel = label;
        voicePlayId = id;
        voicePlayChr = chr;
        voicePlayVolume = volume;
        voicePlayLoop = loop;
        voicePlayChannel = channel;
    }
    void WaitVoice(int) override { waits++; }
    void StopVoice(int, int) override {}
    void PlaySe(int ch, int id, bool, int, int) override { events.push_back("SE"); }
    void StopSe(int, int) override {}
    void WaitSe(int) override { waits++; }

    // --- 定时 ---
    void WaitMs(float ms) override { waits++; lastWaitMs = ms; }
    void StartTimer() override {}
    int  ElapsedTimerMs() override { return elapsedTimerMs; }
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
    // CK-GAL 的 8B C5 不是 Unicode 日文「暁」，重建码表后应成为中文“啊”。
    const uint8_t patch[] = {'A', 0x8B, 0xC5};
    std::string expected = "A\xE5\x95\x8A";
    CHECK(sjis::ToPatchFontUtf8(patch, sizeof(patch)) == expected, "patch code preserved");
    CHECK(sjis::PatchFontSlot(0x8BC5) == 1353, "patch font slot 8bc5");
    CHECK(sjis::PatchFontSlot(0xEAA4) == 7135, "patch font slot eaa4");
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

    std::vector<uint8_t> trailing = buf.data();
    trailing.push_back(0xee);
    ByteReader trailingIn(trailing);
    SaveData rejectedTrailing;
    CHECK(!rejectedTrailing.Load(trailingIn), "saveload rejects trailing bytes");

    ByteBuf negativeCount;
    negativeCount.U32(0x314D4157);
    negativeCount.Str("9001"); negativeCount.Str("bad");
    negativeCount.U32(0); negativeCount.U32(0);
    negativeCount.I32(-1);
    ByteReader negativeIn(negativeCount.data());
    SaveData rejectedNegative;
    CHECK(!rejectedNegative.Load(negativeIn), "saveload rejects negative flag count");

    SceneState oversizedScene;
    oversizedScene.backlog.resize(201);
    ByteBuf sceneBytes;
    oversizedScene.Save(sceneBytes);
    ByteReader sceneIn(sceneBytes.data());
    SceneState rejectedScene;
    CHECK(!rejectedScene.Load(sceneIn), "scene rejects oversized backlog");

    Config config;
    config.textSpeed = 3;
    config.autoDelayFrames = 411;
    config.masterVolume = 173;
    config.bgmVolume = 142;
    config.pageVoice = true;
    config.eroVoice = true;
    config.longPressSkip = true;
    config.fastWait = true;
    config.confirmDefaultYes = false;
    config.windowAlpha = 199;
    config.cgWindowAlpha = 111;
    config.novelWindowAlpha = 87;
    config.charVoice[7] = 0;
    ByteBuf configBytes;
    config.Save(configBytes);
    Config loadedConfig;
    ByteReader configIn(configBytes.data());
    CHECK(loadedConfig.Load(configIn), "config v3 loads");
    CHECK(loadedConfig.textSpeed == 3 && loadedConfig.autoDelayFrames == 411,
          "config v3 text and auto values");
    CHECK(loadedConfig.masterVolume == 173 && loadedConfig.bgmVolume == 142,
          "config v3 audio values");
    CHECK(loadedConfig.pageVoice && loadedConfig.eroVoice && loadedConfig.longPressSkip,
          "config v3 switches");
    CHECK(loadedConfig.windowAlpha == 199 && loadedConfig.cgWindowAlpha == 111 &&
          loadedConfig.novelWindowAlpha == 87 && loadedConfig.charVoice[7] == 0,
          "config v3 display and character voice values");
    CHECK(loadedConfig.fastWait && !loadedConfig.confirmDefaultYes,
          "config v3 original wait and confirmation preferences");

    // The immediately preceding build wrote CFG2/version 2 without the final
    // two preferences.  It must continue loading those files and supply the
    // original defaults for the newly restored settings.
    ByteBuf v2Config;
    v2Config.I32(2); v2Config.I32(2);
    v2Config.I32(134); v2Config.I32(195); v2Config.I32(255);
    v2Config.I32(1);
    v2Config.U32(0x32474643u); v2Config.U32(2);
    v2Config.I32(137); v2Config.I32(195);
    v2Config.I32(207); v2Config.I32(134); v2Config.I32(134);
    v2Config.I32(0); v2Config.I32(0); v2Config.I32(0);
    for (int i = 0; i < 10; ++i) v2Config.U8(1);
    Config migratedV2;
    ByteReader v2ConfigIn(v2Config.data());
    CHECK(migratedV2.Load(v2ConfigIn) && !migratedV2.fastWait &&
          migratedV2.confirmDefaultYes, "config v2 receives restored defaults");

    ByteBuf legacyConfig;
    legacyConfig.I32(1); legacyConfig.I32(3);
    legacyConfig.I32(101); legacyConfig.I32(102); legacyConfig.I32(103);
    legacyConfig.I32(1);
    Config migrated;
    ByteReader legacyConfigIn(legacyConfig.data());
    CHECK(migrated.Load(legacyConfigIn), "legacy config migrates");
    CHECK(migrated.textSpeed == 1 && migrated.bgmVolume == 101 && migrated.skipUnread,
          "legacy config keeps existing values");
    CHECK(migrated.autoDelayFrames == 137 && migrated.windowAlpha == 207 &&
          migrated.masterVolume == 195 && migrated.confirmDefaultYes,
          "legacy config receives original defaults for new values");
    return 0;
}

static int TestSpecialModeTables() {
    CHECK(CgSlots().size() == 168, "CG mode keeps 14x12 slots");
    CHECK(CgSlots()[2].size() == 2 && CgSlots()[2][1] == 100301,
          "CG variants preserve reference order");
    CHECK(SceneReplaySlots().size() == 24 && SceneReplaySlots()[0].unlockFlag == 80 &&
          SceneReplaySlots()[23].unlockFlag == 322,
          "scene replay keeps all 24 unlock flags");
    CHECK(BgmSlots().size() == 63 && BgmSlots()[0] == 4 && BgmSlots()[62] == 0x41,
          "music mode keeps all 63 reference tracks");
    CHECK(VoicePreferenceGroup(2) == 1 && VoicePreferenceGroup(1) == 2 &&
          VoicePreferenceGroup(36) == 8 && VoicePreferenceGroup(35) == 9,
          "character voice preference mapping");
    CHECK(MoveGridCursor(0, 12, 4, -1, 0) == 3 &&
          MoveGridCursor(3, 12, 4, 1, 0) == 0 &&
          MoveGridCursor(10, 12, 4, 0, 1) == 2,
          "special grid wraps safely");

    TestHost host;
    Script sc;
    host.replayMode = 24;
    CHECK(Funcs::Call(host, sc, 0xde, sc.args), "replay opcode continues");
    CHECK(host.lastPushedInt == 24, "replay opcode preserves 1..24 index");
    return 0;
}

static int TestAudioRing() {
    SpscByteRing<16> ring;
    uint8_t input[24];
    for (int i = 0; i < 24; ++i) input[i] = (uint8_t)i;
    uint8_t output[24] = {};

    CHECK(ring.Write(input, 12) == 12, "audio ring initial write");
    CHECK(ring.Available() == 12 && ring.Free() == 4, "audio ring initial counts");
    CHECK(ring.Read(output, 8) == 8, "audio ring partial read");
    CHECK(std::memcmp(output, input, 8) == 0, "audio ring initial order");

    // 写指针跨数组尾部，读出时也跨尾部；顺序应保持为 8..21。
    CHECK(ring.Write(input + 12, 10) == 10, "audio ring wrapped write");
    CHECK(ring.Available() == 14, "audio ring wrapped count");
    CHECK(ring.Read(output, sizeof(output)) == 14, "audio ring wrapped read");
    for (int i = 0; i < 14; ++i)
        CHECK(output[i] == (uint8_t)(i + 8), "audio ring wrapped order");

    CHECK(ring.Write(input, 24) == 16, "audio ring full clamps write");
    CHECK(ring.Write(input, 1) == 0, "audio ring rejects write when full");
    CHECK(ring.Read(output, 24) == 16, "audio ring full read");
    CHECK(ring.Available() == 0 && ring.Free() == 16, "audio ring empty counts");

    // 压力覆盖 acquire/release 发布顺序和数千次绕回。Producer/Consumer
    // 各自只有一个线程，和 Switch 的引擎解码线程/SDL 回调模型一致。
    SpscByteRing<4096> concurrent;
    constexpr size_t total = 2 * 1024 * 1024;
    std::atomic<bool> corrupt{false};
    std::thread producer([&]() {
        uint8_t block[257];
        size_t produced = 0;
        while (produced < total) {
            const size_t want = std::min(sizeof(block), total - produced);
            for (size_t i = 0; i < want; ++i)
                block[i] = (uint8_t)(((produced + i) * 131u + 17u) & 0xffu);
            const size_t n = concurrent.Write(block, want);
            produced += n;
            if (!n) std::this_thread::yield();
        }
    });
    uint8_t block[193];
    size_t consumed = 0;
    while (consumed < total) {
        const size_t n = concurrent.Read(block, std::min(sizeof(block), total - consumed));
        if (!n) {
            std::this_thread::yield();
            continue;
        }
        for (size_t i = 0; i < n; ++i) {
            if (block[i] != (uint8_t)(((consumed + i) * 131u + 17u) & 0xffu))
                corrupt.store(true, std::memory_order_relaxed);
        }
        consumed += n;
    }
    producer.join();
    CHECK(!corrupt.load(std::memory_order_relaxed) && concurrent.Available() == 0,
          "audio ring concurrent SPSC order");
    return 0;
}

static int TestVoiceArgumentMapping() {
    TestHost host;
    Script sc;

    host.voiceLabel = 1002;
    sc.PushInt(5, 3, 12);   // chr
    sc.PushInt(5, 3, 220);  // volume
    sc.PushInt(5, 3, 1);    // loop
    sc.PushInt(5, 3, 9);    // channel
    sc.PushInt(5, 3, 345);  // voice id
    CHECK(Funcs::Call(host, sc, 0x8a, sc.args), "VV continues");
    CHECK(host.voicePlayCount == 1, "VV played once");
    CHECK(host.voicePlayLabel == 1002, "VV current label");
    CHECK(host.voicePlayId == 345, "VV voice id");
    CHECK(host.voicePlayChr == 12, "VV character");
    CHECK(host.voicePlayVolume == 220, "VV volume");
    CHECK(host.voicePlayLoop, "VV loop");
    CHECK(host.voicePlayChannel == 9, "VV channel");

    sc.args.clear();
    sc.PushInt(5, 3, 7);    // chr
    sc.PushInt(5, 3, 678);  // voice id
    sc.PushInt(5, 3, 1003); // label
    sc.PushInt(5, 3, 180);  // volume
    sc.PushInt(5, 3, 0);    // loop
    sc.PushInt(5, 3, 8);    // channel
    CHECK(Funcs::Call(host, sc, 0x8b, sc.args), "VX continues");
    CHECK(host.voicePlayCount == 2, "VX played once");
    CHECK(host.voicePlayLabel == 1003, "VX explicit label");
    CHECK(host.voicePlayId == 678, "VX voice id");
    CHECK(host.voicePlayChr == 7, "VX character");
    CHECK(host.voicePlayVolume == 180, "VX volume");
    CHECK(!host.voicePlayLoop, "VX no loop");
    CHECK(host.voicePlayChannel == 8, "VX channel");
    return 0;
}

static int TestCharacterOpcodeMapping() {
    TestHost host;
    Script sc;

    for (int v : {7, 12, 3, 0, 0, 24}) sc.PushInt(5, 3, v);
    CHECK(!Funcs::Call(host, sc, 0x9a, sc.args), "C yields for transition");
    CHECK(host.charAddId == 7 && host.charAddNo == 12 && host.charAddPos == 3,
          "C maps id/no/pos");
    CHECK(host.charUpdateCount == 1 && host.charUpdateFrames == 24,
          "C commits queued characters");

    sc.args.clear();
    for (int v : {8, 13, 4, 0, 0}) sc.PushInt(5, 3, v);
    CHECK(Funcs::Call(host, sc, 0x9b, sc.args), "CW continues while batching");
    CHECK(host.charAddId == 8 && host.charAddNo == 13 && host.charAddPos == 4,
          "CW maps id/no/pos");
    CHECK(host.charUpdateCount == 1, "CW does not commit the batch");

    sc.args.clear();
    for (int v : {8, 0, 18}) sc.PushInt(5, 3, v);
    CHECK(!Funcs::Call(host, sc, 0x9c, sc.args), "CR yields for transition");
    CHECK(host.charRemoveCount == 1 && host.charRemoveId == 8,
          "CR removes by character id");
    CHECK(host.charUpdateCount == 2 && host.charUpdateFrames == 18,
          "CR commits queued characters");

    sc.args.clear();
    sc.PushInt(5, 3, 7);
    CHECK(Funcs::Call(host, sc, 0x9d, sc.args), "CRW continues while batching");
    CHECK(host.charRemoveCount == 2 && host.charRemoveId == 7,
          "CRW removes by character id");
    CHECK(host.charUpdateCount == 2, "CRW does not commit the batch");
    return 0;
}

static int TestCharacterLifecycle() {
    SceneState scene;
    CharacterVisualState visuals[kMaxChars];
    ResetCharacterVisuals(visuals);

    // 期望列表必须同时保证角色 id 与位置唯一。
    scene.AddOrUpdateChar(4, 10, 1);
    scene.AddOrUpdateChar(7, 20, 2);
    CHECK(scene.FindChar(1) && scene.FindChar(1)->id == 4,
          "desired character stored by position");
    CHECK(scene.FindCharById(7) && scene.FindCharById(7)->pos == 2,
          "desired character found by id");
    scene.AddOrUpdateChar(4, 11, 3);
    CHECK(!scene.FindChar(1) && scene.FindChar(3) && scene.FindChar(3)->no == 11,
          "same character moves without leaving old position");
    scene.AddOrUpdateChar(8, 30, 2);
    CHECK(!scene.FindCharById(7) && scene.FindChar(2)->id == 8,
          "new character replaces occupied position");
    scene.RemoveCharById(4);
    CHECK(!scene.FindCharById(4) && !scene.FindChar(3),
          "CR removes desired character by id");

    scene.ClearChars();
    scene.AddOrUpdateChar(4, 1, 1);   // CW queue
    scene.AddOrUpdateChar(7, 2, 2);   // CW queue
    CHECK(!visuals[1].show && !visuals[2].show,
          "CW batch is invisible before commit");
    CommitCharacterVisuals(scene, visuals, 30);
    CHECK(visuals[1].show && visuals[1].id == 4 && visuals[1].pos == 1,
          "commit uses fixed position slot for first character");
    CHECK(visuals[2].show && visuals[2].id == 7 && visuals[2].pos == 2,
          "commit uses fixed position slot for second character");
    CHECK(visuals[1].alpha == 0.0f && visuals[1].targetAlpha == 1.0f,
          "timed commit starts fade in");
    AdvanceCharacterVisuals(visuals, 0.5f);
    CHECK(visuals[1].alpha > 0.999f && visuals[2].alpha > 0.999f,
          "fade in reaches target");

    scene.RemoveCharById(4);          // CRW queue
    CHECK(visuals[1].show && visuals[1].id == 4,
          "CRW leaves current visual until commit");
    CommitCharacterVisuals(scene, visuals, 30);
    CHECK(visuals[1].show && visuals[1].targetAlpha == 0.0f,
          "CR commit starts fade out");
    CHECK(visuals[2].show && visuals[2].targetAlpha == 1.0f,
          "CR commit preserves remaining character");
    AdvanceCharacterVisuals(visuals, 0.5f);
    CHECK(!visuals[1].show && visuals[1].id == -1,
          "finished fade out releases old visual slot");

    // 同一角色换位置时，旧位置淡出、新位置淡入；数组下标始终等于 pos。
    scene.AddOrUpdateChar(7, 3, 6);
    CHECK(visuals[2].show && !visuals[6].show,
          "queued move does not alter current visual");
    CommitCharacterVisuals(scene, visuals, 15);
    CHECK(visuals[2].targetAlpha == 0.0f && visuals[6].show &&
          visuals[6].id == 7 && visuals[6].targetAlpha == 1.0f,
          "committed move transitions old and new positions");
    AdvanceCharacterVisuals(visuals, 0.25f);
    CHECK(!visuals[2].show && visuals[6].alpha > 0.999f,
          "committed move finishes without stale portrait");

    // B/V 的 keepChar=false 会先清空期望列表，0 帧时画面也必须立即清空。
    scene.ClearChars();
    CommitCharacterVisuals(scene, visuals, 0);
    CHECK(!visuals[6].show && visuals[6].id == -1,
          "background clear immediately removes all portraits");
    return 0;
}

#ifdef WA2_DIAG_TEXT_FIT
static std::string RepeatText(const std::string& glyph, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += glyph;
    return out;
}

static int TestDialogueTextFit() {
    const int bodyWidth = 28 * 28;
    const int safeHeight = 141;

    DialogueTextLayout one = WrapDialogueText(RepeatText("界", 28), 28, bodyWidth);
    CHECK(one.columns == 28 && one.lines.size() == 1,
          "reference layout keeps 28 glyphs on one line");
    CHECK(one.lines[0].charCount == 28 && one.rawCharCount == 28,
          "UTF-8 glyphs count as codepoints, not bytes");

    DialogueTextLayout wrapped = WrapDialogueText(RepeatText("界", 29), 28, bodyWidth);
    CHECK(wrapped.lines.size() == 2 && wrapped.lines[1].firstChar == 28,
          "29th glyph starts a stable second line");
    CHECK(wrapped.lines[1].text == "界" && wrapped.lines[1].charCount == 1,
          "UTF-8 line split preserves complete glyph bytes");

    DialogueTextLayout breaks = WrapDialogueText("\n甲\n\n乙", 28, bodyWidth);
    CHECK(breaks.lines.size() == 2 && breaks.rawCharCount == 5,
          "leading and repeated explicit breaks do not create blank rows");
    CHECK(breaks.lines[0].firstChar == 1 && breaks.lines[1].firstChar == 4,
          "explicit breaks retain raw typewriter offsets");
    CHECK(DialogueLineVisibleChars(breaks.lines[0], 1) == 0 &&
          DialogueLineVisibleChars(breaks.lines[0], 2) == 1,
          "first visible glyph respects an ignored leading break");
    CHECK(DialogueLineVisibleChars(breaks.lines[1], 4) == 0 &&
          DialogueLineVisibleChars(breaks.lines[1], 5) == 1,
          "later line never appears before its raw offset");

    DialogueTextLayout normal = FitDialogueText(RepeatText("字", 84),
                                                bodyWidth, safeHeight);
    CHECK(normal.fontSize == 28 && normal.lines.size() == 3 && normal.fits,
          "one-to-three line dialogue stays at reference 28px");

    DialogueTextLayout fourLines = FitDialogueText(RepeatText("字", 112),
                                                   bodyWidth, safeHeight);
    CHECK(fourLines.fontSize == 26 && fourLines.lines.size() == 4,
          "four-line dialogue shrinks one step to clear the frame border");
    CHECK(fourLines.PixelHeight() == 140 && fourLines.PixelHeight() <= safeHeight,
          "adaptive four-line layout remains inside the safe height");

    DialogueTextLayout longLine = FitDialogueText(RepeatText("字", 140),
                                                  bodyWidth, safeHeight);
    CHECK(longLine.fontSize == 22 && longLine.lines.size() == 4 && longLine.fits,
          "long translated dialogue selects the largest fitting size");

    DialogueTextLayout fiveExplicit = FitDialogueText("甲\n乙\n丙\n丁\n戊",
                                                      bodyWidth, safeHeight);
    CHECK(fiveExplicit.fontSize == 20 && fiveExplicit.lines.size() == 5 &&
          fiveExplicit.fits,
          "five explicit rows shrink enough to stay inside the text box");

    DialogueTextLayout pathological = FitDialogueText(RepeatText("字", 1000),
                                                       bodyWidth, safeHeight);
    CHECK(pathological.fontSize == 16 && !pathological.fits,
          "pathological text is reported after reaching readability floor");

    DialogueTextLayout novel = FitDialogueText(RepeatText("字", 185),
                                               39 * 28, 638);
    CHECK(novel.fontSize == 28 && novel.columns == 39 && novel.lines.size() == 5,
          "novel mode wraps at the reference 39-column grid");
    CHECK(novel.fits && novel.PixelHeight() == 192,
          "long novel paragraph remains at 28px inside the full-screen mask");

    std::string combined = "第一页";
    std::vector<std::string> pages = {"第一页"};
    const int appended = AppendDialoguePages("第二页", combined, pages);
    CHECK(appended == 1 && pages.size() == 2,
          "SetMessage append creates a new page index");
    CHECK(pages[0] == "第一页" && pages[1] == "第二页",
          "SetMessage append never merges text into the visible page");
    CHECK(combined == "第一页\\k第二页",
          "combined dialogue retains reference backlog page marker");

    const int nested = AppendDialoguePages("第三页\\k第四页", combined, pages);
    CHECK(nested == 2 && pages.size() == 4 && pages[2] == "第三页" &&
          pages[3] == "第四页",
          "embedded page markers are preserved while appending");
    return 0;
}

#ifdef WA2_DIAG_TEXT_REFLOW
static int TestDialogueTextReflow() {
    const int bodyWidth = 920;
    const int safeHeight = 141;
    auto fixedCell = [](const std::string&, int size) { return size; };

    DialogueTextLayout full = WrapDialogueTextMeasured(
        RepeatText("界", 32), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(full.lines.size() == 1 && full.lines[0].charCount == 32,
          "920px dialogue uses all 32 full-width glyph slots at 28px");
    CHECK(full.lines[0].pixelWidth == 896 && full.maxLineWidth == 896,
          "pixel layout records the renderer advance instead of a 28-column cap");

    DialogueTextLayout edge = WrapDialogueTextMeasured(
        RepeatText("界", 33), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(edge.lines.size() == 2 && edge.lines[0].charCount == 32 &&
          edge.lines[1].charCount == 1,
          "33rd full-width glyph wraps only at the real right boundary");

    const std::string softText = RepeatText("甲", 20) + "\n" + RepeatText("乙", 20);
    DialogueTextLayout soft = WrapDialogueTextMeasured(
        softText, 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(soft.lines.size() == 2 && soft.lines[0].charCount == 32 &&
          soft.lines[1].charCount == 8,
          "single script newline is reflowed instead of wasting horizontal space");
    CHECK(soft.rawCharCount == 41 && soft.lines[0].rawCharIndices[20] == 21 &&
          soft.lines[1].firstChar == 33,
          "soft newline keeps exact raw typewriter offsets across wrapped lines");
    CHECK(DialogueLineVisibleChars(soft.lines[0], 20) == 20 &&
          DialogueLineVisibleChars(soft.lines[0], 21) == 20 &&
          DialogueLineVisibleChars(soft.lines[0], 22) == 21,
          "typewriter consumes a soft newline without revealing the next glyph early");

    DialogueTextLayout paragraph = WrapDialogueTextMeasured(
        "甲乙\n\n丙丁", 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(paragraph.lines.size() == 2 && paragraph.lines[0].text == "甲乙" &&
          paragraph.lines[1].text == "丙丁" && paragraph.lines[1].firstChar == 4,
          "consecutive newlines remain an intentional paragraph boundary");

    auto proportional = [](const std::string& glyph, int size) {
        return glyph == "i" ? size / 2 : size;
    };
    DialogueTextLayout mixed = WrapDialogueTextMeasured(
        RepeatText("i", 40) + RepeatText("界", 12), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, proportional);
    CHECK(mixed.lines.size() == 1 && mixed.lines[0].charCount == 52 &&
          mixed.maxLineWidth == 896,
          "proportional glyph advances use remaining width that a character grid loses");

    DialogueTextLayout normal = FitDialogueTextMeasured(
        RepeatText("字", 96), bodyWidth, safeHeight,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(normal.fontSize == 28 && normal.lines.size() == 3 && normal.fits,
          "three full 920px rows keep the reference 28px size");

    DialogueTextLayout fourLines = FitDialogueTextMeasured(
        RepeatText("字", 112), bodyWidth, safeHeight,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(fourLines.fontSize == 26 && fourLines.lines.size() == 4 &&
          fourLines.PixelHeight() == 140 && fourLines.fits,
          "four-row reflow still selects the largest vertically safe size");

    for (const DialogueTextLine& line : fourLines.lines) {
        CHECK(line.pixelWidth <= bodyWidth,
              "every reflowed line stays inside the measured 920px body");
    }
    return 0;
}

#ifdef WA2_DIAG_TEXT_SAFE_WIDTH
static int TestDialogueTextSafeWidth() {
    const int bodyWidth = 28 * 28;
    const int safeHeight = 141;
    auto fixedCell = [](const std::string&, int size) { return size; };

    DialogueTextLayout full = WrapDialogueTextMeasured(
        RepeatText("界", 28), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(full.lines.size() == 1 && full.lines[0].charCount == 28 &&
          full.maxLineWidth == bodyWidth,
          "F4 keeps exactly 28 full-width glyphs inside the reference body");
    CHECK(278 + full.maxLineWidth == 1062,
          "F4 measured body ends at the reference x=1062 right boundary");

    DialogueTextLayout edge = WrapDialogueTextMeasured(
        RepeatText("界", 29), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(edge.lines.size() == 2 && edge.lines[0].charCount == 28 &&
          edge.lines[1].charCount == 1,
          "F4 wraps the 29th full-width glyph before the decorated UI area");

    const std::string softText = RepeatText("甲", 20) + "\n" + RepeatText("乙", 20);
    DialogueTextLayout soft = WrapDialogueTextMeasured(
        softText, 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(soft.lines.size() == 2 && soft.lines[0].charCount == 28 &&
          soft.lines[1].charCount == 12,
          "F4 fills the safe row across a single script newline");
    CHECK(soft.lines[0].rawCharIndices[20] == 21 &&
          DialogueLineVisibleChars(soft.lines[0], 21) == 20 &&
          DialogueLineVisibleChars(soft.lines[0], 22) == 21,
          "F4 soft reflow retains exact typewriter progress");

    auto proportional = [](const std::string& glyph, int size) {
        return glyph == "i" ? size / 2 : size;
    };
    DialogueTextLayout mixed = WrapDialogueTextMeasured(
        RepeatText("i", 40) + RepeatText("界", 8), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, proportional);
    CHECK(mixed.lines.size() == 1 && mixed.maxLineWidth == bodyWidth,
          "narrow glyphs may fill but never exceed the 784px body");

    DialogueTextLayout normal = FitDialogueTextMeasured(
        RepeatText("字", 84), bodyWidth, safeHeight,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(normal.fontSize == 28 && normal.lines.size() == 3 && normal.fits,
          "three reference-width rows stay at 28px");
    for (const DialogueTextLine& line : normal.lines) {
        CHECK(line.pixelWidth <= bodyWidth,
              "every F4 line remains inside the reference UI width");
    }
    return 0;
}

#ifdef WA2_DIAG_TEXT_SNOW_SAFE
static int TestDialogueTextSnowSafe() {
    const int bodyWidth = 24 * 28;
    const int bodyRight = 278 + bodyWidth;
    const int shadowPad = 2;
    const int snowLeft = 972;
    const int safeHeight = 141;
    auto fixedCell = [](const std::string&, int size) { return size; };

    DialogueTextLayout full = WrapDialogueTextMeasured(
        RepeatText("界", 24), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(full.lines.size() == 1 && full.lines[0].charCount == 24 &&
          full.maxLineWidth == bodyWidth,
          "F5 keeps exactly 24 full-width glyphs inside the snow-safe body");
    CHECK(bodyRight == 950 && bodyRight + shadowPad == 952 &&
          snowLeft - (bodyRight + shadowPad) == 20,
          "F5 leaves a 20px gap between glyph shadow and visible right snowflake");

    DialogueTextLayout edge = WrapDialogueTextMeasured(
        RepeatText("界", 25), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(edge.lines.size() == 2 && edge.lines[0].charCount == 24 &&
          edge.lines[1].charCount == 1,
          "F5 wraps the 25th full-width glyph before the snowflake margin");

    const std::string softText = RepeatText("甲", 20) + "\n" + RepeatText("乙", 20);
    DialogueTextLayout soft = WrapDialogueTextMeasured(
        softText, 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(soft.lines.size() == 2 && soft.lines[0].charCount == 24 &&
          soft.lines[1].charCount == 16,
          "F5 still reflows a single script newline inside the shorter body");
    CHECK(soft.lines[0].rawCharIndices[20] == 21 &&
          DialogueLineVisibleChars(soft.lines[0], 21) == 20 &&
          DialogueLineVisibleChars(soft.lines[0], 22) == 21,
          "F5 preserves typewriter offsets while reflowing the shorter row");

    auto proportional = [](const std::string& glyph, int size) {
        return glyph == "i" ? size / 2 : size;
    };
    DialogueTextLayout mixed = WrapDialogueTextMeasured(
        RepeatText("i", 32) + RepeatText("界", 8), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, proportional);
    CHECK(mixed.lines.size() == 1 && mixed.maxLineWidth == bodyWidth,
          "proportional glyphs fill but never cross the 672px snow-safe body");

    DialogueTextLayout normal = FitDialogueTextMeasured(
        RepeatText("字", 72), bodyWidth, safeHeight,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(normal.fontSize == 28 && normal.lines.size() == 3 && normal.fits,
          "three snow-safe rows stay at 28px");
    for (const DialogueTextLine& line : normal.lines) {
        CHECK(line.pixelWidth <= bodyWidth,
              "every F5 line remains before the snowflake margin");
    }
    return 0;
}

#ifdef WA2_RELEASE_BUILD
static int TestDialogueTextReleaseWidth() {
    const int bodyWidth = 25 * 28;
    const int bodyRight = 278 + bodyWidth;
    const int safeHeight = 141;
    auto fixedCell = [](const std::string&, int size) { return size; };

    DialogueTextLayout full = WrapDialogueTextMeasured(
        RepeatText("界", 25), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(full.lines.size() == 1 && full.lines[0].charCount == 25 &&
          full.maxLineWidth == bodyWidth,
          "release keeps exactly 25 full-width glyphs on one row");
    CHECK(bodyRight == 978,
          "release dialogue body ends at the user-confirmed x=978 boundary");

    DialogueTextLayout edge = WrapDialogueTextMeasured(
        RepeatText("界", 26), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(edge.lines.size() == 2 && edge.lines[0].charCount == 25 &&
          edge.lines[1].charCount == 1,
          "release wraps the 26th full-width glyph at the final boundary");

    const std::string softText = RepeatText("甲", 20) + "\n" + RepeatText("乙", 20);
    DialogueTextLayout soft = WrapDialogueTextMeasured(
        softText, 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(soft.lines.size() == 2 && soft.lines[0].charCount == 25 &&
          soft.lines[1].charCount == 15,
          "release soft-reflows a single script newline across the 25-cell row");
    CHECK(soft.lines[0].rawCharIndices[20] == 21 &&
          DialogueLineVisibleChars(soft.lines[0], 21) == 20 &&
          DialogueLineVisibleChars(soft.lines[0], 22) == 21,
          "release preserves typewriter offsets across soft reflow");

    auto proportional = [](const std::string& glyph, int size) {
        return glyph == "i" ? size / 2 : size;
    };
    DialogueTextLayout mixed = WrapDialogueTextMeasured(
        RepeatText("i", 34) + RepeatText("界", 8), 28, bodyWidth,
        DialogueNewlinePolicy::ReflowSingle, proportional);
    CHECK(mixed.lines.size() == 1 && mixed.maxLineWidth == bodyWidth,
          "proportional glyphs fill but never exceed the 700px release body");

    DialogueTextLayout normal = FitDialogueTextMeasured(
        RepeatText("字", 75), bodyWidth, safeHeight,
        DialogueNewlinePolicy::ReflowSingle, fixedCell);
    CHECK(normal.fontSize == 28 && normal.lines.size() == 3 && normal.fits,
          "three final-width rows stay at 28px");
    for (const DialogueTextLine& line : normal.lines) {
        CHECK(line.pixelWidth <= bodyWidth,
              "every release line remains inside the 700px body");
    }
    return 0;
}
#endif
#endif
#endif
#endif
#endif

#ifdef WA2_DIAG_C4_RELATIVE_TIMER
static int TestC4RelativeTimer() {
    TestHost host;
    Script sc;

    // 目标时间点 5400 ms，StartTimer 后已经过 1250 ms，只应再等 4150 ms。
    host.elapsedTimerMs = 1250;
    sc.PushInt(5, 3, 5400);
    CHECK(!Funcs::Call(host, sc, 0xc4, sc.args), "C4 yields");
    CHECK(host.waits == 1, "C4 schedules one residual wait");
    CHECK(host.lastWaitMs == 4150.0f, "C4 waits only until absolute timer target");

    // 已经越过目标时间点时，参考实现不会再启动新的等待。
    sc.args.clear();
    host.elapsedTimerMs = 6000;
    host.lastWaitMs = -1.0f;
    sc.PushInt(5, 3, 5400);
    CHECK(!Funcs::Call(host, sc, 0xc4, sc.args), "elapsed C4 still yields one VM tick");
    CHECK(host.waits == 1, "elapsed C4 does not schedule another wait");
    CHECK(host.lastWaitMs < 0.0f, "elapsed C4 leaves timer inactive");
    return 0;
}
#endif

#ifdef WA2_DIAG_C4_CLICK_ADVANCE
static int TestC4ClickAdvanceGate() {
    // 第一次确认发生在打字机仍工作时，只应完整显示当前文字，不能解除定时器。
    CHECK(!ShouldReleaseC4ForAdvance(true, true, true, false),
          "first click keeps C4 timer while text is typing");
    // 当前框已经完整并进入等待确认后，下一次确认应立即解除 C4。
    CHECK(ShouldReleaseC4ForAdvance(true, true, false, true),
          "second click releases C4 timer after text completes");
    // 普通 Wait/语音/SE 等待不能被这条规则误解除。
    CHECK(!ShouldReleaseC4ForAdvance(true, false, false, true),
          "non-C4 timer is not released by dialogue advance");
    CHECK(!ShouldReleaseC4ForAdvance(false, true, false, true),
          "C4 timer needs an actual confirm click");
    return 0;
}
#endif

#ifdef WA2_DIAG_C4_SYNC_CLICK_ADVANCE
static int TestC4SynchronousClickAdvance() {
    // 打字机阶段的第一次确认只完整显示当前文字。
    CHECK(!ShouldSynchronouslyContinueAfterConfirm(true, true, false, false),
          "typing click does not synchronously continue script");
    // \k 分段之间的确认只切到下一段。
    CHECK(!ShouldSynchronouslyContinueAfterConfirm(true, false, true, true),
          "intermediate segment click does not continue script");
    // 最后一段完整显示后的第二次确认，与参考实现一样同步继续脚本。
    CHECK(ShouldSynchronouslyContinueAfterConfirm(true, false, true, false),
          "final segment confirm synchronously continues script");
    // 自动或残留状态不能伪装成一次真实确认输入。
    CHECK(!ShouldSynchronouslyContinueAfterConfirm(false, false, true, false),
          "synchronous continuation needs an actual confirm click");
    return 0;
}
#endif

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

    if (TestAudioRing() != 0) return 1;
    printf("[ok] audio SPSC ring (%d checks)\n", g_checks);

    if (TestScriptRun() != 0) return 1;
    printf("[ok] script vm full run (%d checks)\n", g_checks);

    if (TestVoiceArgumentMapping() != 0) return 1;
    printf("[ok] voice argument mapping (%d checks)\n", g_checks);

    if (TestCharacterOpcodeMapping() != 0) return 1;
    printf("[ok] character opcode mapping (%d checks)\n", g_checks);

    if (TestCharacterLifecycle() != 0) return 1;
    printf("[ok] character desired/visual lifecycle (%d checks)\n", g_checks);

    if (TestSpecialModeTables() != 0) return 1;
    printf("[ok] original special-mode tables and replay index (%d checks)\n", g_checks);

#ifdef WA2_DIAG_TEXT_FIT
    if (TestDialogueTextFit() != 0) return 1;
    printf("[ok] adaptive dialogue text layout (%d checks)\n", g_checks);
#ifdef WA2_DIAG_TEXT_REFLOW
    if (TestDialogueTextReflow() != 0) return 1;
    printf("[ok] measured dialogue text reflow (%d checks)\n", g_checks);
#ifdef WA2_DIAG_TEXT_SAFE_WIDTH
    if (TestDialogueTextSafeWidth() != 0) return 1;
    printf("[ok] reference-width dialogue text safety (%d checks)\n", g_checks);
#ifdef WA2_DIAG_TEXT_SNOW_SAFE
    if (TestDialogueTextSnowSafe() != 0) return 1;
    printf("[ok] snow-safe dialogue text boundary (%d checks)\n", g_checks);
#ifdef WA2_RELEASE_BUILD
    if (TestDialogueTextReleaseWidth() != 0) return 1;
    printf("[ok] production dialogue text boundary (%d checks)\n", g_checks);
#endif
#endif
#endif
#endif
#endif

#ifdef WA2_DIAG_C4_RELATIVE_TIMER
    if (TestC4RelativeTimer() != 0) return 1;
    printf("[ok] C4 relative timer (%d checks)\n", g_checks);
#endif

#ifdef WA2_DIAG_C4_CLICK_ADVANCE
    if (TestC4ClickAdvanceGate() != 0) return 1;
    printf("[ok] C4 two-stage click advance (%d checks)\n", g_checks);
#endif

#ifdef WA2_DIAG_C4_SYNC_CLICK_ADVANCE
    if (TestC4SynchronousClickAdvance() != 0) return 1;
    printf("[ok] C4 synchronous click advance (%d checks)\n", g_checks);
#endif

    if (TestSaveRoundtrip() != 0) return 1;
    printf("[ok] save roundtrip (%d checks)\n", g_checks);

    printf("ALL TESTS PASSED (%d checks, %d failed)\n", g_checks, g_failed);
    LogFlush();
    return g_failed == 0 ? 0 : 1;
}
