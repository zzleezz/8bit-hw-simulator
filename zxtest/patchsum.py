# patchsum.py - put the ROM's CRC into zxtest.rom at CSUM (3F7E), the way
# test 2 checks it (from version 6): the CRC-16/CCITT-FALSE (poly 1021,
# from FFFF, no reflection, no final XOR) of 0000 up to CSUM, low byte
# first. Versions 1-5 stored a 16-bit sum of 0000-3F7F but CSUM.
# Then build the SteamCard Pro image if the original dump is given.
# AI-generated (Claude Code).
#   py -I patchsum.py zxtest.rom [dump.bin out.bin]
import sys
CSUM = 0x3F7E


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021 if c & 0x8000 else c << 1) & 0xFFFF
    return c


assert crc16(b"123456789") == 0x29B1     # CRC-16/CCITT-FALSE's check value
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
assert len(d) == 16384, len(d)
assert all(b == 0xFF for b in d[0x3F80:]), "something at 3F80-3FFF"
s = crc16(d[:CSUM])
d[CSUM], d[CSUM + 1] = s & 0xFF, s >> 8
open(p, 'wb').write(d)
print("ROM CRC %04X at %04X" % (s, CSUM))
if len(sys.argv) > 3:
    img = bytearray(open(sys.argv[2], 'rb').read())
    assert len(img) == 65536
    bench = open('zxbench.rom', 'rb').read()
    img[0x8000:0xC000] = bench          # bank 2: the bench ROM
    img[0xC000:0x10000] = d             # bank 3: the test ROM
    open(sys.argv[3], 'wb').write(img)
    print("image %s: bank 0, 1 from %s, 2 zxbench, 3 zxtest" % (sys.argv[3], sys.argv[2]))
