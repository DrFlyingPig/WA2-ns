// state.cpp — 场景状态 / 配置 / 存档 的序列化
#include "sav.h"
#include <algorithm>

namespace wa2 {

static constexpr int32_t kMaxSavedBacklog = 200;

// ---------------- SceneState ----------------
void SceneState::Save(ByteBuf& out) const {
    out.Str(bg.path);
    out.I32(bg.id); out.I32(bg.x); out.I32(bg.y); out.I32(bg.offset);
    out.F32(bg.sx); out.F32(bg.sy); out.I32(bg.type);
    for (int i = 0; i < kMaxChars; i++) {
        out.I32(chars[i].id); out.I32(chars[i].no); out.I32(chars[i].pos); out.I32(chars[i].show ? 1 : 0);
    }
    out.I32(timeMode);
    out.Str(effectMode);
    out.I32(novelMode ? 1 : 0); out.I32(eroMode ? 1 : 0); out.I32(demoMode ? 1 : 0);
    out.I32(voiceLabel);
    out.I32((int32_t)backlog.size());
    for (const auto& b : backlog) { out.Str(b.name); out.Str(b.text); }
    // selectItems 是瞬时状态,不入档
}

bool SceneState::Load(ByteReader& in) {
    bg.path = in.Str();
    bg.id = in.I32(); bg.x = in.I32(); bg.y = in.I32(); bg.offset = in.I32();
    bg.sx = in.F32(); bg.sy = in.F32(); bg.type = in.I32();
    for (int i = 0; i < kMaxChars; i++) {
        chars[i].id = in.I32(); chars[i].no = in.I32(); chars[i].pos = in.I32();
        chars[i].show = in.I32() != 0;
    }
    timeMode = in.I32();
    effectMode = in.Str();
    novelMode = in.I32() != 0;
    eroMode = in.I32() != 0;
    demoMode = in.I32() != 0;
    voiceLabel = in.I32();
    int32_t n = in.I32();
    if (!in.Ok() || n < 0 || n > kMaxSavedBacklog) return false;
    backlog.clear();
    backlog.reserve((size_t)n);
    for (int32_t i = 0; i < n; i++) {
        BacklogEntry e;
        e.name = in.Str(); e.text = in.Str();
        backlog.push_back(e);
    }
    return in.Ok();
}

// ---------------- Config ----------------
static constexpr uint32_t kConfigExtensionMagic = 0x32474643u; // "CFG2"

void Config::Save(ByteBuf& out) const {
    // 前 24 字节保持 0.1.12 早期 config.bin 的布局，升级不会丢失用户音量。
    out.I32(textSpeed); out.I32(autoSpeed);
    out.I32(bgmVolume); out.I32(seVolume); out.I32(voiceVolume);
    out.I32(skipUnread ? 1 : 0);
    out.U32(kConfigExtensionMagic);
    out.U32(3);
    out.I32(autoDelayFrames); out.I32(masterVolume);
    out.I32(windowAlpha); out.I32(cgWindowAlpha); out.I32(novelWindowAlpha);
    out.I32(pageVoice ? 1 : 0); out.I32(eroVoice ? 1 : 0);
    out.I32(longPressSkip ? 1 : 0);
    for (uint8_t enabled : charVoice) out.U8(enabled ? 1 : 0);
    out.I32(fastWait ? 1 : 0);
    out.I32(confirmDefaultYes ? 1 : 0);
}
bool Config::Load(ByteReader& in) {
    Config loaded;
    textSpeed = in.I32(); autoSpeed = in.I32();
    bgmVolume = in.I32(); seVolume = in.I32(); voiceVolume = in.I32();
    skipUnread = in.I32() != 0;
    if (!in.Ok()) { *this = loaded; return false; }

    // 旧版文件到这里正好结束；新增设置使用原版默认值。
    if (in.Remaining() >= 8) {
        const size_t extension = in.Pos();
        const uint32_t magic = in.U32();
        const uint32_t version = in.U32();
        if (magic == kConfigExtensionMagic && version >= 2) {
            autoDelayFrames = in.I32(); masterVolume = in.I32();
            windowAlpha = in.I32(); cgWindowAlpha = in.I32(); novelWindowAlpha = in.I32();
            pageVoice = in.I32() != 0; eroVoice = in.I32() != 0;
            longPressSkip = in.I32() != 0;
            for (uint8_t& enabled : charVoice) enabled = in.U8() ? 1 : 0;
            if (version >= 3) {
                fastWait = in.I32() != 0;
                confirmDefaultYes = in.I32() != 0;
            }
            if (!in.Ok()) { *this = loaded; return false; }
        } else {
            // 未知尾部属于未来格式；保留已成功读取的旧字段。
            in.Seek(extension);
        }
    }

    textSpeed = std::clamp(textSpeed, 0, 3);
    autoSpeed = std::clamp(autoSpeed, 0, 3);
    autoDelayFrames = std::clamp(autoDelayFrames, 60, 600);
    masterVolume = std::clamp(masterVolume, 0, 255);
    bgmVolume = std::clamp(bgmVolume, 0, 255);
    seVolume = std::clamp(seVolume, 0, 255);
    voiceVolume = std::clamp(voiceVolume, 0, 255);
    windowAlpha = std::clamp(windowAlpha, 0, 256);
    cgWindowAlpha = std::clamp(cgWindowAlpha, 0, 256);
    novelWindowAlpha = std::clamp(novelWindowAlpha, 0, 256);
    return true;
}

// ---------------- SaveData ----------------
void SaveData::Reset() {
    meta = SaveMeta{};
    gameFlags.assign(kMaxGameFlags, 0);
    sysFlags.assign(kSysFlagCount, 0);
    engineBlock.clear();
}

void SaveData::Save(ByteBuf& out) const {
    out.U32(0x314D4157);  // "WAM1" 版本标记
    out.Str(meta.chapter);
    out.Str(meta.preview);
    out.U32(uint32_t(meta.timestamp));
    out.U32(uint32_t(meta.timestamp >> 32));
    size_t gf = gameFlags.size() < kMaxGameFlags ? gameFlags.size() : kMaxGameFlags;
    out.I32((int32_t)gf);
    for (size_t i = 0; i < gf; i++) out.I32(gameFlags[i]);
    size_t sf = sysFlags.size();
    out.I32((int32_t)sf);
    for (size_t i = 0; i < sf; i++) out.U8(sysFlags[i]);
    out.Bytes(engineBlock.data(), engineBlock.size());
}

bool SaveData::Load(ByteReader& in) {
    if (in.U32() != 0x314D4157) return false;
    meta.chapter = in.Str();
    meta.preview = in.Str();
    uint32_t lo = in.U32(), hi = in.U32();
    meta.timestamp = (uint64_t(hi) << 32) | lo;
    int32_t gf = in.I32();
    if (!in.Ok() || gf < 0 || gf > kMaxGameFlags) return false;
    gameFlags.assign((size_t)gf, 0);
    for (int32_t i = 0; i < gf; i++) gameFlags[i] = in.I32();
    int32_t sf = in.I32();
    if (!in.Ok() || sf < 0 || sf > kSysFlagCount) return false;
    sysFlags.assign((size_t)sf, 0);
    for (int32_t i = 0; i < sf; i++) sysFlags[i] = in.U8();
    engineBlock = in.Bytes();
    return in.Ok() && in.Remaining() == 0;
}

} // namespace wa2
