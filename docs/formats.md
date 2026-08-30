# WA2 PC 版数据格式笔记

本文档整理 WA2-ns 实现所依据的《白色相簿2》PC 版(Leaf,2010-2012)数据格式。
格式事实来自社区逆向(主要参考 `reference/wa2-godot` 的实现思路),
C++ 实现为本项目独立编写。

## 1. 归档格式

游戏资源打包在 `*.pac` 归档中,共两种变体,可共存:

### PACK

```
偏移  大小  内容
0     4    magic(读作 LE u32 = 0x5041434B,即字节 4B 43 41 50;部分工具按 "PACK" 处理,两者都验证)
4     8    保留
12    4    条目数 n (u32)
16 + i*44   条目:
        4    crypted (u32)   0=原样,非 0=LZSS 压缩
        24   文件名(Shift-JIS,\0 截断,小写)
        8    保留
        4    offset (u32)
        4    size (u32)
```

### LAC

```
0     4    magic "LAC\0"
4     4    条目数 n (u32)
8 + i*40    条目:
        32   文件名(逐字节按位取反混淆,Shift-JIS)
        4    size (u32)
        4    offset (u32)
```

### LZSS(压缩条目)

条目数据前 8 字节为两个 u32:`inlim`(压缩后长度)、`outlim`(解压后长度),随后是压缩流:

- 0x1000 环形缓冲,初始填 0x20(空格),写指针从 0xFEE 开始
- 每次读 1 字节标志位,LSB 在前,共 8 组:
  - 标志位 1:1 字节原文,写入环形缓冲与输出
  - 标志位 0:2 字节引用 `[b1, b2]`:
    `pos = b1 | ((b2 & 0xF0) << 4)`,`len = (b2 & 0x0F) + 3`
    从环形缓冲 pos 起循环复制 len 字节

## 2. 资源命名(大小写不敏感,一律按小写处理)

| 类型 | 规则 | 示例 |
| --- | --- | --- |
| 背景 | `B{bg:04d}{no}{time}.tga` | `b000100.tga` = 背景 1,变体 0,时间段 0 |
| CG | `v{id:06d}.tga` | `v100100.tga` |
| H 场景 | `h{id:06d}.tga` | |
| 立绘 | `{prefix}{no:06d}.tga` | `kaz000123.tga`;prefix 见角色表 |
| 过渡掩码 | `f0{id:03d}.bmp` | `f0011.bmp`(8-bit 灰度) |
| BGM | `bgm_{id:03d}.ogg` 或 `bgm_{id:03d}_a.ogg` + `_b.ogg` | _A 前奏播完接 _B 循环 |
| SE | `se_{id:04d}.wav`(回退 `.ogg`) | |
| 语音 | `{label:04d}_{id:04d}_{chr:02d}.ogg` | label 由 VI 函数设置 |

图片为标准 TGA(32 位 RGBA / 24 位 RGB),掩码为 8-bit 灰度 BMP。
`SetEffctMode` 可指定一个调色板 LUT 文件(256/768/1280 字节),对图像按
`gray = (77R + 151G + 28B) >> 8` 做逐像素映射(滤镜/夜间效果)。

立绘 prefix 表(角色编号 → 前缀,与原版一致):

```
0:har 1:kaz 2:set 3:koh 4:izu 5:mar 10:tak 11:ioo 12:chi 13:pap 14:mam
15:oto 16:you 17:tan 18:shi 19:tom 20:sat 21:hon 22:nak 23:say 24:aco
25:mih 26:mhh 27:ueh 28:yos 29:tan 30:ham 31:mat 32:kiz 33:suz 34:saw
35:miy 36:yan 37:mas
```

立绘槽位 X 偏移(相对屏幕中心,虚拟分辨率 1280×720):
`[-288, 0, 288, -384, 384, -480, 480, -480, -160, 160]`,
绘制顺序(远→近):`[5, 7, 3, 0, 8, 1, 9, 2, 4, 6]`。

## 3. 脚本:`<name>.bnr` + `<name>.txt`

每个场景两个文件,脚本名即文件主名(如 `1001`、`1008_020`):

- `.txt`:Shift-JIS 文本,**逗号分隔**成字符串表;
  脚本内 `STR_VAR 0` 固定解析为主角默认名(春希),`STR_VAR n>0` → 文本表第 n 项
- `.bnr`:字节码

```
0     4    magic "LSCR" (LE u32 = 0x5243534C)
4     4    保留
8     4    点表数量 n
12 + i*8   (pointId i32, 代码偏移 u32)   ← 偏移为绝对文件偏移
20 + 8n    代码区:u32 小端字流
```

### 指令集(每条以 cmd u32 开头)

| cmd | 语义 | 操作数 |
| --- | --- | --- |
| 0 | 跳转指令 | 1 个 u32 flag + 各自的操作数(见下) |
| 1 | 压入全局旗标引用(GLOBAL_VAR) | 索引 u32 |
| 2 | 压入局部变量引用(LOCAL_VAR) | 索引 u32 |
| 3 | 压入字符串引用(STR_VAR) | 文本表索引 u32 |
| 4 | 调用游戏函数 | 函数号 u32 |
| 5 | 压入立即数 | 类型 u32(4=float 读 f32,否则 int 读 u32) |
| 6 | 计算指令 | 运算号 u32 |

参数栈在函数间保留;**除 flag=0 外的所有跳转指令执行后清空参数栈**;
flag=0 表示让出一拍(常作参数清空的 no-op 用 flag=8/9)。

### 跳转 flag(flag 0-0x10)

| flag | 语义 |
| --- | --- |
| 0 | 让出一拍(不清参数) |
| 1 | 脚本结束 |
| 2 | 压入 type2 条目:读 skip_pos、body_pos(选择性跳转) |
| 3 | 条目 flag≠0 → 跳条目 Pos;否则读 else_pos 存条目 |
| 4 | 条目 flag≠0 → 跳回条目 Pos(循环) |
| 5 | 压入 type5 条目:读 4 个位置 |
| 6 | 压入 type6 条目:读 1 个位置 |
| 7 | 压入 type7(switch)条目:count,然后 count×(pos, flag),最后 default |
| 8/9 | 空操作(用于清参数) |
| 0xA | 弹出条目直到遇到 5/6/7 型 |
| 0xB | 从栈顶找 5 型跳其 PosArr[2],6 型跳其 PosArr[0],其余弹出 |
| 0xC | 绝对跳转(读 1 个 u32) |
| 0xD | flag=args 顶;flag≠0 继续,否则跳条目 PosArr[0](为 0 则 Pos) |
| 0xE | flag=args 顶;flag=0 → 条目 Pos,否则 PosArr[2] |
| 0xF | flag=args 顶;flag=0 → 条目 Pos |
| 0x10 | switch 分发:args 顶的值匹配条目 flagArr[i] → PosArr[i],否则 default |

### 计算运算号(0x00-0x1E)

`0=赋值 1=+ 2=- 3=* 4=/ 5=% 6=& 7=| 8=== 9=< 10=> 11=<= 12=>= 13=&& 14=|| 15=!=`
`16-20=浮点 + - * / % 21=& 22=| 23=取负 24=逻辑非 25=自增 26=自减 27=类型转换`
`28/29=保留 30=清参数栈`。
栈语义:操作数 a=栈顶、b=次顶;1-7/0 把结果写回 b;8-22 弹出双操作数压回结果。

## 4. 游戏函数表(cmd4,函数号 0x00-0xEE)

完整表见 `src/wa2/funcs.cpp` 的 dispatch。要点:

- 返回 true = 继续执行;false = 让出(等文本/点击/定时/动画/选项)
- 文本:`0x82 SetMessage(text, msgIdx, v3)`(v3=0 续写,否则新句)、
  `0x84 EndMessage`、`0x90 WN(name)`、`0x87 K/W(0x8E)` 等点击等待
- 画面:`0x92 B(efc, bg, no, frame, offset, x, y, sx, sy)` 背景、
  `0x94 V` CG、`0x9A/0x9B/0x9C/0x9D` 立绘增删、`0x98/0x99` 颜色淡入淡出、
  `0xE1-0xE3` 为不带归一化的变体(sx/sy 直接使用)
- 音频:`0x9E M(bgm, _, loop, vol)`、`0x9F MS`、`0xA4-0xA9` SE 系、
  `0x89 VI`/`0x8A VV`/`0x8B VX` 语音系
- 选项:`0xD0 SetSelectMess(text, alpha, disable, v3)` 累加选项项,最多 3 个;
  `0xD1 SetSelect(var)` 显示并等待——**点击后把 0 起始的按钮序号写回 args 顶的变量**
  (通常为其前压入的 GLOBAL_VAR 引用),脚本随后用 flag7+flag0x10 switch 分发
- 旗标:`0xC6/0xC7` 系统旗标读写(收 CG、电影已看、选项已读等),
  剧本好感度等走 GLOBAL_VAR(cmd1)

## 5. 脚本执行节拍(引擎宿主)

VM 每 1/30 秒被驱动一次,仅在以下条件全部满足时执行(否则等待):

- 文本打字机完成 **且** 无点击等待
- 无 WaitMs/WaitVoice/WaitSe 定时
- 无画面过渡动画
- 选项未显示、日历弹窗未关闭、菜单未打开
- 跳过模式未激活(跳过时快速推进)

虚拟分辨率 1280×720;所有“帧”参数按 1/60 秒换算为毫秒。

## 6. 与主机版(LucaSystem)的关系

PS3/PSV 版《WHITE ALBUM2 幸せの対面》使用 Prototype 的 LucaSystem 引擎
(Pak 归档、CZ 图像、不同脚本字节码),与本笔记描述的 **PC 版格式不是一回事**。
社区工具(wetor/LuckSystem 等)面向主机版;wa2-godot 与本项目面向 PC 版。
