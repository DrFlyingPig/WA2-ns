// state.cpp — 场景状态 / 配置 / 存档 的序列化
#include "sav.h"

namespace wa2 {

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
    backlog.clear();
    for (int32_t i = 0; i < n; i++) {
        BacklogEntry e;
        e.name = in.Str(); e.text = in.Str();
        backlog.push_back(e);
    }
    return in.Ok();
}

// ---------------- Config ----------------
void Config::Save(ByteBuf& out) const {
    out.I32(textSpeed); out.I32(autoSpeed);
    out.I32(bgmVolume); out.I32(seVolume); out.I32(voiceVolume);
    out.I32(skipUnread ? 1 : 0);
}
bool Config::Load(ByteReader& in) {
    textSpeed = in.I32(); autoSpeed = in.I32();
    bgmVolume = in.I32(); seVolume = in.I32(); voiceVolume = in.I32();
    skipUnread = in.I32() != 0;
    return in.Ok();
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
    gameFlags.assign(gf > kMaxGameFlags ? kMaxGameFlags : gf, 0);
    for (int32_t i = 0; i < gf && i < kMaxGameFlags; i++) gameFlags[i] = in.I32();
    int32_t sf = in.I32();
    sysFlags.assign(sf > kSysFlagCount ? kSysFlagCount : sf, 0);
    for (int32_t i = 0; i < sf && i < kSysFlagCount; i++) sysFlags[i] = in.U8();
    engineBlock = in.Bytes();
    return in.Ok();
}

} // namespace wa2
