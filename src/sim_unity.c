/* sim_unity.c - unity build of the simulation core.
 *
 * w64devkit's gcc was configured with --disable-lto, so the compiler
 * cannot inline across separate .c files: every call from parts.c/c64.c/
 * zx48.c into sim.c's exported functions (sim_drive_changed(), the slow
 * path of the sim_drive() hot pin write; sim_read_group(),
 * sim_drive_except(), sim_settle(), sim_commit(), board_sample()) is a
 * real, non-inlinable function call paid in full at every one of the
 * billions of pin changes a run does. Concatenating the .c files into one
 * translation unit gives -O3 the same cross-file visibility LTO would,
 * with no toolchain change needed: it can inline those calls at their
 * (many) call sites, same as it already inlines the sim.h fast paths
 * (sim_read()/sim_drive()/sim_mark_dirty(), already "static inline" in the
 * header and so already cross-file-inlined without any of this).
 *
 * build.bat compiles this instead of sim.c/parts.c/board.c/c64.c/cart.c/
 * harness.c/zx48.c/ula.c/png.c/chips_impl.c individually. It does NOT fold
 * in c64_board_data.c / zx48_board_data.c: they are generated tables with
 * no hot functions (see their own "do not edit" header), so there is
 * nothing to gain from merging them, and every risk in doing so (their
 * generator uses the same unprefixed field names, "nets"/"pads"/"comps",
 * in both files) is best avoided by just leaving them as they are.
 *
 * ORDER MATTERS. The "chips" emulator headers (m6502.h, m6526.h, m6569.h,
 * m6581.h, kbd.h, z80.h, chips_common.h) are `#pragma once`, which tracks
 * "already included" by file identity alone, NOT by which macros were
 * defined at the time - unlike a normal `#ifndef FOO_H` guard, there is no
 * way to process the same header twice in one translation unit under two
 * different macro states. Each header's actual chip model is compiled by
 * its own internal `#ifdef CHIPS_IMPL` block. So chips_impl.c (the file
 * that `#define`s CHIPS_IMPL before including them) must be the FIRST
 * thing in this unity file: that makes its one-and-only processing of
 * each header emit both the declarations and the implementation. c64.h
 * and zx48.h (pulled in later by c64.c/cart.c/harness.c/zx48.c/ula.c) then
 * `#include` those same headers again for their declarations as they
 * always did; `#pragma once` makes that second attempt a silent no-op,
 * which is exactly what is wanted: the symbols it was looking for are
 * already in scope from the first pass. Everything after that first
 * inclusion can stay in the engine's normal, natural order. */
#include "chips_impl.c"

#include "sim.c"
#include "parts.c"
#include "board.c"
#include "c64.c"
#include "cart.c"
#include "harness.c"
#include "zx48.c"
#include "ula.c"
#include "png.c"
