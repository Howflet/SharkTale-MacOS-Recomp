"""Dump a class vtable (??_7<Class>@@6B@) from a module, naming slots via symbols.db.

Usage: python3 -I tools/vtable.py <module> <Class> [max_slots]
"""
import os
import sqlite3
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_symbols import PE  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    module, cls = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    db = sqlite3.connect(os.path.join(ROOT, 'symbols', 'symbols.db'))
    file, = db.execute('SELECT file FROM modules WHERE name=?', (module,)).fetchone()
    row = db.execute('SELECT va FROM exports WHERE module=? AND mangled=?',
                     (module, f'??_7{cls}@@6B@')).fetchone()
    if not row:
        sys.exit(f'no exported vftable for {cls} in {module}')
    names = dict(db.execute('SELECT va, demangled FROM exports'))
    pe = PE(os.path.join(ROOT, 'orig', 'System', file))
    for slot in range(limit):
        va = row[0] + slot * 4
        target = struct.unpack_from('<I', pe.data, pe.off(va - pe.image_base))[0]
        # Stop at the next vtable's RTTI/data or a zero entry.
        if target == 0 or (target in names and '`vftable' in names[target]):
            break
        print(f'{slot * 4:#05x}  {target:#010x}  {names.get(target, "?")}')


if __name__ == '__main__':
    main()
