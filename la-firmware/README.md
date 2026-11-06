# zx firmware for the LogicAnalyzer boards

> **AI-generated (Claude Code)**, directed and tested by zzleezz. A small
> change to Agustín Gimenez Bernad's LogicAnalyzer firmware
> ([gusmanb/logicanalyzer](https://github.com/gusmanb/logicanalyzer),
> GPL-3); this folder is GPL-3 as well.

The simulator's `--la-capture` works with the stock firmware for a capture
from the reset (`--la-trigger reset`) or of a frame interrupt. The later
stages of the boot (`ramcheck`, `ramdone`, `copyright`, `main`) need this
one.

## Why

- **The trigger fired on glitches.** As the Spectrum's address lines change
  they don't switch together: for a few ns the bus shows addresses in
  between (going from 11DD to D1E2 it showed 11E2, the RAM test's address).
  The stock trigger compares the 16 lines every 15 ns and fires on those.
  While the RAM test writes every address from FFFF down, almost any
  trigger address can be faked like that. The zx trigger needs its address
  on 5 reads in a row (60 ns); a Z80 address stays 500 ns or more.
- **A finished capture was sent up to 2 s late** (the main loop looked every
  2 s), so the PC couldn't tell when a trigger came. Now it looks every ms.
- **Command 10** answers `ZX_FIRMWARE:1`, so the simulator can tell which
  firmware a board has (`--la-devices`). The stock firmware answers
  `ERR_UNKNOWN_MSG`.

Otherwise it is the same: same protocol, same version string
(`LOGIC_ANALYZER_PICO_2_V6_0`), and gusmanb's own program works with it.

## Flashing

Both boards need it. The simulator can restart a board into the Pico's USB
drive mode (no button):

```
c64-hw-sim --la-bootsel --la-ports COM12
```

A drive named **RP2350** appears: copy `LogicAnalyzer-zx.uf2` onto it, and
the board restarts with the new firmware. Then the other board. (Or hold
the Pico's BOOTSEL button while plugging it in.) `c64-hw-sim --la-devices`
shows `zx firmware` for each.

To go back, copy `LogicAnalyzer-stock.uf2` (gusmanb's source, unchanged,
built the same way) or one of his own releases the same way.

## Building

```
git clone -b version/v6_0 https://github.com/gusmanb/logicanalyzer.git gusmanb
py make_zx.py gusmanb/Firmware/LogicAnalyzer_V2 zx
sh build.sh zx
```

`build.sh` expects portable copies, in `J:/Coding/pico-tools` (change `T`
for elsewhere), of the versions gusmanb's releases use: the Arm GNU
toolchain 13.2.Rel1, CMake 3.28.6, Ninja 1.12.1, Pico SDK 2.0.0 (with
lib/tinyusb), and from raspberrypi/pico-sdk-tools v2.0.0-5 picotool and
pioasm. Built for the Pico 2, not overclocked (100 MHz top rate).
