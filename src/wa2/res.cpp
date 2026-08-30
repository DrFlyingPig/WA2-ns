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
    // 登记散装文件(小写)
    looseFiles_.clear();
    for (const auto& name : ListDir(dataDir_))
        looseFiles_[ToLower(name)] = true;

    // 打开数据目录下所有可能的归档
    for (const auto& name : ListDir(dataDir_)) {
        std::string lower = ToLower(name);
        bool maybeArchive = false;
        static const char* kSufs[] = { ".pac", ".pak", ".lad", ".arc", ".dat" };
        for (const char* suf : kSufs) {
            size_t n = lower.size(), m = strlen(suf);
            if (n >= m && lower.compare(n - m, m, suf) == 0) { maybeArchive = true; break; }
        }
        if (maybeArchive)
            ScanArchiveFile(PathJoin(dataDir_, name));
    }
}

bool Res::ScanArchiveFile(const std::string& path) {
    Archive a;
    if (a.Open(path))
        return true;
    return false;
}

bool Res::Exists(const std::string& lowerName) {
    if (Archive::Has(lowerName)) return true;
    if (looseFiles_.count(lowerName)) return true;
    if (FileExists(PathJoin(dataDir_, lowerName))) return true;
    return false;
}

std::vector<uint8_t> Res::Load(const std::string& lowerName) {
    if (Archive::Has(lowerName))
        return Archive::LoadFile(lowerName);
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
std::string Res::SeName(int id) {
    return Fmt("se_%04d.wav", id);
}
std::string Res::VoiceName(int label, int id, int chr) {
    return Fmt("%04d_%04d_%02d.ogg", label, id, chr);
}

} // namespace wa2
