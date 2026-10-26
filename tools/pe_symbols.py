"""Dump PE32 exports and imports to TSV.

Usage: python3 -I tools/pe_symbols.py <out_dir> <pe> [<pe> ...]
Writes <out_dir>/<module>.exports.tsv (ordinal, rva, name)
and    <out_dir>/<module>.imports.tsv (dll, ordinal_or_hint, name).
"""
import os
import struct
import sys


class PE:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        o = struct.unpack_from('<I', d, 0x3c)[0]
        if d[o:o + 4] != b'PE\0\0':
            raise ValueError(f'{path}: not a PE file')
        nsec = struct.unpack_from('<H', d, o + 6)[0]
        optsize = struct.unpack_from('<H', d, o + 20)[0]
        self.opt = o + 24
        if struct.unpack_from('<H', d, self.opt)[0] != 0x10b:
            raise ValueError(f'{path}: not PE32')
        self.image_base = struct.unpack_from('<I', d, self.opt + 28)[0]
        self.sections = []
        for i in range(nsec):
            b = self.opt + optsize + i * 40
            vsize, va, rsize, roff = struct.unpack_from('<IIII', d, b + 8)
            self.sections.append((va, max(vsize, rsize), roff))

    def off(self, rva):
        for va, size, roff in self.sections:
            if va <= rva < va + size:
                return rva - va + roff
        raise ValueError(f'rva {rva:#x} not in any section')

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o:self.data.index(b'\0', o)].decode('latin1')

    def directory(self, index):
        return struct.unpack_from('<II', self.data, self.opt + 96 + index * 8)

    def exports(self):
        rva, size = self.directory(0)
        if not rva:
            return []
        d = self.data
        e = self.off(rva)
        base, nfuncs, nnames, funcs, names, ords = struct.unpack_from('<6I', d, e + 16)
        named = {}
        for i in range(nnames):
            name_rva = struct.unpack_from('<I', d, self.off(names) + i * 4)[0]
            idx = struct.unpack_from('<H', d, self.off(ords) + i * 2)[0]
            named[idx] = self.cstr(name_rva)
        out = []
        for i in range(nfuncs):
            frva = struct.unpack_from('<I', d, self.off(funcs) + i * 4)[0]
            if not frva:
                continue
            name = named.get(i, '')
            if rva <= frva < rva + size:  # forwarder
                name = f'{name} -> {self.cstr(frva)}'
            out.append((base + i, frva, name))
        return out

    def imports(self):
        rva, _ = self.directory(1)
        if not rva:
            return []
        d = self.data
        out = []
        io = self.off(rva)
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from('<5I', d, io)
            if not name_rva:
                break
            dll = self.cstr(name_rva)
            t = self.off(oft or ft)
            while True:
                v = struct.unpack_from('<I', d, t)[0]
                if not v:
                    break
                if v & 0x80000000:
                    out.append((dll, f'#{v & 0xffff}', ''))
                else:
                    o = self.off(v)
                    hint = struct.unpack_from('<H', d, o)[0]
                    out.append((dll, str(hint), self.cstr(v + 2)))
                t += 4
            io += 20
        return out


def main():
    out_dir, paths = sys.argv[1], sys.argv[2:]
    os.makedirs(out_dir, exist_ok=True)
    for p in paths:
        pe = PE(p)
        mod = os.path.splitext(os.path.basename(p))[0]
        with open(os.path.join(out_dir, f'{mod}.exports.tsv'), 'w') as f:
            f.write('ordinal\trva\tname\n')
            for ordinal, frva, name in pe.exports():
                f.write(f'{ordinal}\t{frva:#010x}\t{name}\n')
        with open(os.path.join(out_dir, f'{mod}.imports.tsv'), 'w') as f:
            f.write('dll\thint\tname\n')
            for dll, hint, name in pe.imports():
                f.write(f'{dll}\t{hint}\t{name}\n')
        print(f'{mod}: {len(pe.exports())} exports, {len(pe.imports())} imports')


if __name__ == '__main__':
    main()
