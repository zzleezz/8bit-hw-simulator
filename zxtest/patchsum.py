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
assert all(b == 0xFF for b in d[0x3F80:]), "something at 3F80-3FFF"
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
