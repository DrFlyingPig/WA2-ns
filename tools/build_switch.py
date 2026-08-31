#!/usr/bin/env python3
"""build_switch.py — 本地 Windows 构建驱动(devkitPro 装在非标准路径且无 make 时使用)

流程:编译全部源码 → 链接(specs)→ nacptool → elf2nro 出 wa2.nro。
EGL 桩:portlibs 无 mesa 时自动生成 stub libEGL(引擎走 SDL software 渲染器)。

用法:python tools/build_switch.py [--devkitpro D:/devkitPro]
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build_switch')
OUT = os.path.join(ROOT, 'wa2.nro')
DKP_WIN = 'D:/devkitPro'

SRC = [
    'src/main.cpp',
    'src/wa2/util.cpp', 'src/wa2/archive.cpp', 'src/wa2/res.cpp',
    'src/wa2/script.cpp', 'src/wa2/funcs.cpp', 'src/wa2/state.cpp',
    'src/wa2/sjis.cpp', 'src/wa2/sjis_table.cpp',
    'src/wa2/gfx.cpp', 'src/wa2/audio.cpp', 'src/wa2/engine.cpp',
]

ARCH = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE']
CXXFLAGS = ['-std=gnu++17', '-O2', '-Wall', '-fno-rtti', '-fno-exceptions',
            '-D__SWITCH__', '-DSWITCH'] + ARCH
if os.environ.get('WA2_MINIMAL'):
    CXXFLAGS = CXXFLAGS + ['-DWA2_MINIMAL']
if os.environ.get('WA2_MIN2'):
    CXXFLAGS = CXXFLAGS + ['-DWA2_MIN2']
if os.environ.get('WA2_MIN3'):
    CXXFLAGS = CXXFLAGS + ['-DWA2_MIN3']
LIBS = ['-lSDL2_image', '-lSDL2_ttf', '-lSDL2_mixer', '-lSDL2',
        '-lvorbisidec', '-logg', '-lmodplug', '-lmpg123',
        '-lopusfile', '-lopus',
        '-lpng', '-ljpeg', '-lwebp',
        '-lfreetype', '-lharfbuzz', '-lbz2', '-lz',
        '-ldrm_nouveau',
        '-lnx', '-lm']


def run(cmd, **kw):
    print('>>', ' '.join(str(c) for c in cmd))
    env = kw.pop('env', None) or subprocess_env()
    r = subprocess.run([str(c) for c in cmd], env=env, **kw)
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


def subprocess_env():
    """specs 里用 %:getenv(DEVKITPRO ...) 取路径,必须覆盖系统残留值"""
    env = dict(os.environ)
    env['DEVKITPRO'] = DKP_WIN
    env['DEVKITA64'] = DKP_WIN + '/devkitA64'
    return env


def make_egl_stub(dkp, outdir, gxx, nm, ar):
    """扫描 libSDL2.a 的未定义 egl* 符号,生成桩库(软件渲染不需要真 EGL)。"""
    sdl = os.path.join(dkp, 'portlibs', 'switch', 'lib', 'libSDL2.a')
    syms = sorted(s for s in nm_undefined(sdl, nm) if s.startswith('egl'))
    if not syms:
        print('SDL2 无 EGL 依赖,跳过桩库')
        return None
    c = os.path.join(outdir, 'egl_stub.c')
    with open(c, 'w') as f:
        f.write('// 自动生成:EGL 桩(software 渲染器用不到 GLES)\n')
        for s in syms:
            f.write(f'long {s}(void) {{ return 0; }}\n')
    stub_lib = os.path.join(outdir, 'libEGL_stub.a')
    run([gxx.replace('g++', 'gcc'), ARCH[0], ARCH[1], '-c', c, '-o', os.path.join(outdir, 'egl_stub.o')])
    run([ar, 'rcs', stub_lib, os.path.join(outdir, 'egl_stub.o')])
    print(f'EGL 桩库: {len(syms)} 符号')
    return stub_lib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--devkitpro', default='D:/devkitPro')
    args = ap.parse_args()
    dkp = args.devkitpro
    bin_ = os.path.join(dkp, 'devkitA64', 'bin')
    gxx = os.path.join(bin_, 'aarch64-none-elf-g++.exe')
    nm = os.path.join(bin_, 'aarch64-none-elf-nm.exe')
    ar = os.path.join(bin_, 'aarch64-none-elf-ar.exe')
    portlibs = os.path.join(dkp, 'portlibs', 'switch')
    specs = os.path.join(dkp, 'libnx', 'switch.specs')
    tools = os.path.join(dkp, 'tools', 'bin')

    os.makedirs(BUILD, exist_ok=True)

    # 检查 romfs demo 数据
    romfs = os.path.join(ROOT, 'romfs')
    if not os.path.exists(os.path.join(romfs, 'demo', 'game.ini')):
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
    objs = []
    for src in SRC:
        obj = os.path.join(BUILD, src.replace('/', '_').replace('\\', '_') + '.o')
        if not os.path.exists(obj) or os.path.getmtime(src) > os.path.getmtime(obj):
            run([gxx] + CXXFLAGS +
                ['-I' + os.path.join(ROOT, 'src'),
                 '-I' + os.path.join(portlibs, 'include'),
                 '-I' + os.path.join(portlibs, 'include', 'SDL2'),
                 '-I' + os.path.join(dkp, 'libnx', 'include'),
                 '-c', os.path.join(ROOT, src), '-o', obj])
        objs.append(obj)

    # 链接(必要时自动生成 EGL 桩)
    stub = make_egl_stub(dkp, BUILD, gxx, nm, ar)
    libs = LIBS + (['-lEGL_stub'] if stub else [])
    libpaths = ['-L' + os.path.join(portlibs, 'lib'),
                '-L' + os.path.join(dkp, 'libnx', 'lib')]
    if stub:
        libpaths.append('-L' + BUILD)
    extra = []
    if stub:
        extra = [stub]
    elf = os.path.join(ROOT, 'wa2.elf')
    link = [gxx] + ARCH + [
        '-specs=' + specs,
        '-Wl,-Map,' + os.path.join(ROOT, 'wa2.map'),
        '-o', elf,
    ] + objs + libpaths + libs + extra + ['-lnx', '-lm']
    r = subprocess.run([c for c in link if c], capture_output=True, text=True,
                       env=subprocess_env())
    if r.returncode != 0:
        print(r.stderr[-6000:])
        sys.exit('链接失败')
    print('链接成功:', elf)

    # nacp + nro
    nacp = os.path.join(ROOT, 'wa2.nacp')
    run([os.path.join(tools, 'nacptool.exe'), '--create',
         'WA2-ns', 'WA2-ns contributors', '0.1.0', nacp])
    run([os.path.join(tools, 'elf2nro.exe'), elf, OUT,
         '--icon=' + os.path.join(ROOT, 'icon.jpg'),
         '--nacp=' + nacp,
         '--romfsdir=' + romfs])
    print('完成:', OUT, os.path.getsize(OUT), 'bytes')


if __name__ == '__main__':
    main()
