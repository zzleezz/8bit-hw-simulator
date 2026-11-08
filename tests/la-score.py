#!/usr/bin/env python3
"""tests/la-score.py - how right the logic analyser's verdicts are.

    py -I tests/la-score.py OUT/all.txt [tests/la-faults.txt]

Reads the regression's verdicts (tests/la-diagnose.sh's all.txt) and, for
each case whose real fault is known, says where the verdict's list ("What
to look at, most likely first") first names the faulty part: first, in
the top 3, later, or not at all. "In the first item" means it is named
there, not that it is the only part named.

The fault is known:
  - for a simulated fault inside a chip (removed, dataN=, addrN, flip@,
    nocontention): that chip, from the fault itself;
  - for the healthy board: no fault (it should pass);
  - for anything listed in tests/la-faults.txt: the parts given there
    (a real folder's fault as found at the bench, or a pin fault's).
Pin shorts and opens of the simulated cases aren't scored unless listed:
the right answer there is often the net, not the chip.

la-compare.py says what changed since the last run; this says whether
it's right. AI-generated (Claude Code).
"""
import os
import re
import sys

CHIP_FAULT = re.compile(r'^([A-Z]+[0-9]+):(removed|data\d|addr\d|flip@|nocontention)')


def read_faults(path):
    """tests/la-faults.txt: 'CASE  PART [PART...]  # note'; PART 'none' for
    a working board. Lines starting # are comments."""
    known = {}
    if not path or not os.path.exists(path):
        return known
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        w = line.split()
        if len(w) >= 2:
            known[w[0]] = [p for p in w[1:]]
    return known


def cases(all_txt):
    """(kind, name, lines) for each '######## kind name' block."""
    cur = None
    for line in open(all_txt, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('######## '):
            if cur:
                yield cur
            k = line[9:].split(' ', 1)
            cur = (k[0], k[1] if len(k) > 1 else '', [])
        elif cur:
            cur[2].append(line)
    if cur:
        yield cur


def items(lines):
    """The verdict's numbered items, each as one string (its lines joined),
    up to the check's or the screen's own lists."""
    out, inside = [], False
    for line in lines:
        if line.startswith(('Checked against', 'The screen (', 'From the screen')):
            break
        m = re.match(r'^  (\d+)\. ', line)
        if m:
            out.append(line)
            inside = True
        elif inside and line.startswith('     '):
            out[-1] += ' ' + line.strip()
        else:
            inside = False
    return out


def names(item, part):
    return re.search(r'(?<![A-Za-z0-9])' + re.escape(part) + r'(?![0-9])', item) is not None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    known = read_faults(sys.argv[2] if len(sys.argv) > 2 else
                        os.path.join(os.path.dirname(os.path.abspath(__file__)), 'la-faults.txt'))
    tally = {}
    rows = []
    for kind, name, lines in cases(sys.argv[1]):
        key = name if kind == 'real' else kind + ' ' + name
        parts = known.get(key) or known.get(name if kind != 'real' else None)
        if parts is None:
            if name == 'healthy':
                parts = ['none']
            else:
                m = CHIP_FAULT.match(name)
                if not m or kind == 'real':
                    continue
                parts = [m.group(1)]
        it = items(lines)
        if parts == ['none']:
            # Right: no items, or the first says it's no fault on the
            # Spectrum (the analyser's boards swapped).
            ok = not it or 'not a fault' in it[0]
            res = 'right (passes)' if ok else 'wrong (a fault named)'
        elif not it:
            res = 'missed (no fault found)'
        else:
            at = next((i for i, x in enumerate(it) if any(names(x, p) for p in parts)), None)
            res = ('not named' if at is None else 'in the first item' if at == 0 else 'in the top 3' if at < 3 else 'later')
        tally[res] = tally.get(res, 0) + 1
        rows.append((key, ' '.join(parts), res))
    if not rows:
        print('Nothing to score: no case with a known fault.')
        return 0
    w = max(len(r[0]) for r in rows)
    for key, parts, res in rows:
        print(f'  {key:<{w}}  {parts:<12} {res}')
    print()
    order = ['in the first item', 'right (passes)', 'in the top 3', 'later', 'not named', 'missed (no fault found)',
             'wrong (a fault named)']
    print(f'{len(rows)} cases with a known fault: ' +
          ', '.join(f'{tally[k]} {k}' for k in order if k in tally))
    return 0


if __name__ == '__main__':
    sys.exit(main())
