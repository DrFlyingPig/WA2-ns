# WA2-ns — 白色相簿2 Nintendo Switch 移植(原生引擎)

> 在 Nintendo Switch(homebrew)上原生运行《WHITE ALBUM2》PC 版数据的开源引擎项目。

[![Build](https://img.shields.io/github/actions/workflow/status/OWNER/WA2-ns/build.yml?label=Build&style=flat-square)]() ![Platform](https://img.shields.io/badge/platform-Switch%20%7C%20PC-blue?style=flat-square) ![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)

---

## ⚠️ 重要声明

- 本项目是一个**开源引擎**,不包含、不分发任何《白色相簿2》的游戏资源
  (脚本、图像、音频、视频均属 Leaf/Aquaplus/Prototype 版权所有)。
- 要运行游戏,你需要**自行准备合法获得的 PC 版游戏数据**,并了解其格式
  (格式逆向资料见 `docs/formats.md`,参考项目见文末)。
- 内置的 `demo`(Tech Demo)是**原创占位内容**,仅用于验证引擎功能。

## ✨ 当前状态(Tech Preview)

| 功能 | 状态 |
| --- | --- |
| PACK / LAC 归档 + LZSS 解压 | ✅ 完成,有单测 |
| .bnr 字节码 VM(指令 0-6 / 跳转条目 / 计算栈) | ✅ 完成,有单测 |
| Shift-JIS(CP932)→ UTF-8 | ✅ 完成,有单测 |
| 文本窗 / 打字机 / \k 分段 / 选项 / 回看 | ✅ |
| 背景 / CG / 立绘 / 交叉淡化过渡 | ✅(掩码溶解待实现) |
| BGM(A/B 循环)/ SE / 语音 | ✅ |
| 存档 6 槽 / 设置 / 自动模式 / 跳过 | ✅ |
| 掩码溶解过渡、天气粒子、影片播放 | ⛔ 占位(见 Roadmap) |

## 🎮 Switch 端使用

1. 从 CI(Actions → Build → switch)下载 `wa2.nro`(或自行编译)
2. 放入 SD 卡:`sdmc:/switch/wa2.nro`
3. 数据目录:`sdmc:/wa2/`
   ```
   sdmc:/wa2/
     ├─ game.ini          # start=1001(起始脚本)
     ├─ font.ttf          # 任意 CJK TTF 字体(必须!)
     ├─ *.pac             # PACK 归档(游戏数据)
     ├─ saves/            # 存档(自动生成)
     └─ config.bin        # 设置(自动生成)
   ```
4. 从 HBMenu 启动。没有 SD 卡数据时,内置 romfs 会运行原创 Tech Demo。

**操作**:A 推进/确认 · B 菜单返回 · Y 自动 · R 跳过 · L 回看 · +(Start)菜单 · 触屏点按

## 🖥️ PC 端(开发调试)

```bash
# Linux
sudo apt install libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libsdl2-mixer-dev
make -f Makefile.pc
./wa2 out/Wa2Res        # 用 demo 数据跑

# 核心无头测试(不需要 SDL,Windows 可直接跑)
python tools/gen_demo.py          # 生成原创 demo 数据
tests\build_test.bat              # Windows + VS Build Tools
# 或 Linux:
make -f Makefile.pc test
```

## 🛠️ Switch 编译

```bash
# 方式 A:devkitPro 环境(标准)
#   devkitPro + devkitA64 + libnx + switch-sdl2/sdl2_image/sdl2_ttf/sdl2_mixer/freetype/harfbuzz
make -f Makefile.switch -j$(nproc)    # → wa2.nro

# 方式 B:本机 Windows 无 make(本项目实测路线)
#   devkitPro 手动安装到 D:\devkitPro(pacman 包解压,无需安装器),
#   由 build_switch.py 驱动编译链接,自动生成 EGL 桩(software 渲染)
python tools/build_switch.py --devkitpro D:/devkitPro    # → wa2.nro
```

推荐直接用 GitHub Actions 出包(已配置,见 `.github/workflows/build.yml`)。

> 字体说明:本地构建会把系统微软雅黑嵌入 romfs 便于个人测试,
> **分发的 nro 请勿携带该字体**(在 `romfs/demo/` 中删除 `font.ttf` 再构建,
> 或换成 OFL 授权字体如 Noto Sans SC)。

## 🧰 工具

| 工具 | 说明 |
| --- | --- |
| `tools/pak.py` | PACK/LAC 列表、解包、打包(含 LZSS 压缩) |
| `tools/gen_sjis.py` | 生成 SJIS→Unicode 映射表(`src/wa2/sjis_table.cpp`) |
| `tools/gen_demo.py` | 生成原创 demo 数据包(验证引擎全功能) |

## 📖 架构

```
src/wa2/
  archive.*   PACK/LAC + LZSS          ┐
  res.*       资源命名规则与查找         │ 核心层(无 SDL,
  script.*    .bnr 字节码 VM            │ 有无头单测)
  funcs.*     游戏函数表 + Host 接口     │
  state.cpp   场景状态/存档/配置序列化    ┘
  gfx.*       SDL2 渲染 + 字形缓存文本   ┐
  audio.*     SDL2_mixer 音频           │ 平台层
  engine.*    宿主实现/UI/输入/主循环     ┘
```

核心层与平台层的边界是 `Host` 接口(`funcs.h`):脚本 VM 只跟 Host 对话,
PC/无头测试与 Switch 共享同一套 VM 语义。

## 🗺️ Roadmap

1. 掩码溶解过渡(素材已在格式文档中,差 CPU 混合实现)
2. 用真实游戏数据做逐函数对拍(未知函数号补齐 `funcs.cpp`)
3. 已读文本判定 + 跳过限制、CG/场景回想、BGM 试听
4. 影片占位 → SFD 播放评估
5. 多语言(中文补丁数据需要 CP932 自定义映射,参考 reference/)
6. 触屏 UI 完整化(拖动/捏合、菜单手势)

## 📚 参考与致谢

格式与语义逆向的社区成果是本项目的前提:

- [dorakyuraduang/wa2-godot](https://github.com/dorakyuraduang/wa2-godot) —
  PC 版引擎重实现(Godot/C#),`reference/` 保存其源码作学习参考
- [wetor/LuckSystem](https://github.com/wetor/LuckSystem) 与
  [wetor/LucaSystemTools](https://github.com/wetor/LucaSystemTools) —
  LucaSystem(P/PS3/PSV 版)格式工具
- [YuriSizuku/OnscripterYuri](https://github.com/YuriSizuku/OnscripterYuri) /
  wetor/ONScripter-jh-Switch — Switch 端 VN 移植的构建模板与操作范式

## 📄 许可

引擎与工具:MIT(见 `LICENSE`)。游戏数据归其版权方所有。
`reference/` 目录内容不属于本许可范围,仅供本地学习,请勿再分发。
