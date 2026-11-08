#!/bin/sh
# build.sh - assemble the ZX test ROMs and make the SteamCard Pro image.
# AI-generated (Claude Code). Run from Git Bash: sh zxtest/build.sh
set -e
cd "$(dirname "$0")"
# sjasmplus 1.24.0 (github.com/z00m128/sjasmplus); SJASMPLUS=path to use another
ASM=${SJASMPLUS:-/j/Coding/pico-tools/sjasmplus/sjasmplus-1.24.0.win/sjasmplus.exe}
# A dump of the SteamCard Pro's 64K chip, if there is one: then the
# image with zxbench in bank 2 and zxtest in bank 3 is made too.
DUMP="${STEAMCARD_DUMP:-../../ROMs/RIP0001.bin}"
py -I mkfont.py font.inc
"$ASM" --nologo --lst=zxbench.lst zxbench.asm
"$ASM" --nologo --lst=zxtest.lst --sym=zxtest.sym zxtest.asm
if [ -f "$DUMP" ]; then
    py -I patchsum.py zxtest.rom "$DUMP" ../../ROMs/steamcard-test.bin
else
    py -I patchsum.py zxtest.rom
fi
