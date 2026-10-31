# c64-hw-sim - pin level simulator of the C64 ASSY 250407 motherboard.
#
#   make            build ./c64-hw-sim (c64-hw-sim.exe on Windows)
#   make clean

CC      ?= cc
OPT     ?= -O3 -flto
CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Isrc -Ichips -Ithird_party -DSOKOL_GLCORE

SIM_SRC = src/sim.c src/parts.c src/board.c src/c64.c src/cart.c src/harness.c src/c64_board_data.c src/c64c_board_data.c src/zx48.c src/ula.c src/zx48_board_data.c src/zx48i2_board_data.c src/zx48i1_board_data.c src/zx48i4b_board_data.c src/zx48i6a_board_data.c src/png.c src/chips_impl.c
APP_SRC = src/main.c src/ui.c src/inspect.c src/browser.c src/diagnose.c src/repairs.c src/icon_data.c src/atlas_browse.c src/thumbtex.c src/probe.c src/probe_db.c src/filedialog.c src/atlas.c src/atlas_match.c src/stb_image_impl.c src/healthy.c

EXE =
ifeq ($(OS),Windows_NT)
  # MinGW-w64 gcc; run make from a POSIX shell (Git Bash, MSYS2) for mkdir/rm.
  ifeq ($(origin CC),default)
    CC = gcc
  endif
  EXE = .exe
  SOKOL_FLAGS = -Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-variable \
                -Wno-cast-function-type -Wno-unknown-pragmas
  LIBS = -mwindows -static -lpthread -lkernel32 -luser32 -lgdi32 -lshell32 -lcomdlg32 -lopengl32 -ladvapi32 -lm
else ifeq ($(shell uname -s),Darwin)
  SOKOL_FLAGS = -x objective-c -Wno-deprecated-declarations -Wno-unused-but-set-variable \
                -Wno-sign-compare -Wno-unused-variable
  LIBS = -framework Cocoa -framework QuartzCore -framework OpenGL
else
  SOKOL_FLAGS = -Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-variable
  LIBS = -lX11 -lXi -lXcursor -lGL -ldl -lpthread -lm
endif

all: c64-hw-sim$(EXE)

build/sokol_impl.o: src/sokol_impl.c third_party/sokol/*.h third_party/nuklear/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SOKOL_FLAGS) -c $< -o $@

build/%.o: src/%.c src/*.h chips/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

OBJS = $(patsubst src/%.c,build/%.o,$(SIM_SRC) $(APP_SRC)) build/sokol_impl.o

c64-hw-sim$(EXE): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	rm -rf build c64-hw-sim$(EXE)

.PHONY: all clean
