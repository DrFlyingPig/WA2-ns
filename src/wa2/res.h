// res.h — 资源命名与读取(核心层,无 SDL 依赖)
//
// PC 版资源命名规则(大小写不敏感,统一小写存放):
//   背景   B{bg:04d}{no}{time}.tga      bg=编号/10, no=编号%10, time=时间段 0/1/2
//   CG     v{id:06d}.tga                CG 展示
//   H 场景 h{id:06d}.tga
//   立绘   {prefix}{no:06d}.tga         prefix 来自 CharDict
//   遮罩   f0{id:03d}.bmp               过渡掩码(BMP 灰度)
//   BGM    bgm_{id:03d}.ogg 或 bgm_{id:03d}_a.ogg + bgm_{id:03d}_b.ogg(前奏+循环)
//   SE     se_{id:04d}.wav / .ogg
//   语音   {label:04d}_{id:04d}_{chr:02d}.ogg
#pragma once

#include "wa2.h"

namespace wa2 {

// 资源定位结果:归档内文件名(小写)或磁盘路径
struct ResLoc {
    std::string name;    // 小写资源名(含扩展名)
    bool found = false;
};

class Res {
public:
    // dataDir:游戏数据目录(散装文件的查找根)
    void SetDataDir(const std::string& dir) { dataDir_ = dir; }
    const std::string& dataDir() const { return dataDir_; }

    // 扫描数据目录,登记所有归档(*.pac/*.pak/*.lad 等)并缓存散装文件列表
    void ScanArchives();
    bool UsesPatchFont() const { return usePatchFont_; }

    // 统一读资源:散装补丁文件 → 归档表;返回原始字节。
    // PC 汉化补丁会把覆盖图放在 grp/ 等子目录，必须优先于 grp.pak。
    std::vector<uint8_t> Load(const std::string& lowerName);
    bool Exists(const std::string& lowerName);

    // ---------- 命名规则 ----------
    static std::string BgName(int id, int timeMode);        // B{...}.tga
    static std::string CgName(int id);                      // v{id:6}.tga
    static std::string HName(int id);                       // h{id:6}.tga
    static std::string CharName(int charId, int no);        // {prefix}{no:6}.tga
    static std::string MaskName(int id);                    // f0{id:3}.bmp
    static std::string BgmName(int id, bool loopPart);      // 主文件 / B 循环段
    static std::string BgmIntroName(int id);                // A 前奏段
    static std::string SeName(int id);                      // wav 优先,回退 ogg
    static std::string VoiceName(int label, int id, int chr);

    // 依次尝试候选名,返回第一个存在的
    ResLoc Find(const std::vector<std::string>& candidates);

private:
    std::string dataDir_;
    std::unordered_map<std::string, std::string> looseFiles_; // 小写文件名 -> 实际完整路径
    bool usePatchFont_ = false;
    bool ScanArchiveFile(const std::string& path);
};

} // namespace wa2
