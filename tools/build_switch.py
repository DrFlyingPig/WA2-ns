#!/usr/bin/env python3
"""build_switch.py — WA2-ns Switch 构建入口（Windows / Linux）

流程:编译全部源码 → 链接(specs + libnx framebuffer)→ nacptool → elf2nro 出 wa2.nro。
Switch 正式版不初始化 SDL 视频后端，画面由 SDL software renderer 合成后直接
提交到 libnx framebuffer。EGL 桩只满足 SDL2 静态库的未使用符号，不会在运行时调用。

用法:python tools/build_switch.py [--devkitpro D:/devkitPro] [--release]
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build_switch')
OUT = os.path.join(ROOT, 'wa2.nro')
DEFAULT_DEVKITPRO = os.environ.get('DEVKITPRO', 'D:/devkitPro')
APP_VERSION = '0.1.12'

# devkitPro SDL2 2.28.5 的默认 audren 后端在缓冲状态异常时存在
# buffer[-1] 访问和无超时忙等。实机表现为单核拉满，随后音频线程崩溃。
# 这里固定使用 devkitPro 自己保留的 audout 分支，并只替换 libSDL2.a
# 里的 Switch 音频对象；其余 SDL ABI/实现仍来自本机已安装的 2.28.5。
SDL_AUDOUT_REPO = 'https://github.com/devkitPro/SDL.git'
SDL_AUDOUT_BRANCH = 'switch-sdl-2.28-audout'
SDL_AUDOUT_COMMIT = '371ccb8dad5a274e01eabd863cd42d75ce89db54'
SDL_AUDOUT_SRC = os.path.join(ROOT, 'third_party', 'sdl2-switch-audout')

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

# WA2 私有最小 FFmpeg(仅 LGPL 组件):asf 解复用 + wmv3/vc1/wma 解码 +
# swscale/swresample。由 tools/build_ffmpeg_switch.sh 从锁定的 n7.1 提交
# 交叉编译;官方 switch-ffmpeg 包带 GPL/NVDEC 全家桶,既超 MIT 分发约束
# 又把 NRO 撑到 18MB+,故不用。
FFMPEG_PREFIX = os.path.join(ROOT, 'out', 'ffmpeg_min_prefix')
FFMPEG_LIBS = ['-lavformat', '-lavcodec', '-lswresample', '-lswscale', '-lavutil']


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


def ensure_sdl_audout_source(tool_env):
    """取得经 devkitPro 保留的 SDL2 audout 实现，并锁定已审计提交。"""
    source = os.path.join(SDL_AUDOUT_SRC, 'src', 'audio', 'switch',
                          'SDL_switchaudio.c')
    if not os.path.exists(source):
        os.makedirs(os.path.dirname(SDL_AUDOUT_SRC), exist_ok=True)
        print('>> 首次构建：获取 devkitPro SDL2 audout 后端 ...')
        run(['git', 'clone', '--depth', '1', '--branch', SDL_AUDOUT_BRANCH,
             SDL_AUDOUT_REPO, SDL_AUDOUT_SRC], tool_env)

    try:
        head = subprocess.check_output(
            ['git', '-C', SDL_AUDOUT_SRC, 'rev-parse', 'HEAD'],
            env=tool_env, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        sys.exit('无法核对 SDL2 audout 源码版本，请删除 third_party/sdl2-switch-audout 后重试')
    if head != SDL_AUDOUT_COMMIT:
        sys.exit('SDL2 audout 源码版本不匹配：\n'
                 f'  当前 {head}\n  需要 {SDL_AUDOUT_COMMIT}')
    return source


def make_stable_sdl2(dkp, outdir, gcc, ar, tool_env):
    """生成 WA2 私有 SDL：audout + 安全的 Switch 线程退出/栈大小。"""
    source = ensure_sdl_audout_source(tool_env)
    installed = os.path.join(dkp, 'portlibs', 'switch', 'lib', 'libSDL2.a')
    obj = os.path.join(outdir, 'SDL_switchaudio.o')
    patched_audio_source = os.path.join(outdir, 'SDL_switchaudio_wa2.c')
    thread_source = os.path.join(SDL_AUDOUT_SRC, 'src', 'thread', 'pthread',
                                 'SDL_systhread.c')
    thread_source_dir = os.path.dirname(thread_source)
    patched_thread_source = os.path.join(outdir, 'SDL_systhread_wa2.c')
    thread_obj = os.path.join(outdir, 'SDL_systhread.o')
    patched = os.path.join(outdir, 'libSDL2_wa2_audout.a')
    include = os.path.join(SDL_AUDOUT_SRC, 'include')
    src_include = os.path.join(SDL_AUDOUT_SRC, 'src')
    audio_source_dir = os.path.dirname(source)
    port_include = os.path.join(dkp, 'portlibs', 'switch', 'include', 'SDL2')
    nx_include = os.path.join(dkp, 'libnx', 'include')

    # 上游 audout 驱动忽略运行期 append/wait 的 Result；一旦服务报错，
    # SDL 音频循环便可能无等待地重复提交，表现为单核拉满。失败后应让 SDL
    # 切到 work_buffer+Delay 的断开设备路径，不能继续触碰失效的 audout。
    with open(source, 'r', encoding='utf-8') as f:
        audio_text = f.read()
    old_play_wait = '''static void
SWITCHAUDIO_PlayDevice(_THIS)
{
    this->hidden->cur_buffer = this->hidden->next_buffer;
    audoutAppendAudioOutBuffer(&this->hidden->buffer[this->hidden->cur_buffer]);
    this->hidden->next_buffer = (this->hidden->next_buffer + 1) % NUM_BUFFERS;
}

static void
SWITCHAUDIO_WaitDevice(_THIS)
{
    audoutWaitPlayFinish(&this->hidden->released_out_buffer, &this->hidden->released_out_count, UINT64_MAX);
}'''
    new_play_wait = '''static void
SWITCHAUDIO_PlayDevice(_THIS)
{
    Result res;
    this->hidden->cur_buffer = this->hidden->next_buffer;
    res = audoutAppendAudioOutBuffer(&this->hidden->buffer[this->hidden->cur_buffer]);
    if (R_FAILED(res)) {
        this->hidden->released_out_count = UINT32_MAX;
        SDL_SetError("audoutAppendAudioOutBuffer failed during playback (0x%x)", res);
        SDL_OpenedAudioDeviceDisconnected(this);
        return;
    }
    this->hidden->next_buffer = (this->hidden->next_buffer + 1) % NUM_BUFFERS;
}

static void
SWITCHAUDIO_WaitDevice(_THIS)
{
    Result res;
    if (this->hidden->released_out_count == UINT32_MAX) {
        SDL_Delay(10);
        return;
    }
    /*
     * SDL closes an audio device by setting device->shutdown and joining the
     * audio thread before the backend CloseDevice hook is called.  An infinite
     * audout wait therefore makes the join depend entirely on the service
     * event.  More importantly, hbloader can unmap the NRO while that thread
     * is still asleep, causing an Instruction Abort as soon as the SVC returns.
     *
     * Keep waiting for the same released buffer (do not return to SDL and
     * submit it again), but wake every 20 ms so cooperative shutdown is always
     * observed before the NRO can return to hbloader.
     */
    while (!SDL_AtomicGet(&this->shutdown)) {
        res = audoutWaitPlayFinish(&this->hidden->released_out_buffer,
                                   &this->hidden->released_out_count,
                                   20ULL * 1000ULL * 1000ULL);
        if (R_SUCCEEDED(res)) {
            return;
        }
        if (R_VALUE(res) == R_VALUE(KERNELRESULT(TimedOut))) {
            continue;
        }
        this->hidden->released_out_count = UINT32_MAX;
        SDL_SetError("audoutWaitPlayFinish failed during playback (0x%x)", res);
        SDL_OpenedAudioDeviceDisconnected(this);
        SDL_Delay(10);
        return;
    }
}'''
    if audio_text.count(old_play_wait) != 1:
        sys.exit('SDL_switchaudio.c 结构变化，拒绝生成未经审计的运行期错误补丁')
    audio_text = audio_text.replace(old_play_wait, new_play_wait)
    if ('UINT64_MAX' in audio_text or
            'KERNELRESULT(TimedOut)' not in audio_text or
            'SDL_AtomicGet(&this->shutdown)' not in audio_text):
        sys.exit('SDL_switchaudio.c 关闭校验失败：audout 仍可能无限等待')
    with open(patched_audio_source, 'w', encoding='utf-8', newline='\n') as f:
        f.write(audio_text)

    # 已安装 SDL_audio.o 仍引用 SWITCHAUDIO_bootstrap；用宏保持该 ABI，
    # 驱动对外名称则仍是上游的 switchout。
    run([gcc, '-std=gnu11', '-O2', '-Wall'] + ARCH + [
        '-DSDL_BUILDING_LIBRARY',
        '-D__SWITCH__', '-DSWITCH',
        '-DSWITCHAUDIOOUT_bootstrap=SWITCHAUDIO_bootstrap',
        '-I' + port_include, '-I' + audio_source_dir,
        '-I' + include, '-I' + src_include,
        '-I' + nx_include,
        '-c', patched_audio_source, '-o', obj,
    ], tool_env)

    # SDL_mixer 使用自定义回调，因此 SDL_audio.c 给音频线程传入的 stacksize
    # 实际是 0，而不是 SDL 内部队列线程使用的 64 KiB。旧补丁只处理非零值，
    # 真机仍落回 libnx/pthread 的 128 KiB 默认栈；SE 9711 首次流式 ov_read 后
    # 正是在这条约 132 KiB 的线程崩溃。Switch 上无条件至少给 1 MiB，并让
    # 线程按 SDL 的 shutdown 标志自然返回/join，避免异步取消强制展开。
    with open(thread_source, 'r', encoding='utf-8') as f:
        thread_text = f.read()
    old_stack = '''    if (thread->stacksize) {
        pthread_attr_setstacksize(&type, thread->stacksize);
    }'''
    new_stack = '''#ifdef __SWITCH__
    {
        const size_t safe_stack = thread->stacksize < (1024 * 1024)
            ? (1024 * 1024) : thread->stacksize;
        pthread_attr_setstacksize(&type, safe_stack);
    }
#else
    if (thread->stacksize) {
        pthread_attr_setstacksize(&type, thread->stacksize);
    }
#endif'''
    old_cancel = '''#ifdef PTHREAD_CANCEL_ASYNCHRONOUS
    /* Allow ourselves to be asynchronously cancelled */
    {
        int oldstate;
        pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);
    }
#endif'''
    new_cancel = '''#if defined(PTHREAD_CANCEL_ASYNCHRONOUS) && !defined(__SWITCH__)
    /* Switch threads exit cooperatively and are joined by SDL. */
    {
        int oldstate;
        pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);
    }
#endif'''
    if thread_text.count(old_stack) != 1 or thread_text.count(old_cancel) != 1:
        sys.exit('SDL_systhread.c 结构变化，拒绝生成未经审计的线程补丁')
    thread_text = thread_text.replace(old_stack, new_stack).replace(old_cancel, new_cancel)
    with open(patched_thread_source, 'w', encoding='utf-8', newline='\n') as f:
        f.write(thread_text)
    run([gcc, '-std=gnu11', '-O2', '-Wall'] + ARCH + [
        '-DSDL_BUILDING_LIBRARY', '-D__SWITCH__', '-DSWITCH',
        '-I' + port_include, '-I' + thread_source_dir,
        '-I' + include, '-I' + src_include,
        '-I' + nx_include,
        '-c', patched_thread_source, '-o', thread_obj,
    ], tool_env)

    shutil.copy2(installed, patched)
    run([ar, 'd', patched, 'SDL_switchaudio.o', 'SDL_systhread.o'], tool_env)
    run([ar, 'rcs', patched, obj, thread_obj], tool_env)
    print('SDL2 音频后端: switchout (Audio Out), all SDL threads >= 1 MiB, async cancel off')
    return patched


def make_egl_stub(sdl, outdir, gcc, nm, ar, tool_env):
    """满足 SDL2 静态库的 egl* 引用；正式版从不初始化 SDL 视频。"""
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
    ap.add_argument('--release', action='store_true',
                    help='正式版：启用最终确认的音频、输入、P2B、立绘和文本修复组合')
    ap.add_argument('--diag-disable-se', action='store_true',
                    help='单变量诊断 D1：只禁用 SE 播放/资源加载')
    ap.add_argument('--diag-stream-long-se', action='store_true',
                    help='单变量诊断 D2：超过 2 MiB 的 SE 不论 loop 均流式播放')
    ap.add_argument('--diag-c4-relative-timer', action='store_true',
                    help='累积诊断 D3：在已验证 D2 上只修正 0xC4 的绝对时间点语义')
    ap.add_argument('--diag-c4-click-advance', action='store_true',
                    help='累积诊断 D4：在 D3 上让完成对白后的确认解除 0xC4 等待')
    ap.add_argument('--diag-c4-sync-click-advance', action='store_true',
                    help='累积诊断 D5：在 D4 上按参考实现于第二次确认中同步继续脚本')
    ap.add_argument('--diag-cache-20mb', action='store_true',
                    help='累积性能 P1：D5 基础上将 Switch 纹理工作集预算提高到 20 MiB')
    ap.add_argument('--diag-direct-blocklinear', action='store_true',
                    help='累积性能 P2A：P1 基础上单线程直写 block-linear 帧缓冲')
    ap.add_argument('--diag-parallel-framebuffer', action='store_true',
                    help='累积性能 P2B：P2A 基础上增加 3 个帧缓冲转换 worker')
    ap.add_argument('--diag-static-redraw', action='store_true',
                    help='累积性能 P3：P2 基础上静止画面停止重复软件合成')
    ap.add_argument('--diag-character-lifecycle', action='store_true',
                    help='单变量功能 F1：在已验证 P2B 上修正立绘期望列表和清除语义')
    ap.add_argument('--diag-text-fit', action='store_true',
                    help='单变量功能 F2：在已验证 F1 上修正对白字号、四行边界和异常长文本适配')
    ap.add_argument('--diag-text-reflow', action='store_true',
                    help='历史功能 F3：在 F2 上扩大到 920px，并按实际字宽重排软换行')
    ap.add_argument('--diag-text-safe-width', action='store_true',
                    help='单变量功能 F4：在 F3 上把普通对白恢复到参考 UI 的 784px 安全宽度')
    ap.add_argument('--diag-text-snow-safe', action='store_true',
                    help='单变量功能 F5：在 F4 上把正文收至 x=950，避开右侧雪花装饰')
    ap.add_argument('--output', default=OUT,
                    help='输出 NRO 路径；ELF/MAP/NACP 使用同名路径')
    args = ap.parse_args()

    # 正式版不是一个新的诊断变量，而是把实机逐项确认过的修复链固化下来。
    # P3 曾在真机进入时闪退，因此明确不纳入；demo 资源也不得混入分发包。
    diagnostic_options = (
        'diag_disable_se', 'diag_stream_long_se',
        'diag_c4_relative_timer', 'diag_c4_click_advance',
        'diag_c4_sync_click_advance', 'diag_cache_20mb',
        'diag_direct_blocklinear', 'diag_parallel_framebuffer',
        'diag_static_redraw', 'diag_character_lifecycle',
        'diag_text_fit', 'diag_text_reflow', 'diag_text_safe_width',
        'diag_text_snow_safe',
    )
    if args.release:
        if args.with_demo:
            ap.error('--release 不能与 --with-demo 同时启用：正式包不嵌入 demo/字体')
        explicit_diagnostics = [
            name for name in diagnostic_options if getattr(args, name)
        ]
        if explicit_diagnostics:
            ap.error('--release 已固定完整修复链，不能再混用单变量诊断参数：' +
                     ', '.join(explicit_diagnostics))
        release_chain = (
            'diag_stream_long_se',
            'diag_c4_relative_timer',
            'diag_c4_click_advance',
            'diag_c4_sync_click_advance',
            'diag_cache_20mb',
            'diag_direct_blocklinear',
            'diag_parallel_framebuffer',
            'diag_character_lifecycle',
            'diag_text_fit',
            'diag_text_reflow',
            'diag_text_safe_width',
            'diag_text_snow_safe',
        )
        for name in release_chain:
            setattr(args, name, True)
    if args.diag_disable_se and (args.diag_stream_long_se or
                                 args.diag_c4_relative_timer or
                                 args.diag_c4_click_advance or
                                 args.diag_c4_sync_click_advance or
                                 args.diag_cache_20mb or
                                 args.diag_direct_blocklinear or
                                 args.diag_parallel_framebuffer or
                                 args.diag_static_redraw or
                                 args.diag_character_lifecycle or
                                 args.diag_text_fit or
                                 args.diag_text_reflow or
                                 args.diag_text_safe_width or
                                 args.diag_text_snow_safe):
        ap.error('D1 不能与 D2/D3/D4/D5/P1/P2A/P2B/P3/F1/F2/F3/F4/F5 同时启用：诊断基线必须明确')
    if args.diag_c4_relative_timer and not args.diag_stream_long_se:
        ap.error('D3 是已验证 D2 的单变量增量，必须同时启用 --diag-stream-long-se')
    if args.diag_c4_click_advance and not args.diag_c4_relative_timer:
        ap.error('D4 是 D3 的单变量增量，必须同时启用 --diag-c4-relative-timer')
    if args.diag_c4_sync_click_advance and not args.diag_c4_click_advance:
        ap.error('D5 是 D4 的单变量增量，必须同时启用 --diag-c4-click-advance')
    if args.diag_cache_20mb and not args.diag_c4_sync_click_advance:
        ap.error('P1 是已验证 D5 的单变量增量，必须同时启用 --diag-c4-sync-click-advance')
    if args.diag_direct_blocklinear and not args.diag_cache_20mb:
        ap.error('P2A 是 P1 的单变量增量，必须同时启用 --diag-cache-20mb')
    if args.diag_parallel_framebuffer and not args.diag_direct_blocklinear:
        ap.error('P2B 是 P2A 的单变量增量，必须同时启用 --diag-direct-blocklinear')
    if args.diag_static_redraw and not args.diag_parallel_framebuffer:
        ap.error('P3 是 P2 的单变量增量，必须同时启用 --diag-parallel-framebuffer')
    if args.diag_character_lifecycle and not args.diag_parallel_framebuffer:
        ap.error('F1 是已验证 P2B 的单变量增量，必须同时启用 --diag-parallel-framebuffer')
    if args.diag_character_lifecycle and args.diag_static_redraw:
        ap.error('F1 必须以已验证 P2B 为基线，不能混入已否决的 P3')
    if args.diag_text_fit and not args.diag_character_lifecycle:
        ap.error('F2 是已验证 F1 的单变量增量，必须同时启用 --diag-character-lifecycle')
    if args.diag_text_reflow and not args.diag_text_fit:
        ap.error('F3 是 F2 的单变量增量，必须同时启用 --diag-text-fit')
    if args.diag_text_safe_width and not args.diag_text_reflow:
        ap.error('F4 是 F3 的单变量增量，必须同时启用 --diag-text-reflow')
    if args.diag_text_snow_safe and not args.diag_text_safe_width:
        ap.error('F5 是 F4 的单变量增量，必须同时启用 --diag-text-safe-width')
    dkp = os.path.abspath(os.path.expanduser(args.devkitpro))
    exe = '.exe' if os.name == 'nt' else ''
    bin_ = os.path.join(dkp, 'devkitA64', 'bin')
    gxx = os.path.join(bin_, 'aarch64-none-elf-g++' + exe)
    gcc = os.path.join(bin_, 'aarch64-none-elf-gcc' + exe)
    nm = os.path.join(bin_, 'aarch64-none-elf-nm' + exe)
    objdump = os.path.join(bin_, 'aarch64-none-elf-objdump' + exe)
    ar = os.path.join(bin_, 'aarch64-none-elf-ar' + exe)
    portlibs = os.path.join(dkp, 'portlibs', 'switch')
    specs = os.path.join(dkp, 'libnx', 'switch.specs')
    tools = os.path.join(dkp, 'tools', 'bin')
    tool_env = subprocess_env(dkp)

    required = [gxx, gcc, nm, objdump, ar, specs,
                os.path.join(tools, 'nacptool' + exe),
                os.path.join(tools, 'elf2nro' + exe)]
    missing = [path for path in required if not os.path.exists(path)]
    if missing:
        sys.exit('缺少 devkitPro 构建文件:\n  ' + '\n  '.join(missing))

    os.makedirs(BUILD, exist_ok=True)

    cxxflags = list(CXXFLAGS)
    if args.with_demo:
        cxxflags.append('-DWA2_WITH_DEMO')
    if args.diag_disable_se:
        cxxflags.append('-DWA2_DIAG_DISABLE_SE')
    if args.diag_stream_long_se:
        cxxflags.append('-DWA2_DIAG_STREAM_LONG_SE')
    if args.diag_c4_relative_timer:
        cxxflags.append('-DWA2_DIAG_C4_RELATIVE_TIMER')
    if args.diag_c4_click_advance:
        cxxflags.append('-DWA2_DIAG_C4_CLICK_ADVANCE')
    if args.diag_c4_sync_click_advance:
        cxxflags.append('-DWA2_DIAG_C4_SYNC_CLICK_ADVANCE')
    if args.diag_cache_20mb:
        cxxflags.append('-DWA2_DIAG_CACHE_20MB')
    if args.diag_direct_blocklinear:
        cxxflags.append('-DWA2_DIAG_DIRECT_BLOCKLINEAR')
    if args.diag_parallel_framebuffer:
        cxxflags.append('-DWA2_DIAG_PARALLEL_FRAMEBUFFER')
    if args.diag_static_redraw:
        cxxflags.append('-DWA2_DIAG_STATIC_REDRAW')
    if args.diag_character_lifecycle:
        cxxflags.append('-DWA2_DIAG_CHARACTER_LIFECYCLE')
    if args.diag_text_fit:
        cxxflags.append('-DWA2_DIAG_TEXT_FIT')
    if args.diag_text_reflow:
        cxxflags.append('-DWA2_DIAG_TEXT_REFLOW')
    if args.diag_text_safe_width:
        cxxflags.append('-DWA2_DIAG_TEXT_SAFE_WIDTH')
    if args.diag_text_snow_safe:
        cxxflags.append('-DWA2_DIAG_TEXT_SNOW_SAFE')
    if args.release:
        cxxflags.append('-DWA2_RELEASE_BUILD')

    out = os.path.abspath(os.path.expanduser(args.output))
    out_dir = os.path.dirname(out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    artifact_stem, _ = os.path.splitext(out)

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
    ffmpeg_inc = os.path.join(FFMPEG_PREFIX, 'include')
    ffmpeg_lib = os.path.join(FFMPEG_PREFIX, 'lib')
    have_ffmpeg = all(os.path.exists(os.path.join(ffmpeg_lib, f'lib{lib[2:]}.a'))
                      for lib in FFMPEG_LIBS)
    if have_ffmpeg:
        cxxflags.append('-DWA2_HAS_FFMPEG')
        print('FFmpeg: 使用私有最小构建', FFMPEG_PREFIX)
    else:
        print('FFmpeg: 未找到', FFMPEG_PREFIX,
              '(影片播放将编译为不可用;先运行 tools/build_ffmpeg_switch.sh)')
    objs = []
    for src in SRC:
        obj = os.path.join(BUILD, src.replace('/', '_').replace('\\', '_') + '.o')
        include_dirs = ['-I' + os.path.join(ROOT, 'src'),
                        '-I' + os.path.join(portlibs, 'include'),
                        '-I' + os.path.join(portlibs, 'include', 'SDL2'),
                        '-I' + os.path.join(dkp, 'libnx', 'include')]
        if have_ffmpeg:
            include_dirs.append('-I' + ffmpeg_inc)
        run([gxx] + cxxflags + include_dirs +
            ['-c', os.path.join(ROOT, src), '-o', obj], tool_env)
        objs.append(obj)

    # 替换有忙等/越界风险的 audren 音频对象。这个私有库不会覆盖
    # devkitPro 的系统安装，也不改变 PC 构建。
    stable_sdl = make_stable_sdl2(dkp, BUILD, gcc, ar, tool_env)

    # SDL 的 Switch 视频对象仍保留未使用的 EGL 符号；用桩满足静态链接，
    # 从最终 NRO 中彻底移除 Mesa/Nouveau shader compiler。
    stub = make_egl_stub(stable_sdl, BUILD, gcc, nm, ar, tool_env)
    libpaths = ['-L' + os.path.join(portlibs, 'lib'),
                '-L' + os.path.join(dkp, 'libnx', 'lib')]
    if have_ffmpeg:
        libpaths.insert(0, '-L' + ffmpeg_lib)
    elf = artifact_stem + '.elf'
    map_file = artifact_stem + '.map'
    link_libs = [stable_sdl if lib == '-lSDL2' else lib for lib in LIBS]
    if have_ffmpeg:
        link_libs = link_libs + FFMPEG_LIBS
    link = [gxx] + ARCH + [
        '-specs=' + specs,
        '-Wl,-Map,' + map_file,
        '-o', elf,
    ] + objs + libpaths + ['-Wl,--start-group'] + link_libs + ([stub] if stub else []) + ['-Wl,--end-group']
    r = subprocess.run([c for c in link if c], capture_output=True, text=True,
                       env=tool_env)
    if r.returncode != 0:
        print(r.stderr[-6000:])
        sys.exit('链接失败')
    print('链接成功:', elf)

    # 防止链接顺序或 devkitPro 包升级让旧 audren 对象重新混入 NRO。
    linked_symbols = subprocess.check_output(
        [nm, '-g', elf], env=tool_env, text=True, errors='replace')
    required_audio = ('audoutInitialize', 'audoutWaitPlayFinish')
    forbidden_audio = ('audrenInitialize', 'audrvUpdate')
    missing_audio = [s for s in required_audio if s not in linked_symbols]
    leaked_audio = [s for s in forbidden_audio if s in linked_symbols]
    if missing_audio or leaked_audio:
        sys.exit('Switch 音频后端链接校验失败：'
                 f'缺少={missing_audio} 残留={leaked_audio}')
    if 'pthread_setcanceltype' in linked_symbols:
        sys.exit('Switch 线程校验失败：仍链接了 pthread_setcanceltype')
    if have_ffmpeg:
        # 影片链路必备:ASF 解复用 + WMV3/WMA2 解码 + 音频后混音入口。
        required_video = ('ff_asf_demuxer', 'ff_wmv3_decoder', 'ff_wmav2_decoder',
                          'avformat_open_input', 'sws_scale')
        missing_video = [s for s in required_video if s not in linked_symbols]
        if missing_video:
            sys.exit('Switch 影片链接校验失败：缺少=' + ','.join(missing_video))
        print('影片链接校验: asf/wmv3/wmav2 解码器已进入最终 ELF')
    all_symbols = subprocess.check_output(
        [nm, '-C', elf], env=tool_env, text=True, errors='replace')
    if 'EngineThreadMain(void*)' not in all_symbols:
        sys.exit('Switch 入口线程校验失败：引擎大栈工作线程未链接')
    if args.diag_direct_blocklinear:
        if 'framebufferMakeLinear' in all_symbols:
            sys.exit('直写帧缓冲校验失败：仍链接 libnx 线性 shadow 路径')
    if args.diag_parallel_framebuffer:
        if 'FramebufferSwizzleWorker(void*)' not in all_symbols:
            sys.exit('并行帧缓冲校验失败：转换 worker 未链接')
    elif args.diag_direct_blocklinear:
        if 'FramebufferSwizzleWorker(void*)' in all_symbols:
            sys.exit('P2A 单变量校验失败：意外链接了转换 worker')
    thread_disasm = subprocess.check_output(
        [objdump, '-d', '--disassemble=SDL_SYS_CreateThread', elf],
        env=tool_env, text=True, errors='replace')
    if ('#0x100000' not in thread_disasm or
            '<pthread_attr_setstacksize>' not in thread_disasm):
        sys.exit('Switch SDL 线程栈校验失败：1 MiB 无条件栈补丁未进入最终 ELF')
    mix_disasm = subprocess.check_output(
        [objdump, '-d', '--disassemble=_ZN3wa25Audio11MixStreamSeEPvi', elf],
        env=tool_env, text=True, errors='replace')
    decoder_disasm = subprocess.check_output(
        [objdump, '-d',
         '--disassemble=_ZN3wa2L20PumpOneStreamDecoderEPNS_5Audio8StreamSeEm', elf],
        env=tool_env, text=True, errors='replace')
    if '<ov_read>' in mix_disasm or '<ov_pcm_seek>' in mix_disasm:
        sys.exit('Switch 实时音频校验失败：SDL 回调仍直接调用 Tremor')
    if '<ov_read>' not in decoder_disasm:
        sys.exit('Switch 流式解码校验失败：主线程解码泵未链接 ov_read')
    play_disasm = subprocess.check_output(
        [objdump, '-d', '--disassemble=SWITCHAUDIO_PlayDevice', elf],
        env=tool_env, text=True, errors='replace')
    wait_disasm = subprocess.check_output(
        [objdump, '-d', '--disassemble=SWITCHAUDIO_WaitDevice', elf],
        env=tool_env, text=True, errors='replace')
    if ('<SDL_OpenedAudioDeviceDisconnected>' not in play_disasm or
            '<SDL_OpenedAudioDeviceDisconnected>' not in wait_disasm or
            '<SDL_Delay>' not in wait_disasm):
        sys.exit('Switch audout 错误路径校验失败：断开/节流补丁未进入最终 ELF')
    print('音频链接校验: audout=OK, audren/audrv=absent')
    print('线程链接校验: async pthread cancellation=absent')
    print('SDL 线程栈校验: minimum=1 MiB (including callback stacksize=0)')
    print('入口线程校验: engine worker=libnx native, stack=4 MiB')
    print('流式音频校验: Tremor=engine thread, SDL callback=PCM ring only')
    print('audout 错误校验: append/wait failure=disconnect + throttled')
    print('audout 关闭校验: wait slice=20 ms, cooperative shutdown=enabled')

    # nacp + nro
    nacp = artifact_stem + '.nacp'
    if args.release:
        app_name = 'WA2-ns'
    elif args.diag_text_snow_safe:
        app_name = 'WA2-ns [F5 SNOW SAFE]'
    elif args.diag_text_safe_width:
        app_name = 'WA2-ns [F4 TEXT SAFE]'
    elif args.diag_text_reflow:
        app_name = 'WA2-ns [F3 TEXT REFLOW]'
    elif args.diag_text_fit:
        app_name = 'WA2-ns [F2 TEXT FIT]'
    elif args.diag_character_lifecycle:
        app_name = 'WA2-ns [F1 CHAR STATE]'
    elif args.diag_static_redraw:
        app_name = 'WA2-ns [P3 SMART DRAW]'
    elif args.diag_parallel_framebuffer:
        app_name = 'WA2-ns [P2B WORKER FB]'
    elif args.diag_direct_blocklinear:
        app_name = 'WA2-ns [P2A DIRECT FB]'
    elif args.diag_cache_20mb:
        app_name = 'WA2-ns [P1 CACHE 20M]'
    elif args.diag_c4_sync_click_advance:
        app_name = 'WA2-ns [D5 C4 SYNC]'
    elif args.diag_c4_click_advance:
        app_name = 'WA2-ns [D4 C4 CLICK]'
    elif args.diag_c4_relative_timer:
        app_name = 'WA2-ns [D3 C4 TIMER]'
    elif args.diag_disable_se:
        app_name = 'WA2-ns [D1 SE OFF]'
    elif args.diag_stream_long_se:
        app_name = 'WA2-ns [D2 LONG SE]'
    else:
        app_name = 'WA2-ns'
    run([os.path.join(tools, 'nacptool' + exe), '--create',
         app_name, 'WA2-ns contributors', APP_VERSION, nacp], tool_env)
    nro_cmd = [os.path.join(tools, 'elf2nro' + exe), elf, out,
               '--icon=' + os.path.join(ROOT, 'icon.jpg'),
               '--nacp=' + nacp]
    if args.with_demo:
        nro_cmd.append('--romfsdir=' + romfs)
    run(nro_cmd, tool_env)
    print('完成:', out, os.path.getsize(out), 'bytes')


if __name__ == '__main__':
    main()
