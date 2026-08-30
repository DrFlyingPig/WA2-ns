// funcs.cpp — 游戏函数表实现(函数号语义见 docs/formats.md)
//
// 约定:FrameTime = 1/60s,函数参数里的“帧数”换算为毫秒 = frames * 1000/60。
// 返回值:true = 继续执行,false = 让出(等文本/点击/定时/动画/选项)。
#include "funcs.h"
#include "util.h"

#include <cmath>

namespace wa2 {

static const float kMsPerFrame = 1000.0f / 60.0f;

static int Arg(const std::vector<Var>& args, size_t i, Script& sc) {
    if (i >= args.size()) return 0;
    return (int)sc.GetVar(args[i]).AsInt();
}
static std::string ArgStr(const std::vector<Var>& args, size_t i, Script& sc) {
    if (i >= args.size()) return "";
    const Val& v = sc.GetVar(args[i]);
    return v.kind == Val::Str && v.s ? *v.s : "";
}
static bool IsStrArg(const std::vector<Var>& args, size_t i) {
    return i < args.size() && args[i].cmd == 3;
}

bool Funcs::Call(Host& host, Script& sc, uint32_t idx, std::vector<Var>& args) {
    switch (idx) {
    case 0x00: // SLoad(name, point)
        host.SLoadScript(ArgStr(args, 0, sc), Arg(args, 1, sc));
        args.clear();
        return false;
    case 0x01: // SCall(name, point)
        host.SCallScript(ArgStr(args, 0, sc), Arg(args, 1, sc));
        args.clear();
        return false;
    case 0x02: // call(point):同脚本内跳转到入口点
        host.CallPoint(Arg(args, 0, sc));
        args.clear();
        return false;
    case 0x03: // run:空转
        return false;
    case 0x04: // print(调试输出)
        Log(LogLevel::Info, "script print: %d args", (int)args.size());
        return true;
    case 0x05: // Ret
        return false;
    case 0x06: // _int
        if (!args.empty()) { int v = Arg(args, args.size() - 1, sc); args.pop_back(); host.PushInt(v); }
        return true;
    case 0x07: // _float
        if (!args.empty()) {
            const Val& v = sc.GetVar(args.back());
            float f = v.AsFloat();
            args.pop_back();
            host.PushFloat(f);
        }
        return true;
    case 0x08: // Rand:参考实现为空(返回 true,不改变栈)
        return true;
    case 0x09: { // Sin(角度制)
        if (!args.empty()) { float x = sc.GetVar(args.back()).AsFloat(); args.pop_back(); host.PushFloat(sinf(x * 3.14159265f / 180.0f)); }
        return true;
    }
    case 0x0a: {
        if (!args.empty()) { float x = sc.GetVar(args.back()).AsFloat(); args.pop_back(); host.PushFloat(cosf(x * 3.14159265f / 180.0f)); }
        return true;
    }
    case 0x0b: {
        if (!args.empty()) { float x = sc.GetVar(args.back()).AsFloat(); args.pop_back(); host.PushFloat(tanf(x * 3.14159265f / 180.0f)); }
        return true;
    }
    case 0x0c: {
        if (!args.empty()) { float x = sc.GetVar(args.back()).AsFloat(); args.pop_back(); host.PushFloat(asinf(x)); }
        return true;
    }
    case 0x0d: {
        if (!args.empty()) { float x = sc.GetVar(args.back()).AsFloat(); args.pop_back(); host.PushFloat(acosf(x)); }
        return true;
    }
    case 0x0e: {
        if (!args.empty()) { float x = sc.GetVar(args.back()).AsFloat(); args.pop_back(); host.PushFloat(atanf(x)); }
        return true;
    }
    case 0x0f: {
        if (!args.empty()) {
            float a = sc.GetVar(args.back()).AsFloat(); args.pop_back();
            float b = args.empty() ? 0 : sc.GetVar(args.back()).AsFloat();
            if (!args.empty()) args.pop_back();
            host.PushFloat(atan2f(b, a));
        }
        return true;
    }
    case 0x10: {
        if (!args.empty()) {
            float a = sc.GetVar(args.back()).AsFloat(); args.pop_back();
            float b = args.empty() ? 0 : sc.GetVar(args.back()).AsFloat();
            if (!args.empty()) args.pop_back();
            host.PushFloat(powf(b, a));
        }
        return true;
    }
    case 0x11: {
        if (!args.empty()) { float x = sc.GetVar(args.back()).AsFloat(); args.pop_back(); host.PushFloat(sqrtf(x)); }
        return true;
    }
    case 0x12: // TimeGetTime:宿主计时
        host.PushInt(host.ElapsedTimerMs());
        return true;

    // ---------------- 文本 ----------------
    case 0x80: case 0x81: // printEx / printEx2
        return true;
    case 0x82: // SetMessage(text, msgIdx, v3):v3==0 续写,否则清屏新句
        host.ShowMessage(ArgStr(args, 0, sc), Arg(args, 1, sc), 1, Arg(args, 2, sc) == 0);
        return false;
    case 0x83: // SetMessageE
        host.ShowMessage(ArgStr(args, 0, sc), Arg(args, 1, sc), 2, Arg(args, 2, sc) == 0);
        return false;
    case 0x84: // EndMessage
        host.EndMessage();
        return true;
    case 0x85: // SetMessage2(禁 skip)
        host.SetSkipDisable(true);
        host.ShowMessage(ArgStr(args, 0, sc), Arg(args, 1, sc), 2, Arg(args, 2, sc) == 0);
        return false;
    case 0x86: // WaitMessage2
        host.SetSkipDisable(false);
        return true;
    case 0x87: // K:等待点击
        host.WaitClick();
        return false;
    case 0x88: // SetDemoMode
        host.SetDemoMode(Arg(args, 0, sc) > 0);
        return true;
    case 0x89: // VI(_, label):设置语音文件标签
        if (args.size() >= 2 && Arg(args, 1, sc) != -1)
            host.SetVoiceLabel(Arg(args, 1, sc));
        return true;
    case 0x8a: // VV(_, loop, ch, track, voiceId):按当前标签播音
        host.PlayVoice(host.CurrentVoiceLabel(), Arg(args, 4, sc), Arg(args, 0, sc),
                       Arg(args, 1, sc) == 1, Arg(args, 3, sc));
        return true;
    case 0x8b: // VX(v0, label, v2, ch, loop, track)
        host.PlayVoice(Arg(args, 2, sc), Arg(args, 1, sc), Arg(args, 0, sc),
                       Arg(args, 4, sc) == 1, Arg(args, 5, sc));
        return true;
    case 0x8c: // VW(track):等语音结束
        host.WaitVoice(Arg(args, 1, sc));
        args.clear();
        return false;
    case 0x8d: // VS(time, track):停语音
        host.StopVoice((int)(Arg(args, 0, sc) * kMsPerFrame), Arg(args, 1, sc));
        return true;
    case 0x8e: // W:等待点击
        host.WaitClick();
        return false;
    case 0x8f: // WR:隐藏文本窗并等待
        host.HideWindow((int)(0.2f * 60.0f));
        return false;
    case 0x90: case 0xdf: // WN / WN2:设置人名
        host.SetName(IsStrArg(args, 0) ? ArgStr(args, 0, sc) : "");
        return true;
    case 0x91: case 0xe0: // WNS / WNS2
        return false;

    // ---------------- 画面 ----------------
    case 0x92: // B(efc, bgId, no, frame, offset, x, y, sx, sy)
        host.RenderImage(Arg(args, 2, sc) + 10 * Arg(args, 1, sc), Arg(args, 0, sc), false, 0,
                         Arg(args, 3, sc), Arg(args, 4, sc), Arg(args, 5, sc), Arg(args, 6, sc),
                         Arg(args, 7, sc) / 1280.0f, Arg(args, 8, sc) / 720.0f);
        return false;
    case 0x93: // BC(保留立绘)
        host.RenderImage(Arg(args, 2, sc) + 10 * Arg(args, 1, sc), Arg(args, 0, sc), true, 0,
                         Arg(args, 3, sc), Arg(args, 4, sc), Arg(args, 5, sc), Arg(args, 6, sc),
                         Arg(args, 7, sc) / 1280.0f, Arg(args, 8, sc) / 720.0f);
        return false;
    case 0x94: // V(CG)
        host.RenderImage(Arg(args, 2, sc) + 10 * Arg(args, 1, sc), Arg(args, 0, sc), false, 1,
                         Arg(args, 3, sc), Arg(args, 4, sc), Arg(args, 5, sc), Arg(args, 6, sc),
                         Arg(args, 7, sc) / 1280.0f, Arg(args, 8, sc) / 720.0f);
        return false;
    case 0x95: // H
        return false;
    case 0x96: host.Shake(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc)); return false;
    case 0x97: return false;
    case 0x98: // F(r,g,b,frame):遮罩色淡入(挂 overlay)
        host.ColorFade(Arg(args, 2, sc), Arg(args, 3, sc), Arg(args, 4, sc), Arg(args, 1, sc));
        return false;
    case 0x99: // FB(frame, r,g,b)
        host.ColorFade(Arg(args, 1, sc), Arg(args, 2, sc), Arg(args, 3, sc), Arg(args, 0, sc));
        return false;
    case 0x9a: // C(id, no, pos, ..., frame):加立绘并过渡
        host.AddChar(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc));
        host.UpdateChar(Arg(args, 5, sc));
        return false;
    case 0x9b: // CW(立即)
        host.AddChar(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc));
        return true;
    case 0x9c: // CR(pos, _, frame)
        host.RemoveChar(Arg(args, 0, sc));
        host.UpdateChar(Arg(args, 2, sc));
        return false;
    case 0x9d: // CRW(立即)
        host.RemoveChar(Arg(args, 0, sc));
        return true;

    // ---------------- 音乐/音效 ----------------
    case 0x9e: // M(bgmId, _, loop, vol)
        host.PlayBgm(Arg(args, 0, sc), Arg(args, 2, sc) != 0, Arg(args, 3, sc));
        return true;
    case 0x9f: // MS(frame)
        host.StopBgm(Arg(args, 0, sc));
        return true;
    case 0xa0: case 0xa2: case 0xa3: // MP/MW/MLW:让出(参考实现同)
        return false;
    case 0xa1: return true; // MV
    case 0xa4: // SE(id, vol):取空闲通道播放
        host.PlaySe(-1, Arg(args, 0, sc), false, 0, Arg(args, 1, sc));
        args.clear();
        return true;
    case 0xa5: // SEP(ch, id, fadeIn, loop, vol)
        host.PlaySe(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 3, sc) != 0,
                    Arg(args, 2, sc), Arg(args, 4, sc));
        args.clear();
        return true;
    case 0xa6: // SES(ch, fade)
        host.StopSe(Arg(args, 0, sc), Arg(args, 1, sc));
        args.clear();
        return true;
    case 0xa7: // SEV(ch, vol, frame)
        host.SetSeVolume(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc));
        args.clear();
        return true;
    case 0xa8: // SEW(ch)
        host.WaitSe(Arg(args, 0, sc));
        args.clear();
        return false;
    case 0xa9: return false; // SEVW
    case 0xaa: host.SetTimeMode(Arg(args, 0, sc)); return true;
    case 0xab: return true;  // SetChromaMode
    case 0xac: host.SetEffectMode(ArgStr(args, 0, sc)); return true;

    // ---------------- Bmp 自由图层 ----------------
    case 0xb0: host.LoadBmp(Arg(args, 0, sc), ArgStr(args, 1, sc), Arg(args, 2, sc)); return true;
    case 0xb1: return true;  // LoadBmpAnime(demo 数据不含,暂略)
    case 0xb2: return true;
    case 0xb3: return false;
    case 0xb4: host.ReleaseBmp(Arg(args, 0, sc)); return true;
    case 0xb5: return false;
    case 0xb6: case 0xb7: case 0xb8: return true;
    case 0xb9: host.SetBmpParam(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc), Arg(args, 3, sc)); return true;
    case 0xba: case 0xbb: case 0xbf: case 0xc0: return true;
    case 0xbc: host.SetBmpMove(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc)); return true;
    case 0xbd: host.SetBmpMove(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc)); return true;
    case 0xbe: host.SetBmpZoom(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc),
                               (float)Arg(args, 3, sc), (float)Arg(args, 4, sc)); return true;
    case 0xc1: // SetMovie(movieId, flagIdx):影片(占位实现)
        host.PlayMovie(Arg(args, 0, sc), Arg(args, 1, sc));
        return false;

    // ---------------- 计时/流程/旗标 ----------------
    case 0xc2: host.WaitMs(Arg(args, 0, sc) * kMsPerFrame); return false;
    case 0xc3: host.StartTimer(); return true;
    case 0xc4: host.WaitMs((float)Arg(args, 0, sc)); return false;
    case 0xc5: host.GoTitle(); return true;
    case 0xc6: host.PushInt(host.ReadSysFlag(Arg(args, 0, sc))); return true;
    case 0xc7: host.WriteSysFlag(Arg(args, 0, sc), Arg(args, 1, sc)); return true;
    case 0xc8: return false; // LogOut
    case 0xc9: host.WriteSysFlag(Arg(args, 0, sc) * 10 + Arg(args, 1, sc), Arg(args, 2, sc)); return true; // V_Flag
    case 0xca: return true;  // H_Flag
    case 0xcb: // Calender(y, m, d, dow)
        host.ShowCalender(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc), Arg(args, 3, sc));
        return false;
    case 0xcc: host.PushInt(host.ElapsedTimerMs()); return true;
    case 0xcd: case 0xed: case 0xee: host.PushInt(host.CanSkip() ? 1 : 0); return true;
    case 0xce: host.PushInt(host.Clicked() ? 1 : 0); return true;
    case 0xcf: return false; // runEX

    // ---------------- 选项 ----------------
    case 0xd0: // SetSelectMess(text, alpha, disable, v3)
        host.AddSelectItem(ArgStr(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc), Arg(args, 3, sc));
        return true;
    case 0xd1: // SetSelect(sel):显示选项,等待选择
        host.ShowSelect();
        args.clear();
        return false;

    // ---------------- 背景移动等 ----------------
    case 0xd2: // S(dx, dy, frame)
        host.BgMove(Arg(args, 0, sc), Arg(args, 1, sc), Arg(args, 2, sc));
        return true;
    case 0xd3: case 0xd4: return false; // Z / R
    case 0xd5: return false;            // WSZ:等 S/Z/R 动画
    case 0xd6: return false;            // StopSZR
    case 0xd7: case 0xd8: case 0xd9: case 0xda: return false; // VA/CS/CM/CRS
    case 0xdb: host.StopSkip(); return false;                 // SkipOFF
    case 0xdc: host.SetNovelMode(Arg(args, 0, sc) != 0); return false;
    case 0xdd: host.SetEroMode(Arg(args, 0, sc) == 1); return true;
    case 0xde: host.PushInt(host.ReplayMode() ? 1 : 0); return true;
    case 0xe1: // B2(无归一化的缩放参数)
        host.RenderImage(Arg(args, 2, sc) + 10 * Arg(args, 1, sc), Arg(args, 0, sc), false, 0,
                         Arg(args, 3, sc), Arg(args, 4, sc), Arg(args, 5, sc), Arg(args, 6, sc),
                         (float)Arg(args, 7, sc), (float)Arg(args, 8, sc));
        return false;
    case 0xe2:
        host.RenderImage(Arg(args, 2, sc) + 10 * Arg(args, 1, sc), Arg(args, 0, sc), true, 0,
                         Arg(args, 3, sc), Arg(args, 4, sc), Arg(args, 5, sc), Arg(args, 6, sc),
                         (float)Arg(args, 7, sc), (float)Arg(args, 8, sc));
        return false;
    case 0xe3:
        host.RenderImage(Arg(args, 2, sc) + 10 * Arg(args, 1, sc), Arg(args, 0, sc), false, 1,
                         Arg(args, 3, sc), Arg(args, 4, sc), Arg(args, 5, sc), Arg(args, 6, sc),
                         (float)Arg(args, 7, sc), (float)Arg(args, 8, sc));
        return false;
    case 0xe4: return false; // H2
    case 0xe5: case 0xe6: case 0xe7: return true;  // 天气占位
    case 0xe8: return false; // M2
    case 0xe9: host.NovelHide(Arg(args, 0, sc)); return false;
    case 0xea: host.NovelShow(Arg(args, 0, sc)); return false;
    case 0xeb: host.SetVoiceVolume(Arg(args, 2, sc), Arg(args, 0, sc), Arg(args, 1, sc)); return true;
    case 0xec: host.WaitMs(Arg(args, 0, sc) * kMsPerFrame); return false;

    default:
        Log(LogLevel::Warn, "script: unimplemented func 0x%x (%d args) at %s:%u",
            idx, (int)args.size(), sc.name().c_str(), 0);
        args.clear();
        return true;
    }
}

} // namespace wa2
