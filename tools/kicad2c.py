#!/usr/bin/env python3
"""
kicad2c.py - turn a KiCad PCB file into plain C tables for c64-hw-sim.

Usage:
    python3 tools/kicad2c.py 250407_.kicad_pcb c64 > src/c64_board_data.c
    python3 tools/kicad2c.py Issue3B.kicad_pcb zx48 > src/zx48_board_data.c
    python3 tools/kicad2c.py ZX_Spectrum_i2.kicad_pcb zx48i2 > src/zx48i2_board_data.c
    python3 tools/kicad2c.py Issue1.kicad_pcb zx48i1 > src/zx48i1_board_data.c
    python3 tools/kicad2c.py Issue4B.kicad_pcb zx48i4b > src/zx48i4b_board_data.c
    python3 tools/kicad2c.py Issue6A.kicad_pcb zx48i6a > src/zx48i6a_board_data.c

The second argument names the board: the tables become the board_data_t
bd_<name>. Sources (see BOARDS below):
  c64     https://github.com/bwack/C64-250407-Replica-KiCad (CERN OHL v1.2)
  zx48    https://github.com/liveboxandy/ZX-Spectrum-48K-Issue-3B---Based-on-
          Gerbers-by-PABB-on-PCBWay (CC0 1.0)
  zx48i2  the ZX-Spectrum-48K-Issue-2 KiCad 10 project (CC0 1.0); some nets
          renamed and two parts added, see NET_RENAMES and EXTRAS
  zx48i1  the ZX-Spectrum-48K-Issue-1 KiCad 10 project (CC0 1.0); nets
          renamed as on the Issue 2
  zx48i4b the ZX-Spectrum-48K-Issue-4B KiCad 10 project
  zx48i6a the ZX-Spectrum-48K-Issue-6A KiCad 10 project (CC0 1.0); nets
          renamed as on the 4B, the links J1-J3 renamed, see fixup_zx48i6a

We extract only what the simulator needs:
  - every footprint (reference, value, footprint name),
  - every pad with its absolute position (mm), size and net,
  - the list of net names (KiCad escapes cleaned up),
  - the board outline bounding box (Edge.Cuts layer).
Nothing else (tracks, zones, silkscreen) is used.
"""
import math
import re
import sys


def tokenize(text):
    """Split a KiCad s-expression file into tokens: '(', ')', atoms, strings."""
    tok_re = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+')
    for m in tok_re.finditer(text):
        t = m.group(0)
        if t.startswith('"'):
            yield ('str', t[1:-1].replace('\\"', '"'))
        else:
            yield ('sym', t)


# Copper we don't use, dropped while parsing: the 250469's filled zones
# make its file 44 MB.
SKIP = {'zone', 'segment', 'via', 'arc'}


def parse(tokens):
    """Build nested Python lists from the token stream."""
    stack = [[]]
    for kind, t in tokens:
        if kind == 'sym' and t == '(':
            stack.append([])
        elif kind == 'sym' and t == ')':
            node = stack.pop()
            if len(stack) == 2 and node and node[0] in SKIP:
                continue
            stack[-1].append(node)
        else:
            stack[-1].append(t)
    return stack[0][0]


def children(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def child(node, name):
    c = children(node, name)
    return c[0] if c else None


# What each board is and where it comes from, for the generated file.
BOARDS = {
    'c64': ('C64 ASSY 250407 geometry and netlist, from bwack\'s KiCad replica',
            '(https://github.com/bwack/C64-250407-Replica-KiCad, CERN OHL v1.2).'),
    'c64c': ('C64 ASSY 250469 (Rev. 4) geometry and netlist, from bwack\'s KiCad replica',
             '(https://github.com/bwack/C64C-250469-KiCAD-Replica, CERN OHL v1.2).'),
    'zx48': ('ZX Spectrum 48K Issue 3B geometry and netlist, from liveboxandy\'s KiCad',
             'files (github.com/liveboxandy/ZX-Spectrum-48K-Issue-3B---Based-on-Gerbers-\n'
             ' * by-PABB-on-PCBWay, CC0 1.0), redrawn from PABB\'s gerbers of the board.'),
    'zx48i2': ('ZX Spectrum 48K Issue 2 geometry and netlist, from the ZX-Spectrum-48K-',
               'Issue-2 KiCad project (CC0 1.0), drawn from photos of a bare Issue 2 board.\n'
               ' * Some nets renamed as on the Issue 3B, and TR6 and C74 added (see kicad2c.py).'),
    'zx48i4b': ('ZX Spectrum 48K Issue 4B geometry and netlist, from the ZX-Spectrum-48K-',
                'Issue-4B KiCad project, drawn from a bare Issue 4B board (SRC168).'),
    'zx48i6a': ('ZX Spectrum 48K Issue 6A geometry and netlist, from the ZX-Spectrum-48K-',
                'Issue-6A KiCad project (CC0 1.0), drawn from an Issue 6A board (SRC168).\n'
                ' * Nets renamed as on the Issue 4B, the links J1-J3 renamed (see kicad2c.py).'),
    'zx48i1': ('ZX Spectrum 48K Issue 1 geometry and netlist, from the ZX-Spectrum-48K-',
               'Issue-1 KiCad project (CC0 1.0), drawn from a bare Issue 1 board (SRC089).\n'
               ' * Some nets renamed as on the Issue 3B (see kicad2c.py).'),
}

# Nets renamed to what the Issue 3B calls them, so that the model (and the
# probe readings people see) use the same names on both Spectrum boards.
NET_RENAMES = {
    'zx48i2': dict(
        [('Net-(IC1-A%d)' % i, 'LA%d' % i) for i in range(7)] +
        [('Net-(IC15-A%d)' % i, 'UA%d' % i) for i in range(7)] + [
            ('Net-(IC15-AR)', 'UA7'),
            ('Net-(IC1-/RAS)', '/LRAS'), ('Net-(IC1-/CAS)', '/LCAS'), ('Net-(IC1-/DRAMWE)', 'LDRAM_WE'),
            ('Net-(IC15-/RAS)', '/URAS'), ('Net-(IC15-/CAS)', '/UCAS'), ('Net-(IC15-/WRITE)', '/UWRITE'),
            ('Net-(IC25-SelectAB)', 'Net-(IC25-S)'), ('Net-(IC2-/CPUCLK)', 'Net-(IC2-/CLK)'),
            ('/IOREQULA', '/IORQULA'), ('/CLK', '/CK'), ('-12V', '-12VAC'),
        ]),
}
NET_RENAMES['zx48i1'] = NET_RENAMES['zx48i2']
# The Issue 6A names active low nets with an L at the end ('MREQL'). As
# the 4B, found by the pins on them. The edge connector's RFSH and the
# Z80's RFSHL are one net on the board (two labels in the schematic). The
# lower RAM's /CAS keeps a name of its own: the RAS/CAS link joins it to
# the ULA's. The ZX8401's own nets keep theirs.
NET_RENAMES['zx48i6a'] = dict(
    [('Net-(IC1-DMA%d)' % i, 'LA%d' % i) for i in range(7)] +
    [('Net-(IC15-A%d)' % i, 'UA%d' % i) for i in range(8)] + [
        ('BUSACKL', '/BUSACK'), ('BUSREQL', '/BUSREQ'), ('HALTL', '/HALT'), ('INTL', '/INT'), ('M1L', '/M1'),
        ('MREQL', '/MREQ'), ('NMIL', '/NMI'), ('RDL', '/RD'), ('WRL', '/WR'), ('RESETL', '/RESET'),
        ('ROMCSL', '/ROMCS'), ('WAITL', '/WAIT'), ('RFSH', '/RFSH'), ('RFSHL', '/RFSH'), ('PHICPU', '/CK'),
        ('IOREQL', '/IOREQ'), ('IORQULAL', '/IORQULA'),
        ('Net-(IC1-CASL)', '/LCAS'), ('Net-(IC1-RASL)', '/LRAS'), ('Net-(IC1-DRAMWEL)', 'LDRAM_WE'),
        ('Net-(IC1-INTL)', 'Net-(IC1-_INT)'), ('Net-(IC1-ROMCSL)', 'Net-(IC1-_ROMCS)'),
        ('Net-(IC1-VCC{slash}VAN)', 'Net-(IC1-Vcc2)'),
        ('Net-(IC15-CASL)', '/UCAS'), ('Net-(IC15-RASL)', '/URAS'), ('Net-(IC15-WRL)', '/UWRITE'),
    ])

# Parts on the real board that have no place on the PCB (fitted across
# other parts), added by hand: (ref, value, footprint, pads), each pad
# (name, x, y, net name). Placed in free space near where they are wired.
EXTRAS = {
    'zx48i2': [
        # A0 keeps the ULA's /IORQ high (as TR6 on the Issue 3B): base on
        # IC2 pin 30 (A0), collector on IC2 pin 11 (+5V), emitter on IC1
        # pin 33 (/IORQULA). Drawn in the gap between the glue chips.
        ('TR6', 'ZTX313', 'TO-92Flat (fitted across components)',
         [('1', 159.9, 62.0, '+5V'), ('2', 162.5, 62.0, 'A0'), ('3', 165.1, 62.0, '/IORQULA')]),
        # The DC-DC fix: + on TR5's emitter (C34 +), - on TR5's base.
        ('C74', '47uF', 'CP (fitted across components)',
         [('1', 226.5, 33.0, '+5V'), ('2', 233.0, 33.0, 'Net-(TR5-B)')]),
    ],
    'zx48i1': [
        # The clock pull-up a 6C001 needs (fitted with one: see zx48.c),
        # /CK (the ULA's pin 32) to +5V, as on the Issue 2. In the free row
        # between R25 and R1, next to TR3, with their 12.7 mm pitch.
        ('R73', '1K0', 'R (fitted with a 6C001)',
         [('1', 145.9, 73.8, '/CK'), ('2', 158.6, 73.8, '+5V')]),
    ],
}


def fixup_early(comps, board):
    """Make the Issues 1 and 2 drawn like the Issue 3B where its KiCad footprints
    differ for no reason that matters here: no mounting holes, the
    modulator and the PAL crystal without their metal cans' huge pads (as
    the 3B has them), the trimmers small (clear of their neighbours, as
    the board has them turned), C28 off J1 (Issue 2), the slanted resistors
    level (Issue 1)."""
    def scaled(pads, k, size):
        cx = sum(p[1] for p in pads) / len(pads)
        cy = sum(p[2] for p in pads) / len(pads)
        return [(p[0], cx + (p[1] - cx) * k, cy + (p[2] - cy) * k, size, size, p[5]) for p in pads]
    out = []
    for ref, value, fpname, pads in comps:
        if ref.startswith('H') and value == 'MountingHole':
            continue
        if ref == 'U1':         # the pins and the can's two lugs, as the 3B's TV1
            pads = [p if p[3] < 5 else (p[0], p[1], p[2], 5.715, 5.715, p[5]) for p in pads if p[3] < 6]
        elif ref == 'X2':       # the two legs (the can isn't simulated)
            pads = [p for p in pads if p[0] != '3']
        if ref in ('TC1', 'TC2'):
            pads = scaled(pads, 0.5, 1.2)
        elif ref in ('VR1', 'VR2'):
            pads = scaled(pads, 0.6, 1.4)
        elif ref == 'C28' and board == 'zx48i2':    # its + leg pulled in, clear of J1's mounting pin
            pads = [(p[0], 126.5 if p[0] == '2' else p[1], p[2], p[3], p[4], p[5]) for p in pads]
        elif ref in ('R47', 'R56', 'R63') and board == 'zx48i1':
            # Fitted at a slant: drawn level (a slanted part gets a big box).
            pads = [p if p[0] == '1' else (p[0], pads[0][1] + 12.7, pads[0][2], p[3], p[4], p[5]) for p in pads]
        if board == 'zx48i1':
            pads = layout_zx48i1(ref, fpname, pads)
        out.append((ref, value, fpname, pads))
    return out


# The Issue 1 moved apart where its parts and their names crowd each
# other on screen (the nets stay as they are): mm to move each part by.
I1_MOVES = {
    'C45': (0, 2.6),                            # off R64's leg, its name clear
    'LS1': (-2.5, 0),                           # clear of D9
    'C1': (0, -1.5), 'C2': (0, -1.5), 'C3': (0, -1.5), 'C4': (0, -1.5),     # clear of the RAMs' names
    'VidU1': (-1.2, 0), 'VidV1': (-1.2, 0), 'VidY1': (-1.2, 0),             # clear of R34
    'R34': (0, 1.5),                            # clear of C29
    'TR1': (0, 2.0),                            # its top at TR2's bottom
    'EXT2': (-3.0, 0), 'R61': (-2.0, 0),        # clear of the parts on their right
}
# Standing parts whose tops line up with R43's (y 76.4), as on the board.
I1_TOP = 76.4
I1_LINE_UP = ('C48', 'R40', 'C36', 'R41')


def layout_zx48i1(ref, fpname, pads):
    # The capacitors with a footprint for 5 mm and 12.7 mm legs (pad 2
    # twice): only the 5 mm pair, as fitted, so the part and its name are
    # its size.
    if ref.startswith('C') and sum(p[0] == '2' for p in pads) == 2:
        one = next(p for p in pads if p[0] == '1')
        near = min((p for p in pads if p[0] == '2'), key=lambda p: math.hypot(p[1] - one[1], p[2] - one[2]))
        pads = [one, near]
    dx, dy = I1_MOVES.get(ref, (0, 0))
    if ref in I1_LINE_UP:
        dy = I1_TOP - min(p[2] for p in pads)
    return [(p[0], p[1] + dx, p[2] + dy, p[3], p[4], p[5]) for p in pads]


def fixup_zx48i4b(comps, board):
    """The Issue 4B: no mounting hole (H1, a pad on the 9 V input's 0 V),
    and the ferrite links L1 and L2 (fitted slanted, crossing) upright side
    by side between C101 and T1, and TR2's pads the size of the other
    transistors'."""
    out = []
    for ref, value, fpname, pads in comps:
        if value.startswith('MountingHole'):
            continue
        if ref in ('L1', 'L2'):
            x = 236.8 if ref == 'L1' else 239.8
            pads = [(p[0], x, p[2], p[3], p[4], p[5]) for p in pads]
        elif ref == 'TR2':      # its pads as the other transistors' (1.6 mm)
            pads = [(p[0], p[1], p[2], 1.6, 1.6, p[5]) for p in pads]
        out.append((ref, value, fpname, pads))
    return out


def fixup_zx48i6a(comps, board):
    """The Issue 6A: the 4B's bare PCB (see fixup_zx48i4b). Its links
    have connector names in the KiCad project: the RAS/CAS link J1 (a 2x3
    header, footprint RASCASLINK) is LK5 here, next to LK1-LK4; J2 and J3
    are LKV and LKU, their values (the U and V video lines to the edge
    connector)."""
    names = {'J1': 'LK5', 'J2': 'LKV', 'J3': 'LKU'}
    return [(names.get(ref, ref), value, fpname, pads) for ref, value, fpname, pads in fixup_zx48i4b(comps, board)]


def fixup_c64c(comps, board):
    """The 250469: no mounting holes, no logos (footprints without a
    reference or pads)."""
    return [c for c in comps if c[0] and c[3] and not c[0].startswith('HOLE')]


FIXUPS = {'c64c': fixup_c64c, 'zx48i2': fixup_early, 'zx48i1': fixup_early, 'zx48i4b': fixup_zx48i4b, 'zx48i6a': fixup_zx48i6a}


# Active low signals on the Spectrum board are named with underscores:
# '_M_REQ' is /MREQ, '_IO_RQULA' /IORQULA.
UNDERSCORE_BAR = False
# The 250469 writes a / in a name as {slash}.
SLASH_NAMES = False


def clean_net(name):
    """KiCad net names -> readable names: '/sheet/~{AEC}' -> '/AEC'."""
    name = re.sub(r'^/250407_sheet2/', '', name)
    name = re.sub(r'^/', '', name)
    name = name.replace('\\\\', '\\')
    # ~{X} means "X with an overbar" (active low): render as /X
    name = re.sub(r'~\{([^}]*)\}', r'/\1', name)
    name = name.replace('\\', '')
    if SLASH_NAMES:     # 'R{slash}~{W}' -> 'R/W', '+9V{slash}12V' -> '+9V/12V'
        name = name.replace('{slash}/', '/').replace('{slash}', '/')
    if UNDERSCORE_BAR and name.startswith('_'):
        name = '/' + name[1:].replace('_', '')
    return name


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in BOARDS:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    board = sys.argv[2]
    global UNDERSCORE_BAR, SLASH_NAMES
    UNDERSCORE_BAR = board in ('zx48', 'zx48i4b')     # their nets are named '_M_REQ'
    SLASH_NAMES = board == 'c64c'
    pcb = parse(tokenize(open(sys.argv[1], encoding='utf-8').read()))

    # Net table (index 0 is the unconnected net in KiCad). KiCad 10 has no
    # table: pads name their net, numbered here in order of appearance.
    renames = NET_RENAMES.get(board, {})
    nets = {}
    unconnected = set()
    for n in children(pcb, 'net'):
        name = clean_net(n[2])
        nets[int(n[1])] = renames.get(name, name)
        if name.startswith('unconnected-('):        # a pad on its own: no net
            unconnected.add(int(n[1]))
    by_name = {}

    def net_index(net):
        if not net:
            return 0
        if len(net) > 2 or net[1].isdigit():    # (net 12 "name")
            return 0 if int(net[1]) in unconnected else int(net[1])
        name = clean_net(net[1])                # (net "name")
        if name.startswith('unconnected-('):    # a pad on its own: no net
            return 0
        name = renames.get(name, name)
        if name not in by_name:
            by_name[name] = len(nets) + len(by_name) + 1
            nets[by_name[name]] = name
        return by_name[name]

    def named_net(name):
        """The index of an existing net, by its (renamed) name."""
        for idx, n in nets.items():
            if n == name:
                return idx
        sys.exit('kicad2c: no net %r for an extra part' % name)

    # Board outline: bounding box of everything on Edge.Cuts.
    xs, ys = [], []
    for kind in ('gr_line', 'gr_rect', 'gr_arc'):
        for g in children(pcb, kind):
            layer = child(g, 'layer')
            if not layer or layer[1] != 'Edge.Cuts':
                continue
            for key in ('start', 'end', 'mid'):
                p = child(g, key)
                if p:
                    xs.append(float(p[1]))
                    ys.append(float(p[2]))

    comps = []
    for fp in children(pcb, 'footprint'):
        at = child(fp, 'at')
        fx, fy = float(at[1]), float(at[2])
        frot = float(at[3]) if len(at) > 3 else 0.0
        ref = value = '?'
        for prop in children(fp, 'property'):
            if prop[1] == 'Reference':
                ref = prop[2]
            elif prop[1] == 'Value':
                value = prop[2]
        for text in children(fp, 'fp_text'):    # KiCad 7 and older
            if text[1] == 'reference':
                ref = text[2]
            elif text[1] == 'value':
                value = text[2]
        if ref.startswith('REF'):          # mounting holes
            continue
        a = math.radians(frot)
        pads = []
        for pad in children(fp, 'pad'):
            pat = child(pad, 'at')
            px, py = float(pat[1]), float(pat[2])
            # KiCad: pad offsets are in footprint space, rotate them by the
            # footprint angle (counter-clockwise on screen, y axis down).
            ax = fx + px * math.cos(a) + py * math.sin(a)
            ay = fy - px * math.sin(a) + py * math.cos(a)
            size = child(pad, 'size')
            w, h = float(size[1]), float(size[2])
            if round(frot) % 180 == 90:
                w, h = h, w
            netidx = net_index(child(pad, 'net'))
            pads.append((pad[1], ax, ay, w, h, netidx))
        comps.append((ref, value, child(fp, 'footprint') or fp[1], pads))
    for ref, value, fpname, pads in EXTRAS.get(board, []):
        comps.append((ref, value, fpname,
                      [(name, x, y, 1.4, 1.4, named_net(net)) for name, x, y, net in pads]))
    if board in FIXUPS:
        comps = FIXUPS[board](comps, board)

    # Renumber nets densely, dropping the "no net" entry.
    used = sorted({p[5] for c in comps for p in c[3] if p[5] != 0})
    remap = {old: i for i, old in enumerate(used)}

    out = sys.stdout
    out.write('/* %s_board_data.c - GENERATED by tools/kicad2c.py, do not edit.\n' % board)
    out.write(' * %s\n * %s */\n' % BOARDS[board])
    out.write('#include "board_data.h"\n\n')
    out.write('static const char *const nets[] = {\n')
    for old in used:
        out.write('    "%s",\n' % nets[old].replace('"', '\\"'))
    out.write('};\n\n')

    out.write('static const bd_pad_t pads[] = {\n')
    first = []
    n = 0
    for ref, value, fpname, pads in comps:
        first.append(n)
        for name, x, y, w, h, net in pads:
            out.write('    { "%s", %.3ff, %.3ff, %.3ff, %.3ff, %d },\n' %
                      (name, x, y, w, h, remap[net] if net else -1))
            n += 1
    out.write('};\n\n')

    out.write('static const bd_comp_t comps[] = {\n')
    for i, (ref, value, fpname, pads) in enumerate(comps):
        fpname = fpname.split(':')[-1]
        out.write('    { "%s", "%s", "%s", %d, %d },\n' %
                  (ref, value.replace('"', '\\"'), fpname, first[i], len(pads)))
    out.write('};\n\n')
    out.write('const board_data_t bd_%s = {\n' % board)
    out.write('    { %.3ff, %.3ff, %.3ff, %.3ff },\n' % (min(xs), min(ys), max(xs), max(ys)))
    out.write('    nets, %d, pads, %d, comps, %d,\n};\n' % (len(used), n, len(comps)))


if __name__ == '__main__':
    main()
