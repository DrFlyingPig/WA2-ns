// script.h — .bnr 字节码虚拟机
//
// 脚本 = <name>.bnr(字节码)+ <name>.txt(文本串, Shift-JIS, 逗号分隔)
// .bnr 头:[magic "LSCR" u32][4B 保留][pointCount u32][ (pointId i32, pos u32) × pointCount ]
// 字节码为 u32 小端字流:
//   0  → 跳转指令(后随 flag u32,语义见 ParseJumpFlag)
//   1/2/3 → 压入 全局旗标/局部变量/字符串 引用(后随索引 u32)
//   4  → 调用游戏函数(后随函数号 u32)
//   5  → 压入常量(后随类型 u32:4=float 读 f32,否则读 u32)
//   6  → 计算指令(后随运算号 u32,对参数栈操作)
// 函数返回 false = 让出本节拍,引擎在等待条件满足后继续。
#pragma once

#include "wa2.h"
#include "util.h"

namespace wa2 {

class Host;   // 引擎宿主接口(funcs.h)
class Res;

// 参数/变量:cmd 决定取值来源,val 决定数值解释
struct Var {
    int cmd = 5;      // 1=全局旗标 2=局部变量 3=字符串 5=立即数
    int val = 3;      // 3=int 4=float
    int idx = 0;
    int32_t ival = 0;
    float fval = 0;
};

struct Val {
    enum Kind { Int, Float, Str } kind = Int;
    int64_t i = 0;
    float f = 0;
    const std::string* s = nullptr;
    int64_t AsInt() const { return kind == Str ? 0 : (kind == Float ? (int64_t)f : i); }
    float   AsFloat() const { return kind == Str ? 0.f : (kind == Float ? f : (float)i); }
};

struct JumpEntry {
    uint32_t type = 0;
    uint32_t count = 0;
    uint32_t pos = 0;
    int32_t  flag = 0;
    uint32_t posArr[64] = {};
    uint32_t flagArr[64] = {};
};

enum class TickResult { Wait, End };   // Wait=本节拍让出 End=本脚本执行完毕

class Script {
public:
    bool Load(Res& res, const std::string& name, int point);
    TickResult Tick(Host& host);

    const std::string& name() const { return name_; }
    bool exitFlag() const { return exit_; }

    // --- 供函数表使用 ---
    void PushInt(int cmdType, int valType, int v);
    void PushFloat(int cmdType, int valType, float v);
    Val  GetVar(const Var& v) const;             // 求值(需要 gameFlags)
    void SetVar(Var& v, const Val& value);
    std::vector<Var> args;
    int  gloInts[kMaxLocalVars] = {};
    float gloFloats[kMaxLocalVars] = {};
    int  TextsSizeForDebug() const { return (int)texts_.size(); }
    uint32_t pos() const { return pos_; }

    void SetGameFlags(std::vector<int>* flags) { gameFlags_ = flags; }

    // --- 存档 ---
    void Save(ByteBuf& out) const;
    bool LoadState(ByteReader& in, Res& res);

private:
    uint32_t ReadU32();
    float    ReadF32();
    int      ArgsBackInt();                      // args.back() 求值为 int(空则 0)
    bool     ParseJumpFlag(Host& host);
    bool     CallFunc(Host& host);
    void     ParseCalc(Host& host);

    std::string name_;
    std::vector<uint8_t> buf_;
    std::vector<std::string> texts_;
    std::map<int, uint32_t> points_;
    std::vector<JumpEntry> entries_;
    uint32_t pos_ = 0;
    bool exit_ = false;
    std::vector<int>* gameFlags_ = nullptr;
};

// 把 .txt(Shift-JIS, 逗号分隔)解析为文本表;idx 0 固定为默认主角名
void ParseScriptTexts(const std::vector<uint8_t>& data, std::vector<std::string>& out,
                      bool patchFont = false);

} // namespace wa2
