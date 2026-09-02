#!/usr/bin/env python3
"""gen_demo.py — 生成原创演示数据包(引擎自测 / 无游戏数据时的体验用)

产出内容全部为本工程原创,不含任何第三方游戏素材:
  - 剧本:原创占位短篇(日文 CP932),仅用于验证引擎功能
  - 背景/立绘:程序化渐变与剪影(TGA)
  - 掩码:程序化灰度图案(BMP)
  - BGM/SE:程序化合成(WAV)
  - 脚本字节码:由内置汇编器生成(.bnr + .txt,资源以 PACK+LZSS 打包)

用法:python tools/gen_demo.py [--font 字体路径]
"""
import math
import os
import shutil
import struct
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak import pack_dir  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 1280, 720


# ---------------- 素材生成(程序化,原创) ----------------
def make_bg(path, top, bottom, sun=None, seed=7):
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        t = y / (H - 1)
        color = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(W):
            px[x, y] = color
    d = ImageDraw.Draw(img)
    if sun:
        d.ellipse([900, 90, 1120, 310], fill=sun)
    rng = np.random.RandomState(seed)
    x0 = 0
    while x0 < W:
        w = int(rng.randint(60, 160))
        h = int(rng.randint(60, 200))
        d.rectangle([x0, H - 140 - h, x0 + w, H],
                    fill=tuple(int(c * 0.45) for c in bottom))
        x0 += w + int(rng.randint(4, 20))
    img.save(path, format="TGA")


def make_char(path, body_rgb, accent_rgb):
    img = Image.new("RGBA", (900, 720), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([330, 40, 570, 280], fill=body_rgb + (255,))
    d.rounded_rectangle([280, 270, 620, 700], 90, fill=body_rgb + (255,))
    d.ellipse([380, 90, 440, 150], fill=accent_rgb + (255,))
    d.ellipse([460, 90, 520, 150], fill=accent_rgb + (255,))
    d.rounded_rectangle([540, 320, 600, 560], 30, fill=accent_rgb + (255,))
    img.save(path, format="TGA")


def make_mask(path, id_):
    arr = (np.indices((H, W)).sum(axis=0) * 3 + id_ * 17) % 256
    Image.fromarray(arr.astype(np.uint8), "L").save(path, format="BMP")


def make_wav(path, seconds, freqs, amp=0.22, rate=44100):
    t = np.linspace(0, seconds, int(rate * seconds), endpoint=False)
    sig = np.zeros_like(t)
    for f in freqs:
        sig += np.sin(2 * np.pi * f * t)
    sig = (sig / len(freqs)) * amp
    fade = int(rate * 0.3)
    sig[:fade] *= np.linspace(0, 1, fade)
    sig[-fade:] *= np.linspace(1, 0, fade)
    data = (sig * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.column_stack([data, data]).tobytes())


# ---------------- 原创剧本文本(idx0 由引擎固定,不使用) ----------------
TEXTS = [
    "ダミー",                                            # 0
    "アオイ",                                            # 1
    "レン",                                              # 2
    "――放課後の屋上。風が気持ちいい。",                  # 3
    "アオイ「ねえ、聞いて。新しい曲、作ったんだ」",        # 4
    "レン「また?……聞かせてよ」",                         # 5
    "アオイ「メロディだけど、まだ詞がなくて」",            # 6
    "「歌詞を一緒に考える」",                             # 7 选项1
    "アオイ「その代わり?」",                             # 8
    "「まず曲だけ聴かせてもらう」",                       # 9 选项2
    "レン「完成したら、一番最初に僕が聴く約束」",          # 10
    "アオイ「……うん、約束」",                            # 11
    "レン「(図書室で、散らかったノートを開く)」",         # 12
    "レン「よし……まずは、タイトルから決めよう」",         # 13
    "アオイ「ねえ、この曲の題名、もう決めてる?」",        # 14
    "アオイ「……ふふ、まだ秘密」",                         # 15
    "レン「(夜の教室で、白板に音符を書く)」",             # 16
    "レン「打ち上げ花火、のような一曲にしたいな」",        # 17
    "アオイ「じゃあ、サビで一気に盛り上げよう」",          # 18
    "9003",                                              # 19 脚本名(SLoad 参数)
]

BG_ROOF, BG_LIB, BG_NIGHT = 1, 2, 3       # b0001/2/3 00.tga
CHAR_AOI, CHAR_REN = 0, 1                 # har / kaz


class Bnr:
    """bnr 字节码汇编器(指令语义见 docs/formats.md)

    所有地址以“代码区偏移”记录;end_bytes 时统一加上头部基址
    (magic 12B + 点表 8B×n),点表位于文件前部,代码随后。
    """

    def __init__(self):
        self.words = []
        self.points = {}
        self.addr_words = []   # 内容是“代码区地址”的 word 下标(goto/switch 操作数)

    def w(self, v):
        self.words.append(int(v) & 0xFFFFFFFF)

    def point(self, pid):
        self.points[pid] = len(self.words) * 4

    def raw(self, *vals):
        for v in vals:
            self.w(v)

    def flush(self):
        self.raw(0, 8)          # cmd0 flag8:清参数 no-op

    def push_int(self, v):
        self.raw(5, 3, v)

    def push_str(self, idx):
        self.raw(3, idx)

    def push_global(self, idx):
        self.raw(1, idx)

    def call(self, fid):
        self.raw(4, fid)

    def goto(self, pos):
        idx = len(self.words) + 2
        self.addr_words.append(idx)
        self.raw(0, 0x0C, pos)
        return idx

    def switch(self, flags, value_global_idx):
        """switch 结构(两条指令,与 VM 语义对应):
            cmd0 flag7  : 压 type7 条目(count + (pos,flag)×n + default)
            push_global : 压分发值(全局旗标引用)
            cmd0 flag0x10: 按值分发
        返回 (各分支 pos 的 word 下标, default pos 的 word 下标)。"""
        self.raw(0, 7, len(flags))
        pos_idx = []
        for f in flags:
            self.addr_words.append(len(self.words))
            pos_idx.append(len(self.words))
            self.w(0)        # pos 占位
            self.w(f)
        self.addr_words.append(len(self.words))
        def_idx = len(self.words)
        self.w(0)            # default 占位
        self.push_global(value_global_idx)
        self.raw(0, 0x10)
        return pos_idx, def_idx

    def setword(self, idx, v):
        self.words[idx] = int(v) & 0xFFFFFFFF

    # --- 组合指令(参数一律用 push,禁止裸字) ---
    def bg(self, efc, bg_id, no, frame, sx=1280, sy=720):
        for v in (efc, bg_id, no, frame, 0, 0, 0, sx, sy):
            self.push_int(v)
        self.call(0x92)
        self.flush()

    def chara(self, cid, no, pos, frame=0):
        for v in (cid, no, pos, 0, 0):
            self.push_int(v)
        if frame > 0:
            self.push_int(frame)
            self.call(0x9A)     # C:过渡
        else:
            self.call(0x9B)     # CW:立即
        self.flush()

    def chara_remove(self, cid, frame=0):
        self.push_int(cid)
        self.push_int(0)
        if frame > 0:
            self.push_int(frame)
            self.call(0x9C)
        else:
            self.call(0x9D)
        self.flush()

    def bgm(self, bid):
        for v in (bid, 0, 1, 255):
            self.push_int(v)
        self.call(0x9E)
        self.flush()

    def se(self, sid):
        self.push_int(sid)
        self.push_int(255)
        self.call(0xA4)
        self.flush()

    def name(self, idx):
        self.push_str(idx)
        self.call(0x90)
        self.flush()

    def say(self, name_idx, text_idx):
        if name_idx is not None:
            self.name(name_idx)
        self.push_str(text_idx)
        self.push_int(text_idx)
        self.push_int(1)
        self.call(0x82)
        self.flush()

    def wait(self, frames):
        self.push_int(frames)
        self.call(0xC2)
        self.flush()

    def calender(self, y, m, d, dow):
        for v in (y, m, d, dow):
            self.push_int(v)
        self.call(0xCB)
        self.flush()

    def setflag(self, idx, v):
        self.push_int(idx)
        self.push_int(v)
        self.call(0xC7)
        self.flush()

    def fade_out(self, frames):
        for v in (frames, 0, 0, 0):   # FB(frame, r, g, b)
            self.push_int(v)
        self.call(0x99)
        self.flush()

    def sload(self, name, point=0):
        self.push_str(name)
        self.push_int(point)
        self.call(0x00)
        self.flush()

    def gotitle(self):
        self.call(0xC5)
        self.flush()

    def end_bytes(self):
        # 头部基址:magic(12B) + 点表(8B × n);点表 pos 与代码内绝对地址统一换算
        base = 12 + 8 * len(self.points)
        for i in self.addr_words:
            self.words[i] = (self.words[i] + base) & 0xFFFFFFFF
        out = struct.pack("<III", 0x5243534C, 0, len(self.points))
        for pid, pos in sorted(self.points.items()):
            out += struct.pack("<iI", pid, pos + base)
        out += b"".join(struct.pack("<I", v) for v in self.words)
        return out


def build_9001():
    """原创短篇(前半):屋上对话 + 选项分支"""
    s = Bnr()
    s.point(0)
    s.bgm(1)
    s.bg(0, BG_ROOF, 0, 45)
    s.wait(30)
    s.chara(CHAR_AOI, 1, 1, 20)
    s.say(1, 3)
    s.say(1, 4)
    s.chara(CHAR_REN, 1, 2, 20)
    s.say(2, 5)
    s.say(1, 6)
    # --- 选项(结果写入 GameFlags[100])---
    for t in (7, 9):
        s.push_str(t)
        s.raw(255, 0, 0)
        s.call(0xD0)
        s.flush()
    s.push_global(100)
    s.call(0xD1)
    pos_idx, def_idx = s.switch([0, 1], 100)   # 选项结果:0 起始的按钮序号
    # case 1 → 図書室
    case1 = len(s.words) * 4
    s.say(2, 7)     # 复用台词(选项文本与台词同 idx,demo 简化)
    s.say(1, 8)
    s.say(2, 10)
    s.say(1, 11)
    s.se(1)
    s.setflag(101, 1)
    g1 = s.goto(0)
    # case 2 → 教室
    case2 = len(s.words) * 4
    s.say(2, 12)
    s.say(2, 13)
    s.say(1, 14)
    s.setflag(102, 1)
    g2 = s.goto(0)
    merge = len(s.words) * 4
    # --- 回填(代码区地址,end_bytes 统一加基址)---
    s.setword(pos_idx[0], case1)
    s.setword(pos_idx[1], case2)
    s.setword(def_idx, merge)
    s.setword(g1, merge)
    s.setword(g2, merge)
    # --- 合流 → 9003(脚本名取自文本表)---
    s.sload(19)
    return s


def build_9003():
    """原创短篇(后半):夜教室 + 日历演示 + 收尾"""
    s = Bnr()
    s.point(0)
    s.bg(0, BG_NIGHT, 0, 45)
    s.chara(CHAR_AOI, 2, 1, 20)
    s.say(1, 15)
    s.calender(2026, 8, 30, -1)
    s.say(1, 16)
    s.say(2, 17)
    s.say(1, 18)
    s.chara_remove(CHAR_AOI, 20)
    s.wait(20)
    s.fade_out(60)
    s.wait(80)
    s.gotitle()
    return s


def main():
    args = sys.argv[1:]
    font = args[args.index("--font") + 1] if "--font" in args else None

    stage = os.path.join(ROOT, "out", "_stage")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    # 素材(命名遵循 Res 命名规则)
    make_bg(os.path.join(stage, "b000100.tga"), (120, 180, 235), (210, 230, 245), sun=(255, 250, 220), seed=7)
    make_bg(os.path.join(stage, "b000200.tga"), (235, 150, 90), (250, 210, 160), seed=11)
    make_bg(os.path.join(stage, "b000300.tga"), (25, 30, 70), (90, 80, 140), seed=13)
    make_char(os.path.join(stage, "har000001.tga"), (90, 130, 200), (240, 240, 255))
    make_char(os.path.join(stage, "har000002.tga"), (80, 110, 180), (240, 240, 255))
    make_char(os.path.join(stage, "kaz000001.tga"), (200, 120, 130), (255, 230, 235))
    make_char(os.path.join(stage, "kaz000002.tga"), (180, 100, 115), (255, 230, 235))
    make_mask(os.path.join(stage, "f0011.bmp"), 11)
    make_wav(os.path.join(stage, "bgm_001.wav"), 8.0, [220.0, 277.18, 329.63])
    make_wav(os.path.join(stage, "bgm_002.wav"), 8.0, [196.0, 246.94, 293.66])
    make_wav(os.path.join(stage, "se_0001.wav"), 0.4, [880.0], amp=0.3)

    s9001, s9003 = build_9001(), build_9003()
    with open(os.path.join(stage, "9001.bnr"), "wb") as f:
        f.write(s9001.end_bytes())
    with open(os.path.join(stage, "9001.txt"), "wb") as f:
        f.write(",".join(TEXTS).encode("cp932"))
    with open(os.path.join(stage, "9003.bnr"), "wb") as f:
        f.write(s9003.end_bytes())
    with open(os.path.join(stage, "9003.txt"), "wb") as f:
        f.write(",".join(TEXTS).encode("cp932"))

    # 打包为 PACK(资源全部走归档+LZSS 路径)
    out = os.path.join(ROOT, "out", "Wa2Res")
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    pack_dir(stage, os.path.join(out, "demo.pac"))
    shutil.rmtree(stage)

    with open(os.path.join(out, "game.ini"), "w", encoding="ascii") as f:
        f.write("title=WA2-ns Tech Demo\nstart=9001\n")

    # 字体(本机字体仅本地测试用,仓库不带)
    copied = False
    cands = [font] if font else []
    cands += ["C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf"]
    for cand in cands:
        if cand and os.path.isfile(cand):
            shutil.copy(cand, os.path.join(out, "font.ttf"))
            print("font copied:", cand)
            copied = True
            break
    if not copied:
        print("WARN: 未找到 CJK 字体,请手动复制一份到 Wa2Res/font.ttf")

    print("demo data ->", out)


if __name__ == "__main__":
    main()
