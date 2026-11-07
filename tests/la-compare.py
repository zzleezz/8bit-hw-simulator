"""tests/la-compare.py EXPECTED NEW - what changed in the logic analyser
diagnosis's verdicts between two runs of tests/la-diagnose.sh, case by
case (NEW: a case only in the new run; GONE: only in the old; CHANGED: the
lines that differ). The stage lines ("2 of 6...", "OK", "FAULT") are left
out: the verdict below them says the same. Exit 1 if anything changed.

AI-generated (Claude Code)."""
import sys, re, difflib


def load(p):
    d, cur = {}, None
    for l in open(p, encoding='utf-8', errors='replace'):
        l = l.rstrip('\n')
        if l.startswith('######## '):
            cur = l[9:]
            d[cur] = []
            continue
        if cur is None:
            continue
        if re.match(r'^[0-9] of 6|^  (FAULT|Wrong too|It never|OK|CLK is)', l):
            continue
        d[cur].append(l)
    return d


a, b = load(sys.argv[1]), load(sys.argv[2])
changed = 0
for k in b:
    if k not in a:
        print('NEW', k)
        print('\n'.join(b[k]))
        print()
        changed += 1
        continue
    if a[k] != b[k]:
        print('CHANGED', k)
        for l in difflib.unified_diff(a[k], b[k], lineterm='', n=0):
            if not l.startswith(('---', '+++', '@@')):
                print(l)
        print()
        changed += 1
for k in a:
    if k not in b:
        print('GONE', k)
        changed += 1
print('%d of %d cases changed' % (changed, len(b)) if changed else 'No change in %d cases' % len(b))
sys.exit(1 if changed else 0)
