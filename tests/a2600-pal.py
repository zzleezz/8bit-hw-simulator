"""Run the PAL bench cartridge and check its raster and pin faults.

Usage from the repository root: python tests/a2600-pal.py ./c64-hw-sim
"""
from itertools import groupby
from pathlib import Path
from struct import unpack_from
from subprocess import run
import sys
from tempfile import TemporaryDirectory
from zlib import decompress


def scanlines(path):
    """Read the unfiltered RGBA scanlines written by src/png.c."""
    png = path.read_bytes()
    assert png.startswith(b"\x89PNG\r\n\x1a\n")
    width, height = unpack_from(">II", png, 16)
    assert (width, height) == (160, 312)
    pos = 8
    data = bytearray()
    while pos < len(png):
        size, kind = unpack_from(">I4s", png, pos)
        if kind == b"IDAT":
            data.extend(png[pos + 8:pos + 8 + size])
        pos += 12 + size
    pixels = decompress(data)
    stride = 1 + width * 4
    assert len(pixels) == stride * height and not any(pixels[::stride])
    return [pixels[start + 1:start + stride] for start in range(0, len(pixels), stride)]


def capture(binary, shot, *args):
    run([str(binary), "carts/pal-bench.a26", "--headless", "5",
         "--shot", str(shot), *args], check=True)
    return scanlines(shot)


def bars(lines):
    """Return (RGB, height) for each horizontal band."""
    for line in lines:
        assert line == line[:4] * 160
    return [(rgb, len(list(rows))) for rgb, rows in groupby(line[:3] for line in lines)]


def main():
    binary = Path(sys.argv[1]).resolve()
    with TemporaryDirectory() as tmp:
        shot = Path(tmp) / "pal.png"
        healthy = bars(capture(binary, shot))
        assert [count for _, count in healthy] == [48] + [19] * 12 + [36]
        assert len({rgb for rgb, _ in healthy[1:-1]}) == 12
        assert sum(len(set(rgb)) > 1 for rgb, _ in healthy) == 11

        failed = bars(capture(binary, shot, "--fault", "A202:data7=0"))
        assert failed == [(b"\0\0\0", 48 + 19)] + [(b"\xff\xff\xff", 19),
                            (b"\0\0\0", 19)] * 5 + [(b"\xff\xff\xff", 19), (b"\0\0\0", 36)]

        no_colour = bars(capture(binary, shot, "--fault", "A201.9:open"))
        assert len(no_colour) > 2 and all(len(set(rgb)) == 1 for rgb, _ in no_colour)

        no_sync = capture(binary, shot, "--fault", "A201.2:open")
        assert len(set(no_sync)) > 300
    print("PAL raster, RIOT RAM fault, TIA colour and sync checks passed.")


if __name__ == "__main__":
    main()
