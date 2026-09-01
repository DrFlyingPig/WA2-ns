// res.cpp — 资源命名与读取实现
#include "res.h"
#include "util.h"
#include "archive.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace wa2 {

const std::map<int, std::string>& CharDict() {
    static const std::map<int, std::string> dict = {
        {0, "har"}, {1, "kaz"}, {2, "set"}, {3, "koh"}, {4, "izu"}, {5, "mar"},
        {10, "tak"}, {11, "ioo"}, {12, "chi"}, {13, "pap"}, {14, "mam"}, {15, "oto"},
        {16, "you"}, {17, "tan"}, {18, "shi"}, {19, "tom"}, {20, "sat"}, {21, "hon"},
        {22, "nak"}, {23, "say"}, {24, "aco"}, {25, "mih"}, {26, "mhh"}, {27, "ueh"},
        {28, "yos"}, {29, "tan"}, {30, "ham"}, {31, "mat"}, {32, "kiz"}, {33, "suz"},
        {34, "saw"}, {35, "miy"}, {36, "yan"}, {37, "mas"},
    };
    return dict;
}

void Res::ScanArchives() {
    Archive::Index().clear();
    usePatchFont_ = false;
    // 登记散装文件。CK-GAL 把中文系统 UI 放在 grp/ 中；这些文件与原版
    // grp.pak 同名，原 PC 引擎会优先使用它们，Switch 端也保持相同语义。
    looseFiles_.clear();
    const std::vector<std::string> files = ListDir(dataDir_);
    for (const auto& name : files)
        looseFiles_[ToLower(name)] = PathJoin(dataDir_, name);
    static const char* kLooseDirs[] = {"grp", "char", "bak", "bgm", "se", "voice"};
    for (const char* sub : kLooseDirs) {
        const std::string dir = PathJoin(dataDir_, sub);
        for (const auto& name : ListDir(dir))
            looseFiles_[ToLower(name)] = PathJoin(dir, name);
    }

    std::vector<std::string> baseArchives, patchArchives;
    // 基础包先加载；CK-GAL 脚本和配套 fon 字库最后加载，保证汉化覆盖原版。
    for (const auto& name : files) {
        std::string lower = ToLower(name);
        bool maybeArchive = false;
        static const char* kSufs[] = { ".pac", ".pak", ".lad", ".arc", ".dat" };
        for (const char* suf : kSufs) {
            size_t n = lower.size(), m = strlen(suf);
            if (n >= m && lower.compare(n - m, m, suf) == 0) { maybeArchive = true; break; }
        }
        if (!maybeArchive) continue;
        if (lower == "ck-gal.pak" || lower == "fon.pak") patchArchives.push_back(name);
        else baseArchives.push_back(name);
        if (lower == "ck-gal.pak") usePatchFont_ = true;
    }
    // 与 wa2-godot 的覆盖顺序一致：本篇 BGM → IC 数据 → 本篇其余数据。
    // IC 目录是 introductory chapter 的真实 PC 数据，漏扫会导致后续脚本/素材缺失。
    for (const auto& name : baseArchives)
        if (ToLower(name) == "bgm.pak") ScanArchiveFile(PathJoin(dataDir_, name));
    const std::string icDir = PathJoin(dataDir_, "IC");
    for (const auto& name : ListDir(icDir)) {
        const std::string lower = ToLower(name);
        const bool archive = lower.size() >= 4 &&
            (lower.compare(lower.size() - 4, 4, ".pak") == 0 ||
             lower.compare(lower.size() - 4, 4, ".pac") == 0);
        // mv*.pak 是视频容器，不是 PACK/LAC 文件表。
        if (archive && lower.rfind("mv", 0) != 0)
            ScanArchiveFile(PathJoin(icDir, name));
    }
    for (const auto& name : baseArchives)
        if (ToLower(name) != "bgm.pak") ScanArchiveFile(PathJoin(dataDir_, name));
    // 字库先于脚本覆盖；两者条目类型不同，但固定顺序便于日志审计。
    for (const auto& name : patchArchives)
        if (ToLower(name) == "fon.pak") ScanArchiveFile(PathJoin(dataDir_, name));
    for (const auto& name : patchArchives)
        if (ToLower(name) == "ck-gal.pak") ScanArchiveFile(PathJoin(dataDir_, name));

    if (usePatchFont_) {
        auto it = Archive::Index().find("1001.txt");
        Log(LogLevel::Info, "res: Chinese patch enabled; script overlay=%s",
            it != Archive::Index().end() ? it->second.pkgPath.c_str() : "(missing)");
    }
    Log(LogLevel::Info, "res: %u loose overrides indexed", (unsigned)looseFiles_.size());
}

bool Res::ScanArchiveFile(const std::string& path) {
    Archive a;
    if (a.Open(path))
        return true;
    return false;
}

bool Res::Exists(const std::string& lowerName) {
    const std::string key = ToLower(lowerName);
    if (looseFiles_.count(key)) return true;
    if (Archive::Has(key)) return true;
    if (FileExists(PathJoin(dataDir_, lowerName))) return true;
    return false;
}

std::vector<uint8_t> Res::Load(const std::string& lowerName) {
    const std::string key = ToLower(lowerName);
    auto loose = looseFiles_.find(key);
    if (loose != looseFiles_.end())
        return ReadFileAll(loose->second);
    if (Archive::Has(key))
        return Archive::LoadFile(key);
    return ReadFileAll(PathJoin(dataDir_, lowerName));
}

ResLoc Res::Find(const std::vector<std::string>& candidates) {
    ResLoc loc;
    for (const auto& c : candidates) {
        std::string lower = ToLower(c);
        if (Exists(lower)) { loc.name = lower; loc.found = true; return loc; }
    }
    return loc;
}

// ---------- 命名规则 ----------
static std::string Fmt(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string Res::BgName(int id, int timeMode) {
    return Fmt("b%04d%d%d.tga", id / 10, id % 10, timeMode);
}
std::string Res::CgName(int id) {
    return Fmt("v%06d.tga", id);
}
std::string Res::HName(int id) {
    return Fmt("h%06d.tga", id);
}
std::string Res::CharName(int charId, int no) {
    auto& dict = CharDict();
    auto it = dict.find(charId);
    const char* prefix = it != dict.end() ? it->second.c_str() : "";
    return Fmt("%s%06d.tga", prefix, no);
}
std::string Res::MaskName(int id) {
    return Fmt("f0%03d.bmp", id);
}
std::string Res::BgmName(int id, bool loopPart) {
    if (loopPart) return Fmt("bgm_%03d_b.ogg", id);
    return Fmt("bgm_%03d.ogg", id);
}
std::string Res::BgmIntroName(int id) {
    return Fmt("bgm_%03d_a.ogg", id);
}
std::string Res::SeName(int id) {
    return Fmt("se_%04d.wav", id);
}
std::string Res::VoiceName(int label, int id, int chr) {
    return Fmt("%04d_%04d_%02d.ogg", label, id, chr);
}

} // namespace wa2
