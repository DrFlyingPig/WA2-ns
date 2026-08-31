// script.cpp — .bnr 字节码虚拟机实现
#include "script.h"
#include "util.h"
#include "res.h"
#include "funcs.h"

#include <cstring>
#include <cstdio>

namespace wa2 {

// ---- Shift-JIS → UTF-8(表见 sjis_table.cpp,由 tools/gen_sjis.py 生成)----
namespace sjis {
// 返回写入 out 的字节数;无法识别时写 '?' 返回 1
int OneToUtf8(const uint8_t* src, size_t avail, char* out);
std::string ToUtf8(const uint8_t* data, size_t size);
} // namespace sjis

static const char kDefaultName[] = "\xe6\x98\xa5\xe5\xb8\x8c"; // 「春希」(STR_VAR 0 的默认取值)

void ParseScriptTexts(const std::vector<uint8_t>& data, std::vector<std::string>& out) {
    std::string all = sjis::ToUtf8(data.data(), data.size());
    out.clear();
    // 以逗号切分(文本本身不含换行;若出现 \n 保留)
    size_t start = 0;
    while (true) {
        size_t pos = all.find(',', start);
        if (pos == std::string::npos) {
            out.push_back(all.substr(start));
            break;
        }
        out.push_back(all.substr(start, pos - start));
        start = pos + 1;
    }
}

bool Script::Load(Res& res, const std::string& name, int point) {
    name_ = ToLower(name);
    points_.clear();
    entries_.clear();
    args.clear();
    exit_ = false;

    buf_ = res.Load(name_ + ".bnr");
    if (buf_.empty()) {
        Log(LogLevel::Error, "script: %s.bnr not found", name_.c_str());
        return false;
    }
    if (buf_.size() >= 12 && wa2::ReadU32(buf_.data()) == 0x5243534C) {
        uint32_t n = wa2::ReadU32(buf_.data() + 8);
        for (uint32_t i = 0; i < n; i++) {
            size_t off = 12 + size_t(i) * 8;
            if (off + 8 > buf_.size()) break;
            int32_t id = (int32_t)wa2::ReadU32(buf_.data() + off);
            uint32_t p = wa2::ReadU32(buf_.data() + off + 4);
            points_[id] = p;   // 与参考实现一致:同 id 后者覆盖
        }
    }
    auto it = points_.find(point);
    pos_ = it != points_.end() ? it->second : 0;
    if (it == points_.end() && point != 0)
        Log(LogLevel::Warn, "script: %s point %d missing, start at 0", name_.c_str(), point);

    std::vector<uint8_t> txt = res.Load(name_ + ".txt");
    if (!txt.empty()) ParseScriptTexts(txt, texts_);
    else Log(LogLevel::Warn, "script: %s.txt missing", name_.c_str());

    Log(LogLevel::Info, "script: loaded %s (points=%zu texts=%zu start=%u)",
        name_.c_str(), points_.size(), texts_.size(), pos_);
    return true;
}

uint32_t Script::ReadU32() {
    if (pos_ + 4 > buf_.size()) { exit_ = true; return 0; }
    uint32_t v = wa2::ReadU32(buf_.data() + pos_);   // util 的全局 ReadU32(小端)
    pos_ += 4;
    return v;
}
float Script::ReadF32() {
    if (pos_ + 4 > buf_.size()) { exit_ = true; return 0; }
    float v = wa2::ReadF32(buf_.data() + pos_);
    pos_ += 4;
    return v;
}

void Script::PushInt(int cmdType, int valType, int v) {
    Var var;
    var.cmd = cmdType; var.val = valType; var.ival = v;
    args.push_back(var);
}
void Script::PushFloat(int cmdType, int valType, float v) {
    Var var;
    var.cmd = cmdType; var.val = valType; var.fval = v;
    args.push_back(var);
}

Val Script::GetVar(const Var& v) const {
    Val out;
    switch (v.cmd) {
    case 5: // 立即数
        if (v.val == 4) { out.kind = Val::Float; out.f = v.fval; }
        else { out.kind = Val::Int; out.i = v.ival; }
        return out;
    case 1: // 全局旗标(操作数存在 ival)
        out.kind = Val::Int;
        out.i = gameFlags_ && v.ival >= 0 && v.ival < (int)gameFlags_->size()
                    ? (*gameFlags_)[v.ival] : 0;
        return out;
    case 2: // 局部变量(>=26 走浮点槽,复刻参考实现)
        out.kind = Val::Int;
        if (v.ival >= kMaxLocalVars) { out.kind = Val::Float; out.f = v.ival >= 26 ? gloFloats[v.ival % 26] : 0; }
        else out.i = gloInts[v.ival];
        return out;
    case 3: // 字符串
        out.kind = Val::Str;
        if (v.ival == 0) {
            static std::string def = kDefaultName;
            out.s = &def;
        } else if (v.ival > 0 && v.ival < (int)texts_.size()) {
            out.s = &texts_[v.ival];
        } else {
            static std::string empty;
            out.s = &empty;
        }
        return out;
    default:
        out.kind = Val::Int;
        return out;
    }
}

void Script::SetVar(Var& v, const Val& value) {
    switch (v.cmd) {
    case 1:
        if (gameFlags_ && v.ival >= 0 && v.ival < (int)gameFlags_->size())
            (*gameFlags_)[v.ival] = (int)value.AsInt();
        break;
    case 2:
        if (v.ival >= kMaxLocalVars) gloFloats[v.ival % 26] = value.AsFloat();
        else if (v.ival >= 0) gloInts[v.ival] = (int)value.AsInt();
        break;
    case 5:
        if (value.kind == Val::Float) { v.val = 4; v.fval = value.f; }
        else { v.val = 3; v.ival = (int32_t)value.AsInt(); }
        break;
    default:
        break;
    }
}

int Script::ArgsBackInt() {
    if (args.empty()) return 0;
    return (int)GetVar(args.back()).AsInt();
}

// ---------------- 跳转指令 ----------------
bool Script::ParseJumpFlag(Host& host) {
    uint32_t flag = ReadU32();
    switch (flag) {
    case 0:
        // 让出一拍;注意:与参考实现一致,不清空参数
        return false;
    case 1:
        exit_ = true;
        break;
    case 2:
        if (entries_.size() < 15) {
            JumpEntry e; e.type = 2;
            e.posArr[0] = ReadU32();
            e.pos = ReadU32();
            entries_.push_back(e);
        } else { ReadU32(); ReadU32(); }
        break;
    case 3:
        if (!entries_.empty()) {
            if (entries_.back().flag != 0) {
                pos_ = entries_.back().pos;
            } else {
                entries_.back().type = 3;
                entries_.back().posArr[0] = ReadU32();
            }
        }
        break;
    case 4:
        if (!entries_.empty() && entries_.back().flag == 0) break;
        if (!entries_.empty()) pos_ = entries_.back().pos;
        break;
    case 5:
        if (entries_.size() < 15) {
            JumpEntry e; e.type = 5;
            e.pos = ReadU32();
            e.posArr[0] = ReadU32();
            e.posArr[1] = ReadU32();
            e.posArr[2] = ReadU32();
            entries_.push_back(e);
        } else { ReadU32(); ReadU32(); ReadU32(); ReadU32(); }
        break;
    case 6:
        if (entries_.size() < 15) {
            JumpEntry e; e.type = 6;
            e.pos = ReadU32();
            entries_.push_back(e);
        } else ReadU32();
        break;
    case 7:
        if (entries_.size() < 15) {
            JumpEntry e; e.type = 7;
            e.count = ReadU32();
            for (uint32_t i = 0; i < e.count && i < 64; i++) {
                e.posArr[i] = ReadU32();
                e.flagArr[i] = ReadU32();
            }
            e.pos = ReadU32();
            entries_.push_back(e);
        } else {
            uint32_t n = ReadU32();
            for (uint32_t i = 0; i < n * 2 + 1; i++) ReadU32();
        }
        break;
    case 8:
    case 9:
        break;
    case 0xa:
        for (int i = (int)entries_.size() - 1; i >= 0; i--) {
            uint32_t t = entries_[i].type;
            if (t == 5 || t == 6 || t == 7) break;
            entries_.erase(entries_.begin() + i);
        }
        break;
    case 0xb:
        for (int i = (int)entries_.size() - 1; i >= 0; i--) {
            uint32_t t = entries_[i].type;
            if (t == 5) { pos_ = entries_[i].posArr[2]; break; }
            if (t == 6) { pos_ = entries_[i].posArr[0]; break; }
            entries_.erase(entries_.begin() + i);
        }
        break;
    case 0xc:
        pos_ = ReadU32();
        break;
    case 0xd: {
        if (!entries_.empty()) {
            JumpEntry& e = entries_.back();
            e.flag = ArgsBackInt();
            uint32_t pos1 = e.posArr[0], pos2 = e.pos;
            if (e.flag != 0) break;
            pos_ = pos1 != 0 ? pos1 : pos2;
        }
        break;
    }
    case 0xe: {
        if (!entries_.empty()) {
            JumpEntry& e = entries_.back();
            e.flag = ArgsBackInt();
            if (e.flag == 0) pos_ = e.pos;
            else pos_ = e.posArr[2];
        }
        break;
    }
    case 0xf: {
        if (!entries_.empty()) {
            JumpEntry& e = entries_.back();
            e.flag = ArgsBackInt();
            if (e.flag == 0) pos_ = e.pos;
        }
        break;
    }
    case 0x10: {
        if (!entries_.empty()) {
            JumpEntry& e = entries_.back();
            e.flag = ArgsBackInt();
            if (e.type != 7) break;
            bool hit = false;
            for (uint32_t i = 0; i < e.count && i < 64; i++) {
                if (e.flagArr[i] == (uint32_t)e.flag) {
                    pos_ = e.posArr[i];
                    hit = true;
                    break;
                }
            }
            if (!hit) pos_ = e.pos;
        }
        break;
    }
    default:
        break;
    }
    args.clear();
    return true;
}

// ---------------- 计算指令 ----------------
void Script::ParseCalc(Host& host) {
    uint32_t op = ReadU32();
    if (op > 0x1e) return;

    Val a, b;            // a=栈顶(将被消费), b=次顶(保留者)
    bool hasA = false, hasB = false;
    if (!args.empty() && op <= 0x1b) { a = GetVar(args.back()); hasA = true; }
    if (((op >= 1 && op < 0x17) || op == 0x1b || op == 0) && args.size() >= 2) {
        b = GetVar(args[args.size() - 2]);
        hasB = true;
    }
    // 与参考实现一致的弹栈规则:
    //  上述 b 情况:弹出栈顶(a);0x8~0x16(比较/数值):再弹 b,压回结果
    bool poppedA = false, poppedB = false;
    if (((op >= 1 && op < 0x17) || op == 0x1b || op == 0) && args.size() >= 2) {
        args.pop_back();
        poppedA = true;
    }
    if (op >= 8 && op <= 0x16 && !args.empty()) {
        args.pop_back();
        poppedB = true;
    }
    (void)hasA; (void)hasB;

    auto iA = [&]() { return a.AsInt(); };
    auto fA = [&]() { return a.AsFloat(); };
    auto iB = [&]() { return b.AsInt(); };
    auto fB = [&]() { return b.AsFloat(); };

    switch (op) {
    case 0: // 赋值 b = a
        if (poppedA && !args.empty()) SetVar(args.back(), a);
        break;
    case 1: if (poppedA && !args.empty()) SetVar(args.back(), Val{Val::Int, (int64_t)(int)(iA() + iB())}); break;
    case 2: if (poppedA && !args.empty()) SetVar(args.back(), Val{Val::Int, (int64_t)(int)(iB() - iA())}); break;
    case 3: if (poppedA && !args.empty()) SetVar(args.back(), Val{Val::Int, (int64_t)(int)(iA() * iB())}); break;
    case 4: if (poppedA && !args.empty() && iA() != 0) SetVar(args.back(), Val{Val::Int, (int64_t)(int)(iB() / iA())}); break;
    case 5: if (poppedA && !args.empty() && iA() != 0) SetVar(args.back(), Val{Val::Int, (int64_t)(iB() % iA())}); break;
    case 6: if (poppedA && !args.empty()) SetVar(args.back(), Val{Val::Int, (int64_t)(iB() & iA())}); break;
    case 7: if (poppedA && !args.empty()) SetVar(args.back(), Val{Val::Int, (int64_t)(iB() | iA())}); break;
    case 8:  PushInt(5, 3, iA() == iB() ? 1 : 0); break;
    case 9:  PushInt(5, 3, iB() < iA() ? 1 : 0); break;
    case 0xa: PushInt(5, 3, iB() > iA() ? 1 : 0); break;
    case 0xb: PushInt(5, 3, iB() <= iA() ? 1 : 0); break;
    case 0xc: PushInt(5, 3, iB() >= iA() ? 1 : 0); break;
    case 0xd: PushInt(5, 3, (iB() == 0 || iA() == 0) ? 0 : 1); break;
    case 0xe: PushInt(5, 3, (iB() != 0 || iA() != 0) ? 1 : 0); break;
    case 0xf: PushInt(5, 3, iB() != iA() ? 1 : 0); break;
    case 0x10: // 浮点加(任一为 float)
        if (a.kind == Val::Float || b.kind == Val::Float) PushFloat(5, 4, fA() + fB());
        else PushInt(5, 3, (int)(iA() + iB()));
        break;
    case 0x11:
        if (a.kind == Val::Float || b.kind == Val::Float) PushFloat(5, 4, fB() - fA());
        else PushInt(5, 3, (int)(iB() - iA()));
        break;
    case 0x12:
        if (a.kind == Val::Float || b.kind == Val::Float) PushFloat(5, 4, fB() * fA());
        else PushInt(5, 3, (int)(iB() * iA()));
        break;
    case 0x13:
        if (a.kind == Val::Float || b.kind == Val::Float) PushFloat(5, 4, fB() / fA());
        else PushInt(5, 3, (int)(iB() / iA()));
        break;
    case 0x14: PushInt(5, 3, iA() != 0 ? (int)(iB() % iA()) : 0); break;
    case 0x15: PushInt(5, 3, (int)(iB() & iA())); break;
    case 0x16: PushInt(5, 3, (int)(iB() | iA())); break;
    case 0x17: // 取负 / 逻辑非 / 自增 / 自减(原地修改栈顶)
        if (!args.empty()) { Val t = GetVar(args.back()); t.i = -(int64_t)t.AsInt(); SetVar(args.back(), Val{Val::Int, t.i}); }
        break;
    case 0x18:
        if (!args.empty()) SetVar(args.back(), Val{Val::Int, GetVar(args.back()).AsInt() == 0 ? 1 : 0});
        break;
    case 0x19:
        if (!args.empty()) { Val t = GetVar(args.back()); t.i = t.AsInt() + 1; SetVar(args.back(), Val{Val::Int, t.i}); }
        break;
    case 0x1a:
        if (!args.empty()) { Val t = GetVar(args.back()); t.i = t.AsInt() - 1; SetVar(args.back(), Val{Val::Int, t.i}); }
        break;
    case 0x1b: // 类型转换:栈顶(原次顶)的 val 类型 = 被弹出的 a 的值
        if (poppedA && !args.empty()) {
            args.back().val = (int)iA() == 4 ? 4 : 3;
        }
        break;
    case 0x1c:
    case 0x1d:
        break;
    case 0x1e:
        args.clear();
        break;
    default:
        break;
    }
}

// ---------------- 函数调用 ----------------
bool Script::CallFunc(Host& host) {
    uint32_t idx = ReadU32();
    return Funcs::Call(host, *this, idx, args);
}

// ---------------- 主循环 ----------------
TickResult Script::Tick(Host& host) {
    bool flag = true;
    while (flag && !exit_) {
        if (pos_ >= buf_.size()) { exit_ = true; break; }
        int cmd = (int)ReadU32();
        switch (cmd) {
        case 0: flag = ParseJumpFlag(host); break;
        case 1: case 2: case 3:
            PushInt(cmd, -1, (int)ReadU32());
            flag = true;
            break;
        case 4:
            flag = CallFunc(host);
            break;
        case 5: {
            int type = (int)ReadU32();
            if (type == 4) PushFloat(5, type, ReadF32());
            else PushInt(5, type, (int)ReadU32());
            flag = true;
            break;
        }
        case 6:
            ParseCalc(host);
            flag = true;
            break;
        default:
            break;
        }

        // 位置匹配的跳转条目出栈(复刻参考实现的清理规则)
        if (!entries_.empty()) {
            uint32_t t = entries_.back().type;
            if (t >= 2 && t <= 7) {
                for (int i = (int)entries_.size() - 1; i >= 0; i--) {
                    if (entries_[i].pos != pos_) break;
                    entries_.erase(entries_.begin() + i);
                }
            }
        }
    }
    return exit_ ? TickResult::End : TickResult::Wait;
}

// ---------------- 存档 ----------------
void Script::Save(ByteBuf& out) const {
    out.Str(name_);
    out.U32(pos_);
    out.I32(exit_ ? 1 : 0);
    out.I32((int32_t)args.size());
    for (const auto& v : args) {
        out.I32(v.cmd); out.I32(v.val); out.I32(v.idx); out.I32(v.ival); out.F32(v.fval);
    }
    for (int i = 0; i < kMaxLocalVars; i++) out.I32(gloInts[i]);
    for (int i = 0; i < kMaxLocalVars; i++) out.F32(gloFloats[i]);
    out.I32((int32_t)entries_.size());
    for (const auto& e : entries_) {
        out.U32(e.type); out.U32(e.count); out.U32(e.pos); out.I32(e.flag);
        for (int i = 0; i < 64; i++) out.U32(e.posArr[i]);
        for (int i = 0; i < 64; i++) out.U32(e.flagArr[i]);
    }
}

bool Script::LoadState(ByteReader& in, Res& res) {
    std::string name = in.Str();
    // 先按名字加载文件本体(会重置 pos/args/entries),再恢复运行态
    uint32_t pos = in.U32();
    exit_ = in.I32() != 0;
    int32_t nArgs = in.I32();
    std::vector<Var> savedArgs;
    for (int32_t i = 0; i < nArgs; i++) {
        Var v;
        v.cmd = in.I32(); v.val = in.I32(); v.idx = in.I32(); v.ival = in.I32(); v.fval = in.F32();
        savedArgs.push_back(v);
    }
    int32_t savedInts[kMaxLocalVars], savedFloatsAsInt[kMaxLocalVars];
    for (int i = 0; i < kMaxLocalVars; i++) savedInts[i] = in.I32();
    for (int i = 0; i < kMaxLocalVars; i++) savedFloatsAsInt[i] = in.I32();
    int32_t nEntries = in.I32();
    std::vector<JumpEntry> savedEntries;
    for (int32_t i = 0; i < nEntries; i++) {
        JumpEntry e;
        e.type = in.U32(); e.count = in.U32(); e.pos = in.U32(); e.flag = in.I32();
        for (int j = 0; j < 64; j++) e.posArr[j] = in.U32();
        for (int j = 0; j < 64; j++) e.flagArr[j] = in.U32();
        savedEntries.push_back(e);
    }
    if (!in.Ok()) return false;
    if (!Load(res, name, 0)) return false;
    pos_ = pos;
    args = std::move(savedArgs);
    for (int i = 0; i < kMaxLocalVars; i++) {
        gloInts[i] = savedInts[i];
        memcpy(&gloFloats[i], &savedFloatsAsInt[i], 4);
    }
    entries_ = std::move(savedEntries);
    return true;
}

} // namespace wa2
