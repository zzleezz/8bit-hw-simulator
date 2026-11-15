# 8bit-hw-simulator

*Started as antirez's [c64-hw-simulator](https://github.com/antirez/c64-hw-simulator).
The program is still called `c64-hw-sim`.*

Simulate classic hardware at its pins. Lift a leg, short a signal, pull a
chip or open a resistor, then watch what happens on the monitor, logic
probe and oscilloscope.

## Systems

| Manufacturer | Model | Boards |
| --- | --- | --- |
| [Atari](docs/atari/README.md) | [2600A PAL](docs/atari/2600-pal.md) | Experimental digital bench for the UK four-switch console |
| [Commodore](docs/commodore/README.md) | [Commodore 64](docs/commodore/64.md) | ASSY 250407 and 250469 |
| [Sinclair](docs/sinclair/README.md) | [ZX Spectrum 48K](docs/sinclair/zx-spectrum-48k.md) | Issues 1, 2, 3B, 4B and 6A |

To try the Atari bench, select **Atari 2600 PAL** and open
[pal-bench.a26](carts/pal-bench.a26). The cartridge draws 12 colour bars
when RIOT RAM passes and alternating black and white bars when it fails.
The bench models the 6507, RIOT and PAL TIA digital signals. It supports
unbanked 2 KiB and 4 KiB cartridges. See the model page for fault examples
and the published hardware references.

## Guides

- [Using the bench](docs/bench.md)
- [Diagnostics and repair records](docs/diagnostics.md)
- [Command-line reference](docs/command-line.md)
- [How the simulation works and its limits](docs/simulation.md)

## Building

    make
    ./c64-hw-sim [program.prg]

It runs on macOS, Linux (X11) and Windows, with OpenGL. On Windows, build
with MinGW-w64 gcc from a POSIX shell (Git Bash or MSYS2), for example
`mingw32-make`, then run `c64-hw-sim.exe`. The graphics library (sokol)
and the interface library (nuklear) are in `third_party/`. Two of their
headers carry local fixes to mouse input and window handling; they are
listed in `third_party/PATCHES.md`.

The icon is a piece of the C64 board: `src/c64-hw-sim.ico` for the
Windows .exe (build.bat compiles `src/icon.rc` into it), and the same
pixels in `src/icon_data.c` for the window and the taskbar on every
system. `tools/make_icon.ps1 shot.png [X Y SIZE]` makes both again from
a `--screenshot`.

The mouse wheel scrolls whatever window is under the mouse (over the
board, it zooms). Esc closes the window on top. If the interface ever
seems stuck, press F12: it writes the state of the windows to
`ui-debug.log`, next to where the program was started. A frame that takes
over 1.5 s is logged there too, with what the interface was doing.

The windows are light or dark as the system is set (macOS, Windows 10
and later, GNOME), and follow it when it changes. The Theme row at the
bottom of the controls picks light or dark for good. The board, the
monitor and the scope screen look the same either way.

## Acknowledgements

I (antirez) got the idea for this program while using and observing Andre
Weissflog's (floooh) chips, emulators of the 6502, VIC-II, CIA, SID and
friends that use pins as their interface. The next step was to take this
approach, with his code as a starting point, to its logical extreme:
pin-level models of every other part of the C64, the RAMs, the ROMs, the
logic chips, the PLA, and so forth.

The chip cores are floooh's chips, taken from c64-llm (zlib license). The
boards come from bwack's C64-250407-Replica-KiCad and
C64C-250469-KiCAD-Replica (both CERN OHL). The PLA equations come from
the 82S100 read-out by Mark Smith and Marko Mäkelä.

For the ZX Spectrum: the Z80 is floooh's too, altered to run on the edges
of its clock. The five boards come from the KiCad projects of
[liveboxandy](https://github.com/liveboxandy): the Issue 3B, redrawn
from PABB's gerbers (CC0); the Issue 2 (ZX-Spectrum-48K-Issue-2, CC0),
drawn from photos of a bare board; the Issue 1 (ZX-Spectrum-48K-Issue-1,
CC0), drawn from a bare board too; the Issue 4B
(ZX-Spectrum-48K-Issue-4B, CC0); and the Issue 6A (ZX-Spectrum-48K-Issue-6A,
CC0). The ZX8401's insides (its 74LS equivalent, pin by pin) come from
the drop-in replacement on trastero.speccy.org (cosas/JL/pcf1306p). The ULA model is checked against
the gate netlist of the 6C001 recovered from the die by the emu-russia/ula
project (CC0). The ROM is Amstrad's, who allow its distribution. The ZX
Spectrum Diagnostics ROM is by Brendan Alford and its contributors (LGPL).

The light theme is by Henry, published here with permission.

> **An AI-generated project.** What this fork adds to antirez's original
> (the code, the board models, the tests and this documentation) was
> written by Claude, Anthropic's AI model, working in Claude Code, at the
> direction of zzleezz, who decided what to build and tested it. Where
> the model had to assume something about the hardware, the assumption
> is written down next to it (for example in "The 250469 board"). Check
> a diagnosis against the real machine before acting on it.

> **Credits.** Based on [c64-hw-simulator](https://github.com/antirez/c64-hw-simulator)
> by Salvatore Sanfilippo ([antirez](https://github.com/antirez)), the
> original project. This is a modified version of it, not the original:
> the ZX Spectrum, the C64's 250469 board, the fault atlas and the
> diagnosis tools were added here. The ZX Spectrum board layouts (Issues
> 1, 2, 3B, 4B and 6A) come from the KiCad projects of
> [liveboxandy](https://github.com/liveboxandy), the C64 ones from
> [bwack](https://github.com/bwack)'s.
> The rest of the people whose work it builds on are in
> [Acknowledgements](#acknowledgements).
