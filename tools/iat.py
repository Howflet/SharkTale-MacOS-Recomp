"""Map IAT slot addresses to imported names for a PE32 module.

Usage: python3 -I tools/iat.py <pe> [hex_va ...]   (no VAs: print the whole table)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_symbols import PE  # noqa: E402


def iat(pe):
    rva, _ = pe.directory(1)
    out, io = {}, pe.off(rva)
    while True:
        oft, _, _, name_rva, ft = struct.unpack_from('<5I', pe.data, io)
        if not name_rva:
            return out
        dll, t, slot = pe.cstr(name_rva), pe.off(oft or ft), ft
        while True:
            v = struct.unpack_from('<I', pe.data, t)[0]
            if not v:
                break
            out[pe.image_base + slot] = f'{dll}!' + (f'#{v & 0xffff}' if v & 0x80000000 else pe.cstr(v + 2))
            t += 4
            slot += 4
        io += 20


if __name__ == '__main__':
    table = iat(PE(sys.argv[1]))
    for va in sys.argv[2:] or [f'{k:x}' for k in sorted(table)]:
        print(va, table.get(int(va, 16), '?'))
