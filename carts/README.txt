The cartridge images
====================

deadtest-781220.crt, diag-586220.crt
    C64 diagnostic cartridges (see LICENSE).

zx-diagnostics-v0.37.rom
    ZX Diagnostics for the Spectrum (see zx-diagnostics.LICENSE).

zxtest.rom
    Our own test ROM for 16K/48K Spectrums, made for the logic analyser
    (its source is in zxtest/ in the repository). zxtest.rom is always the
    current version: the program looks for it by this name. The version
    is inside the ROM and shows on its ALIVE line on the screen. In V1.3
    it is version 7.

zxtest-v3.rom, zxtest-v4.rom ... (in the repository, not the download)
    Older versions of the test ROM, kept so that captures taken with an
    older version are still diagnosed against the code that made them.
    When a new version is built, the previous zxtest.rom is kept as
    zxtest-vN.rom. Nobody needs them for new captures: put
    zxtest.rom on the cartridge.
