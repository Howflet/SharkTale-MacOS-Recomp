"""Annotate guest addresses (0x1xxxxxxx) in text with module!function+offset.

  build/sharktale ... 2>&1 | python3 -I tools/symbolize.py

Uses symbols/<Module>.functions.tsv (original addresses) and the relocation
deltas from tools/recomp_layout.txt.
"""
import bisect
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from pe_symbols import PE  # noqa: E402

starts, names = [], []
for line in open(os.path.join(ROOT, 'tools', 'recomp_layout.txt')):
    if line.startswith('#') or not line.split():
        continue
    f, base = line.split()
    mod = os.path.splitext(f)[0]
    pe = PE(os.path.join(ROOT, 'orig', 'System', f))
    delta = int(base, 16) - pe.image_base
    tsv = os.path.join(ROOT, 'symbols', f'{mod}.functions.tsv')
    if not os.path.exists(tsv):
        continue
    for row in open(tsv):
        p = row.rstrip('\n').split('\t')
        if p[0] == 'entry':
            continue
        starts.append(int(p[0], 16) + delta)
        names.append(f'{mod}!{p[3]}')
order = sorted(range(len(starts)), key=starts.__getitem__)
starts = [starts[i] for i in order]
names = [names[i] for i in order]


def sym(m):
    a = int(m.group(0), 16)
    i = bisect.bisect_right(starts, a) - 1
    if i < 0 or a - starts[i] > 0x4000:
        return m.group(0)
    return f'{m.group(0)} <{names[i]}+{a - starts[i]:#x}>'


for line in sys.stdin:
    sys.stdout.write(re.sub(r'\b(?:0x)?1[01][0-9a-f]{6}\b', sym, line))
