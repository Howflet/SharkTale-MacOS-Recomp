"""List UTF-16LE strings (>= 4 chars) in a PE's data with their VAs.

Usage: python3 -I tools/wstrings.py <pe> [min_len]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_symbols import PE  # noqa: E402

pe = PE(sys.argv[1])
n = int(sys.argv[2]) if len(sys.argv) > 2 else 4
for va, size, roff in pe.sections:
    blob = pe.data[roff:roff + size]
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % n, blob):
        if m.start() % 2:
            continue
        print(f'{pe.image_base + va + m.start():#010x}  {m.group().decode("utf-16-le")}')
