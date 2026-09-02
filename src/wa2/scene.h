// scene.h — 场景状态数据模型(核心层;SDL 层负责把状态画出来)
#pragma once

#include "wa2.h"
#include "util.h"

namespace wa2 {

struct CharItem {
    int  id = -1;     // 角色编号(CharDict)
    int  no = 0;      // 立绘差分号
    int  pos = 0;     // 槽位(0-9,kCharPos 偏移)
    bool show = false;
};

struct BgInfo {
    std::string path;   // 当前背景资源名(小写)
    int id = -1;
    int x = 0, y = 0;   // 显示偏移
    int offset = 0;
    float sx = 1.0f, sy = 1.0f;
    int type = 0;       // 0=背景 1=CG 2=H
};

struct SelectItem {
    std::string text;
    int v1 = 0, v2 = 0, v3 = 0;
};

struct BacklogEntry {
    std::string name;
    std::string text;
};

class SceneState {
public:
    BgInfo bg;
    CharItem chars[kMaxChars];
    int timeMode = 0;
    std::string effectMode;      // 调色板 LUT 文件名(空 = 不用)
    std::vector<SelectItem> selectItems;
    std::vector<BacklogEntry> backlog;
    bool novelMode = false;
    bool eroMode = false;
    bool demoMode = false;
    int voiceLabel = 0;

    CharItem* FindChar(int pos) {
        for (auto& c : chars) if (c.show && c.pos == pos) return &c;
        return nullptr;
    }
    const CharItem* FindChar(int pos) const {
        for (const auto& c : chars) if (c.show && c.pos == pos) return &c;
        return nullptr;
    }
    const CharItem* FindCharById(int id) const {
        for (const auto& c : chars) if (c.show && c.id == id) return &c;
        return nullptr;
    }
    void AddOrUpdateChar(int id, int no, int pos) {
        if (pos < 0 || pos >= kMaxChars) return;
        // 参考实现的期望列表同时以角色 id 和位置保持唯一。先移除冲突项，
        // CW 可安全地为随后的 C/CR/BC 批量排队多个变化。
        for (auto& slot : chars) {
            if (slot.show && (slot.id == id || slot.pos == pos)) {
                slot = {};
            }
        }
        CharItem* c = nullptr;
        for (auto& slot : chars) {
            if (!slot.show) { c = &slot; break; }
        }
        if (!c) return;   // 唯一性成立时最多只有 kMaxChars 个有效位置
        c->id = id; c->no = no; c->pos = pos; c->show = true;
    }
    void RemoveCharById(int id) {
        // CR/CRW 的第一个参数是角色 id，不是画面位置。
        for (auto& c : chars) {
            if (c.show && c.id == id) { c = {}; return; }
        }
    }
    void RemoveCharAt(int pos) {
        for (auto& c : chars) if (c.show && c.pos == pos) { c = {}; return; }
    }
    void ClearChars() {
        for (auto& c : chars) c = {};
    }
    void AddBacklog(const std::string& name, const std::string& text) {
        backlog.push_back({name, text});
        if (backlog.size() > 200) backlog.erase(backlog.begin());
    }
    void Save(ByteBuf& out) const;
    bool Load(ByteReader& in);
};

} // namespace wa2
