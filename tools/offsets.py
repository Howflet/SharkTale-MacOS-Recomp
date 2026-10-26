"""List distinct `this + 0xNN` / `param_N + 0xNN` accesses per function in a
DecompileMatching.java dump, with one sample line each.

Usage: python3 -I tools/offsets.py <dump.c> <function-substring> [var]
  var defaults to 'this'.
"""
import re
import sys


def functions(path):
    name, body = None, []
    for line in open(path, encoding='utf-8'):
        m = re.match(r'^// (\S+) @ ([0-9a-f]+)', line)
        if m:
            if name:
                yield name, body
            name, body = f'{m.group(1)} @ {m.group(2)}', []
        elif name:
            body.append(line.rstrip())
    if name:
        yield name, body


def main():
    path, needle = sys.argv[1], sys.argv[2]
    var = sys.argv[3] if len(sys.argv) > 3 else 'this'
    pat = re.compile(rf'\b{re.escape(var)} \+ (0x[0-9a-f]+|\d+)\)')
    for name, body in functions(path):
        if needle not in name:
            continue
        seen = {}
        for line in body:
            for m in pat.finditer(line):
                off = int(m.group(1), 0)
                seen.setdefault(off, line.strip()[:110])
        print(f'== {name}')
        for off in sorted(seen):
            print(f'  {off:#06x}  {seen[off]}')


if __name__ == '__main__':
    main()
