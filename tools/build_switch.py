#!/usr/bin/env python3
"""build_switch.py — WA2-ns Switch 构建入口（Windows / Linux）

流程:编译全部源码 → 链接(specs + libnx framebuffer)→ nacptool → elf2nro 出 wa2.nro。
Switch 正式版不初始化 SDL 视频后端，画面由 SDL software renderer 合成后直接
提交到 libnx framebuffer。EGL 桩只满足 SDL2 静态库的未使用符号，不会在运行时调用。

用法:python tools/build_switch.py [--devkitpro D:/devkitPro] [--with-demo]
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build_switch')
OUT = os.path.join(ROOT, 'wa2.nro')
DEFAULT_DEVKITPRO = os.environ.get('DEVKITPRO', 'D:/devkitPro')
APP_VERSION = '0.1.7'

SRC = [
    'src/main.cpp',
    'src/wa2/util.cpp', 'src/wa2/archive.cpp', 'src/wa2/res.cpp',
    'src/wa2/script.cpp', 'src/wa2/funcs.cpp', 'src/wa2/state.cpp',
    'src/wa2/sjis.cpp', 'src/wa2/sjis_table.cpp',
    'src/wa2/gfx.cpp', 'src/wa2/audio.cpp', 'src/wa2/video.cpp', 'src/wa2/engine.cpp',
]

ARCH = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE']
CXXFLAGS = ['-std=gnu++17', '-O2', '-Wall', '-fno-rtti', '-fno-exceptions',
            '-D__SWITCH__', '-DSWITCH'] + ARCH
LIBS = ['-lSDL2_image', '-lSDL2_ttf', '-lSDL2_mixer', '-lSDL2',
        '-lvorbisidec', '-logg', '-lmodplug', '-lmpg123',
        '-lopusfile', '-lopus',
        '-lpng', '-ljpeg', '-lwebp',
        '-lfreetype', '-lharfbuzz', '-lbz2', '-lz',
        '-lnx', '-lm']


def run(cmd, tool_env, **kw):
    print('>>', ' '.join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], env=tool_env, **kw)
    if r.returncode != 0:
        sys.exit(f'命令失败: {cmd[0]} (exit {r.returncode})')
    return r


def nm_undefined(lib, nm):
    out = subprocess.run([nm, lib], capture_output=True, text=True).stdout
    undef = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == 'U':
            undef.add(parts[1])
        elif len(parts) == 3 and parts[1] == 'U':
            undef.add(parts[2])
    return undef


def subprocess_env(dkp):
    """specs 里用 %:getenv(DEVKITPRO ...) 取路径,必须覆盖系统残留值"""
    env = dict(os.environ)
    env['DEVKITPRO'] = dkp
    env['DEVKITA64'] = os.path.join(dkp, 'devkitA64')
    return env


def make_egl_stub(dkp, outdir, gcc, nm, ar, tool_env):
    """满足 SDL2 静态库的 egl* 引用；正式版从不初始化 SDL 视频。"""
    sdl = os.path.join(dkp, 'portlibs', 'switch', 'lib', 'libSDL2.a')
    syms = sorted(s for s in nm_undefined(sdl, nm) if s.startswith('egl'))
    if not syms:
        print('SDL2 无 EGL 依赖,跳过桩库')
        return None
    c = os.path.join(outdir, 'egl_stub.c')
    with open(c, 'w') as f:
        f.write('// 自动生成:EGL 桩(libnx framebuffer 正式版运行时不会调用)\n')
        for s in syms:
            f.write(f'long {s}(void) {{ return 0; }}\n')
    stub_lib = os.path.join(outdir, 'libEGL_stub.a')
    run([gcc, ARCH[0], ARCH[1], '-c', c, '-o', os.path.join(outdir, 'egl_stub.o')], tool_env)
    run([ar, 'rcs', stub_lib, os.path.join(outdir, 'egl_stub.o')], tool_env)
    print(f'EGL 桩库: {len(syms)} 符号')
    return stub_lib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--devkitpro', default=DEFAULT_DEVKITPRO)
    ap.add_argument('--with-demo', action='store_true',
                    help='把 romfs/demo 嵌入 NRO（仅开发测试；实机默认不嵌入以节省内存）')
    args = ap.parse_args()
    dkp = os.path.abspath(os.path.expanduser(args.devkitpro))
    exe = '.exe' if os.name == 'nt' else ''
    bin_ = os.path.join(dkp, 'devkitA64', 'bin')
    gxx = os.path.join(bin_, 'aarch64-none-elf-g++' + exe)
    gcc = os.path.join(bin_, 'aarch64-none-elf-gcc' + exe)
    nm = os.path.join(bin_, 'aarch64-none-elf-nm' + exe)
    ar = os.path.join(bin_, 'aarch64-none-elf-ar' + exe)
    portlibs = os.path.join(dkp, 'portlibs', 'switch')
    specs = os.path.join(dkp, 'libnx', 'switch.specs')
    tools = os.path.join(dkp, 'tools', 'bin')
    tool_env = subprocess_env(dkp)

    required = [gxx, gcc, nm, ar, specs,
                os.path.join(tools, 'nacptool' + exe),
                os.path.join(tools, 'elf2nro' + exe)]
    missing = [path for path in required if not os.path.exists(path)]
    if missing:
        sys.exit('缺少 devkitPro 构建文件:\n  ' + '\n  '.join(missing))

    os.makedirs(BUILD, exist_ok=True)

    cxxflags = list(CXXFLAGS)
    if args.with_demo:
        cxxflags.append('-DWA2_WITH_DEMO')

    # 正式 NRO 直接读取 sdmc:/wa2，不再把 42 MiB demo/字体附在 NRO 后面。
    # hbloader/appet 模式下这部分会侵占本就有限的进程地址空间和内存预算。
    romfs = os.path.join(ROOT, 'romfs')
    if args.with_demo and not os.path.exists(os.path.join(romfs, 'demo', 'game.ini')):
        print('>> 生成 demo 数据到 romfs/demo ...')
        subprocess.run([sys.executable,
                        os.path.join(ROOT, 'tools', 'gen_demo.py')], check=True)
        os.makedirs(romfs, exist_ok=True)
        import shutil
        dst = os.path.join(romfs, 'demo')
        if os.path.exists(dst):
            shutil.rmtree(dst)
        shutil.copytree(os.path.join(ROOT, 'out', 'Wa2Res'), dst)
        # 字体不入库:本地测试用系统字体,提交/分发前请移除
        if not os.path.exists(os.path.join(dst, 'font.ttf')):
            for cand in ('C:/Windows/Fonts/msyh.ttc', 'C:/Windows/Fonts/simhei.ttf'):
                if os.path.exists(cand):
                    import shutil as sh
                    sh.copy(cand, os.path.join(dst, 'font.ttf'))
                    print('   本地字体已嵌入 romfs(仅个人测试用):', cand)
                    break

    # 编译
    #
    # 这里故意每次全量编译。以前只比较 .cpp 和 .o 的时间戳，但没有
    # 生成头文件依赖；一旦头文件改变 Engine/Gfx 对象布局，未重编译的
    # main.o 会按旧 sizeof(Engine) 分配栈空间，真机必然内存破坏。
    # 当前项目只有十几个编译单元，全量构建的代价远小于错误 NRO 的风险。
    objs = []
    for src in SRC:
        obj = os.path.join(BUILD, src.replace('/', '_').replace('\\', '_') + '.o')
        run([gxx] + cxxflags +
            ['-I' + os.path.join(ROOT, 'src'),
             '-I' + os.path.join(portlibs, 'include'),
             '-I' + os.path.join(portlibs, 'include', 'SDL2'),
             '-I' + os.path.join(dkp, 'libnx', 'include'),
             '-c', os.path.join(ROOT, src), '-o', obj], tool_env)
        objs.append(obj)

    # SDL 的 Switch 视频对象仍保留未使用的 EGL 符号；用桩满足静态链接，
    # 从最终 NRO 中彻底移除 Mesa/Nouveau shader compiler。
    stub = make_egl_stub(dkp, BUILD, gcc, nm, ar, tool_env)
    libpaths = ['-L' + os.path.join(portlibs, 'lib'),
                '-L' + os.path.join(dkp, 'libnx', 'lib')]
    elf = os.path.join(ROOT, 'wa2.elf')
    link = [gxx] + ARCH + [
        '-specs=' + specs,
        '-Wl,-Map,' + os.path.join(ROOT, 'wa2.map'),
        '-o', elf,
    ] + objs + libpaths + ['-Wl,--start-group'] + LIBS + ([stub] if stub else []) + ['-Wl,--end-group']
    r = subprocess.run([c for c in link if c], capture_output=True, text=True,
                       env=tool_env)
    if r.returncode != 0:
        print(r.stderr[-6000:])
        sys.exit('链接失败')
    print('链接成功:', elf)

    # nacp + nro
    nacp = os.path.join(ROOT, 'wa2.nacp')
    run([os.path.join(tools, 'nacptool' + exe), '--create',
         'WA2-ns', 'WA2-ns contributors', APP_VERSION, nacp], tool_env)
    nro_cmd = [os.path.join(tools, 'elf2nro' + exe), elf, OUT,
               '--icon=' + os.path.join(ROOT, 'icon.jpg'),
               '--nacp=' + nacp]
    if args.with_demo:
        nro_cmd.append('--romfsdir=' + romfs)
    run(nro_cmd, tool_env)
    print('完成:', OUT, os.path.getsize(OUT), 'bytes')


if __name__ == '__main__':
    main()
