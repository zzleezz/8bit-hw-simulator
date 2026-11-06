#!/bin/sh
# Build one firmware folder (stock or zx): ./build.sh stock
# Tools in J:/Coding/pico-tools (portable copies, see README.txt).
set -e
T=/j/Coding/pico-tools
DIR="$(cd "$(dirname "$0")" && pwd)/$1"
export PATH="$T/arm-toolchain/arm-gnu-toolchain-13.2.Rel1-mingw-w64-i686-arm-none-eabi/bin:$T/cmake/cmake-3.28.6-windows-x86_64/bin:$T/ninja:$T/picotool/picotool:$PATH"
export PICO_SDK_PATH="J:/Coding/pico-tools/pico-sdk"
export PICO_TOOLCHAIN_PATH="J:/Coding/pico-tools/arm-toolchain/arm-gnu-toolchain-13.2.Rel1-mingw-w64-i686-arm-none-eabi"
cd "$DIR"
mkdir -p build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -Dpicotool_DIR="J:/Coding/pico-tools/picotool/picotool" -Dpioasm_DIR="J:/Coding/pico-tools/sdk-tools/pioasm" .. > cmake.log 2>&1 || { tail -30 cmake.log; exit 1; }
ninja > ninja.log 2>&1 || { tail -40 ninja.log; exit 1; }
ls -la *.uf2
