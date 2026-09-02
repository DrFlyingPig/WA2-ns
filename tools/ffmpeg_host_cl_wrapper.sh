#!/usr/bin/env bash
# ffmpeg_host_cl_wrapper.sh — configure 的 host 编译器入口(Windows)。
# FFmpeg configure 用 host-cc 编译运行期探针;MSVC cl 不接受 POSIX 风格的
# -Fo/out.obj 路径,这里转成 Windows 路径再转发。cl 必须已在 PATH
# (调用方先运行 vcvars64.bat;见 tools/build_ffmpeg_switch.sh)。
set -e

args=()
for arg in "$@"; do
    case "$arg" in
        -Fo/*)
            args+=("-Fo$(cygpath -w "${arg#-Fo}")")
            ;;
        -Fe/*)
            args+=("-Fe$(cygpath -w "${arg#-Fe}")")
            ;;
        /Fo/*)
            args+=("/Fo$(cygpath -w "${arg#/Fo}")")
            ;;
        /Fe/*)
            args+=("/Fe$(cygpath -w "${arg#/Fe}")")
            ;;
        *)
            args+=("$arg")
            ;;
    esac
done

exec cl "${args[@]}"
