#!/usr/bin/env bash
# Native Linux build of the launcher.
#
# The Windows launcher is premake + MSVC + WebView2 (see premake5.lua). There is no
# WebView2 on Linux, so this build hosts the same ui/ page in Qt WebEngine: CMake and
# Qt6 do the work here.
#
# Requirements:
#   - a C++20 compiler (gcc or clang)
#   - cmake and ninja (or make)
#   - Qt6 base, widgets, webengine and webchannel development packages
#     Arch: pacman -S qt6-base qt6-webengine qt6-websockets cmake
#     Debian/Ubuntu: qt6-base-dev qt6-webengine-dev libqt6webchannel6-dev
#
# Usage: ./build-linux.sh [debug]
set -euo pipefail
cd "$(dirname "$0")"

config="${1:-release}"
if [ "$config" != "debug" ] && [ "$config" != "release" ]; then
    echo "usage: $0 [debug|release]" >&2
    exit 1
fi

build_dir="build/linux"
cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="${config^}"
cmake --build "$build_dir" -j"$(nproc)"

echo
echo "built: $(pwd)/$build_dir/Shield_Launcher"
