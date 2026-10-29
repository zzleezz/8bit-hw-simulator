@echo off
rem ==========================================================================
rem  build.bat - build c64-hw-sim.exe (native Windows 64-bit) with w64devkit.
rem
rem  Just double-click this file, or run it from a command prompt.
rem  The toolchain is expected at ..\w64devkit\bin\gcc.exe (next to this
rem  source folder). Every path is quoted, so spaces in the folder name
rem  (e.g. "Claude work") are fine.
rem
rem    build.bat          build c64-hw-sim.exe
rem    build.bat clean    delete the build folder and the exe
rem ==========================================================================
setlocal enabledelayedexpansion

rem --- Where we are, and where the compiler is -------------------------------
set "SRCDIR=%~dp0"
set "SRCDIR=%SRCDIR:~0,-1%"
set "DEVKIT=%SRCDIR%\..\w64devkit\bin"

if not exist "%DEVKIT%\gcc.exe" (
    rem Fall back to a w64devkit\bin inside this folder.
    set "DEVKIT=%SRCDIR%\w64devkit\bin"
)
if not exist "%DEVKIT%\gcc.exe" (
    echo ERROR: could not find gcc.exe in "%DEVKIT%".
    echo Edit the DEVKIT line in this script to point at your w64devkit\bin folder.
    exit /b 1
)

rem --- Put the toolchain first on PATH, just for this script ----------------
set "PATH=%DEVKIT%;%PATH%"
set "CC=%DEVKIT%\gcc.exe"

if /i "%~1"=="clean" goto :clean

rem --- Flags ----------------------------------------------------------------
rem  SOKOL_GLCORE keeps sokol_gfx / sokol_app on native Win32 + OpenGL.
rem
rem  -march=x86-64-v2/-mtune=generic: portable baseline (SSE4.2, POPCNT,
rem  CMPXCHG16B; no AVX), not tailored to the machine that compiles it -
rem  this .exe is meant to run on a friend's PC too. Safe on effectively
rem  any 64-bit Windows PC from the last ~15 years (Nehalem/Bulldozer or
rem  newer); skips only genuinely ancient first-gen 64-bit chips. (Going
rem  back to -march=native for a single-machine build, see the profiling
rem  we did: it bought ~2 points of real-time speed on this project's
rem  branchy, non-vectorizable hot path, not worth the lost portability.)
rem
rem  -flto: left out. This w64devkit gcc reports "LTO support has not
rem  been enabled in this configuration" (its cc1 wasn't built with it).
rem
rem  -ffast-math: deliberately left out, not just unhelpful. The hot path
rem  here is the event-driven net resolver in sim.c, which is branchy
rem  integer/pointer work, not floating point, so -ffast-math buys
rem  ~nothing on the actual bottleneck. Worse, sim.c's floating-net model
rem  (compute_hold_times, drift()) relies on isinf() to detect an infinite
rem  hold time; -ffast-math implies -ffinite-math-only, under which GCC is
rem  allowed to assume isinf() is always false and fold it away, silently
rem  corrupting that logic. Not worth the risk for a speedup it wouldn't
rem  even deliver.
set "OPT=-O3 -march=x86-64-v2 -mtune=generic -DNDEBUG"
set "CFLAGS=%OPT% -g -Wall -Wextra -Wno-unused-parameter -I"%SRCDIR%\src" -I"%SRCDIR%\chips" -I"%SRCDIR%\third_party" -DSOKOL_GLCORE"

rem  Extra warning silencers for the big single-header third-party blob.
set "SOKOL_FLAGS=-Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-variable -Wno-cast-function-type -Wno-unknown-pragmas"

rem  sim_unity.c (see that file) concatenates sim.c/parts.c/board.c/c64.c/
rem  cart.c/harness.c/zx48.c/ula.c/png.c/chips_impl.c into one translation
rem  unit, working around this w64devkit gcc having no -flto: it lets -O3
rem  inline the hot calls from parts.c/c64.c/zx48.c into sim.c's
rem  sim_drive_changed()/sim_settle()/sim_commit()/board_sample() (called
rem  well over a billion times on a full boot) instead of paying a real
rem  function-call for every one of them. -funroll-loops additionally
rem  unrolls the small, extremely hot loops this leaves behind (combine()'s
rem  drive-strength scan, the dirty-queue walk in sim_settle()).
rem  -fomit-frame-pointer frees up a register in those same loops (-O2+
rem  already implies it on this target, but asked for explicitly, so here
rem  it is explicitly).
set "UNITY_FLAGS=-fomit-frame-pointer -funroll-loops"

rem  Profile-guided optimization (-fprofile-generate, a training run of both
rem  machines booting and running their diagnostic cartridges, then
rem  -fprofile-use) was tried and measured: no gain (C64 within +/-3%,
rem  Spectrum ~2% slower). The hot path's branches depend on the netlist's
rem  data, not on code layout, so there is little for PGO to improve.

rem  Windows libraries in place of the Linux X11 set
rem  (-lX11 -lXi -lXcursor -lGL -ldl -lpthread). -static links libgcc,
rem  libstdc++ and winpthreads (libwinpthread-1.dll) straight into the
rem  .exe: nothing but OS-shipped DLLs (gdi32/opengl32/etc, already on
rem  every Windows PC) is needed at runtime, so this .exe is fine to zip
rem  up and hand to someone else as-is.
set "LIBS=-mwindows -static -lpthread -lgdi32 -lopengl32 -lole32 -lcomdlg32 -lwinmm -luuid -lkernel32 -luser32 -lshell32 -ladvapi32 -lm"

rem --- Sources ---------------------------------------------------------------
rem  c64_board_data.c / zx48_board_data.c are generated netlist tables (see
rem  their own "do not edit" header) with no hot functions: left out of
rem  sim_unity.c, compiled separately as before.
set "DATA_SRC=c64_board_data c64c_board_data zx48_board_data zx48i2_board_data zx48i1_board_data zx48i4b_board_data zx48i6a_board_data"
set "APP_SRC=main ui inspect browser diagnose repairs icon_data atlas_browse thumbtex probe probe_db filedialog atlas atlas_match stb_image_impl"

if not exist "%SRCDIR%\build" mkdir "%SRCDIR%\build"

echo Using gcc: "%CC%"
"%CC%" -dumpmachine
echo.

rem --- Compile ---------------------------------------------------------------
set "OBJS="

echo [1/3] compiling sokol + nuklear implementation (this one takes a while)...
"%CC%" %CFLAGS% %SOKOL_FLAGS% -c "%SRCDIR%\src\sokol_impl.c" -o "%SRCDIR%\build\sokol_impl.o"
if errorlevel 1 goto :failed
set "OBJS=!OBJS! "%SRCDIR%\build\sokol_impl.o""

echo [2/3] compiling the simulation core (unity build, also takes a while)...
"%CC%" %CFLAGS% %UNITY_FLAGS% -c "%SRCDIR%\src\sim_unity.c" -o "%SRCDIR%\build\sim_unity.o"
if errorlevel 1 goto :failed
set "OBJS=!OBJS! "%SRCDIR%\build\sim_unity.o""

echo [3/3] compiling board data and the interface...
for %%F in (%DATA_SRC% %APP_SRC%) do (
    echo    - %%F.c
    "%CC%" %CFLAGS% -c "%SRCDIR%\src\%%F.c" -o "%SRCDIR%\build\%%F.o"
    if errorlevel 1 goto :failed
    set "OBJS=!OBJS! "%SRCDIR%\build\%%F.o""
)

rem  The .exe's icon (src/icon.rc, src/c64-hw-sim.ico; tools/make_icon.ps1).
rem  windres runs the preprocessor through the shell with its own full path,
rem  which the space in a folder name like "Claude work" breaks: so plain
rem  gcc (first on PATH), from inside src with relative paths.
echo    - icon.rc
pushd "%SRCDIR%\src"
windres.exe --preprocessor=gcc --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED icon.rc -O coff -o ..\build\icon.o
set "RCERR=!errorlevel!"
popd
if not "!RCERR!"=="0" goto :failed
set "OBJS=!OBJS! "%SRCDIR%\build\icon.o""

rem --- Link ------------------------------------------------------------------
echo.
echo linking c64-hw-sim.exe ...
"%CC%" %CFLAGS% !OBJS! -o "%SRCDIR%\c64-hw-sim.exe" %LIBS%
if errorlevel 1 goto :failed

echo.
echo ==========================================================
echo  Built: "%SRCDIR%\c64-hw-sim.exe"
echo ==========================================================
endlocal
exit /b 0

:clean
echo Cleaning...
if exist "%SRCDIR%\build" rmdir /s /q "%SRCDIR%\build"
if exist "%SRCDIR%\c64-hw-sim.exe" del /q "%SRCDIR%\c64-hw-sim.exe"
echo Done.
endlocal
exit /b 0

:failed
echo.
echo *** BUILD FAILED ***
endlocal
exit /b 1
