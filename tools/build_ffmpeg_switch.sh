#!/usr/bin/env bash
# build_ffmpeg_switch.sh — WA2 私有最小 FFmpeg(Switch, 仅 LGPL 组件)
#
# 用途:为影片播放提供 asf 解复用 + wmv3/vc1/wma 解码 + swscale/swresample。
# 不用 devkitPro 官方 switch-ffmpeg 包的原因:该包含 GPL/NVDEC 全量组件,
# NRO 膨胀到 18MB+ 且不满足本项目的 MIT 分发约束。
#
# 依赖(Windows):Git Bash + MSVC cl(经 vcvars64)+ msys make 包中的 make.exe
#              (Linux/macOS:系统 cc + make 即可)。
# 产物:out/ffmpeg_min_prefix(静态库+头文件),供 tools/build_switch.py 链接。
#
# 可复现性:FFmpeg 源码锁定到固定提交;configure 的 horizon 补丁在
# tools/patches/ffmpeg-horizon.patch。
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src_dir="$root/out/ffmpeg_min_src/ffmpeg-7.1"
prefix="$root/out/ffmpeg_min_prefix"
# n7.1 分支头;构建后可用 `git -C $src_dir rev-parse HEAD` 复核。
FFMPEG_EXPECTED_COMMIT=b08d7969c550a804a59511c7b83f2dd8cc0499b8

# Windows:codex 验证过的 make 路径(MSYS2 make 包解压);其它平台用 PATH。
win_make="$root/out/msys_make/usr/bin/make.exe"

if [ -n "${DEVKITPRO:-}" ]; then
    dkp="$DEVKITPRO"
else
    dkp="/d/devkitPro"
    if [ ! -d "$dkp" ] && [ -d "/opt/devkitpro" ]; then dkp="/opt/devkitpro"; fi
fi
export DEVKITPRO="$dkp"
export DEVKITA64="$dkp/devkitA64"

make_bin="make"
if [[ -x "$win_make" ]]; then
    make_bin="$win_make"
    export PATH="$(dirname "$win_make"):$PATH"
fi

# 1. 获取源码(浅克隆固定分支,校验提交)。
if [ ! -d "$src_dir" ]; then
    mkdir -p "$(dirname "$src_dir")"
    git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git "$src_dir"
fi
head="$(git -C "$src_dir" rev-parse HEAD)"
echo "FFmpeg source at $head"
if [ "$head" != "$FFMPEG_EXPECTED_COMMIT" ]; then
    echo "警告: FFmpeg 提交与锁定值不同(期望 $FFMPEG_EXPECTED_COMMIT)" >&2
fi

# 2. horizon 目标支持补丁(configure 识别 target-os=horizon)。
if ! git -C "$src_dir" diff --quiet -- configure 2>/dev/null; then
    echo "configure 已打过补丁"
else
    git -C "$src_dir" apply "$root/tools/patches/ffmpeg-horizon.patch"
fi

# 3. host 编译器:configure 需要运行期探针;Windows 上 MSVC 经 wrapper 暴露为 cc。
host_cc=""
if [[ "$OSTYPE" == msys || "$OSTYPE" == cygwin || "$OSTYPE" == win32 ]]; then
    host_cc="$root/tools/ffmpeg_host_cl_wrapper.sh"
else
    host_cc="${CC:-cc}"
fi

cd "$src_dir"

./configure \
  --prefix="$prefix" \
  --cross-prefix=aarch64-none-elf- \
  --enable-cross-compile \
  --host-cc="$host_cc" \
  --arch=aarch64 \
  --cpu=cortex-a57 \
  --target-os=horizon \
  --enable-pic \
  --extra-cflags='-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -ffunction-sections -fdata-sections' \
  --extra-ldflags="-fPIE -L$dkp/libnx/lib -Wl,--gc-sections" \
  --disable-runtime-cpudetect \
  --disable-programs \
  --disable-debug \
  --disable-doc \
  --disable-autodetect \
  --disable-network \
  --disable-everything \
  --disable-gpl \
  --disable-version3 \
  --disable-nonfree \
  --enable-avcodec \
  --enable-avformat \
  --enable-avutil \
  --enable-swscale \
  --enable-swresample \
  --enable-demuxer=asf \
  --enable-decoder=wmv3,vc1,wmav1,wmav2,wmapro,wmavoice \
  --enable-parser=vc1 \
  --enable-protocol=file \
  --enable-pthreads \
  --enable-asm \
  --enable-neon \
  --enable-small

"$make_bin" -j4
"$make_bin" install

echo "完成: $prefix"
ls -la "$prefix/lib"
