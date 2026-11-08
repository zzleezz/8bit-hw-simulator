# patchsum.py - put the ROM's sum into zxtest.rom at CSUM (3F7E), the way
# test 2 checks it: the 16-bit sum of 0000-3F7F but the two bytes at CSUM.
# Then build the SteamCard Pro image if the original dump is given.
# AI-generated (Claude Code).
#   py -I patchsum.py zxtest.rom [dump.bin out.bin]
import sys
CSUM = 0x3F7E
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
assert len(d) == 16384, len(d)
# self-checks of the code's hand-counted jumps: RESULT's "jr z,$+9" and
# FAILBITS' "jr nz,$+8"
assert d.count(bytes([0xD9, 0xCB, 0x40, 0xD9, 0x28, 0x07, 0x22])) == 8, "RESULT"
assert d.count(bytes([0xFD, 0xB5, 0x20, 0x06, 0x7C, 0xFD, 0x67, 0x7D, 0xFD, 0x6F])) == 3, "FAILBITS"
assert all(b == 0xFF for b in d[0x3F80:]), "something at 3F80-3FFF"
# INTBACK's hand-counted jumps, at 0038 and 3A3A: jr z,$+9 / jr z,$+5 land on JP (IX)
for at in (0x38, 0x3A3A):
    assert d[at + 6] == 0x28 and d[at + 7] == 7 and d[at + 10] == 0x28 and d[at + 11] == 3 and d[at + 15:at + 17] == bytes([0xDD, 0xE9]), "INTBACK at %04X" % at
d[CSUM] = d[CSUM + 1] = 0
s = sum(d[:0x3F80]) & 0xFFFF      # test 2 sums 0000-3F7F
d[CSUM], d[CSUM + 1] = s & 0xFF, s >> 8
open(p, 'wb').write(d)
print("ROM sum %04X at %04X" % (s, CSUM))
if len(sys.argv) > 3:
    img = bytearray(open(sys.argv[2], 'rb').read())
    assert len(img) == 65536
    bench = open('zxbench.rom', 'rb').read()
    img[0x8000:0xC000] = bench          # bank 2: the bench ROM
    img[0xC000:0x10000] = d             # bank 3: the test ROM
    open(sys.argv[3], 'wb').write(img)
    print("image %s: bank 0, 1 from %s, 2 zxbench, 3 zxtest" % (sys.argv[3], sys.argv[2]))
