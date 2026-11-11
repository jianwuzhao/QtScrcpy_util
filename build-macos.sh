#!/bin/sh
set -eu
if [ "$#" -gt 1 ]; then
    echo 'Usage: ./build-macos.sh [Qt6 installation prefix]' >&2
    exit 2
fi
if [ "$#" -eq 1 ]; then
    qt_prefix="$(cd "$1" && pwd)"
fi
cd "$(dirname "$0")"
if [ "$#" -eq 1 ]; then
    cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release -DQT_DESIRED_VERSION=6 \
        -DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_PREFIX_PATH=$qt_prefix"
else
    qtbase="$(brew --prefix qtbase)"
    multimedia="$(brew --prefix qtmultimedia)"
    cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release -DQT_DESIRED_VERSION=6 \
        -DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_PREFIX_PATH=$(brew --prefix qt)" \
        "-DQt6_DIR=$qtbase/lib/cmake/Qt6" "-DQt6Multimedia_DIR=$multimedia/lib/cmake/Qt6Multimedia"
fi
cmake --build build/macos --parallel
app=output/arm64/Release/QtScrcpy.app
cmake -E copy_directory keymap "$app/Contents/MacOS/keymap"
printf '%s\n' "Built: $PWD/$app"
