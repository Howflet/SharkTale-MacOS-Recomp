"""Export flattened class/struct layouts (inherited fields included) to JSON for Ghidra.

  python3 -I tools/export_layouts.py > symbols/layouts.json

Each entry: {"name", "size", "fields": [{"name", "offset", "size", "type"}]}, where
type is one of int, float, byte, ptr, name, bool, struct:<F-name>, blob.
"""
import json
import os
import sqlite3

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB = sqlite3.connect(os.path.join(ROOT, 'symbols', 'symbols.db'))

UOBJECT_CPP = [('VfTable', 0, 4, 'ptr'), ('Index', 4, 4, 'int'), ('HashNext', 8, 4, 'ptr'),
               ('StateFrame', 12, 4, 'ptr'), ('_Linker', 16, 4, 'ptr'), ('_LinkerIndex', 20, 4, 'int')]

TYPE_MAP = {'IntProperty': 'int', 'FloatProperty': 'float', 'ByteProperty': 'byte',
            'ObjectProperty': 'ptr', 'ClassProperty': 'ptr', 'PointerProperty': 'ptr',
            'NameProperty': 'name', 'BoolProperty': 'bool', 'StrProperty': 'blob',
            'ArrayProperty': 'blob', 'DelegateProperty': 'blob'}


def script_fields(script_name):
    rows = DB.execute('SELECT name, type, detail, offset, size, array_dim, bitmask FROM script_members '
                      'WHERE owner=? ORDER BY offset, rowid', (script_name,)).fetchall()
    out = []
    for name, typ, detail, off, size, dim, bit in rows:
        if bit and bit != 1:
            continue  # later bits share the first bool's DWORD
        t = f'struct:F{detail}' if typ == 'StructProperty' else TYPE_MAP.get(typ, 'blob')
        out.append({'name': name, 'offset': off, 'size': size * dim, 'type': t})
    return out


EXTRAS = {k: v for k, v in json.load(open(os.path.join(ROOT, 'tools', 'layout_extras.json'))).items()
          if not k.startswith('_')}


def main():
    classes = {cpp: (script, size, base) for cpp, script, size, base in
               DB.execute('SELECT cpp, script, size, base FROM classes')}
    layouts = []
    for cpp, (script, size, base) in sorted(classes.items()):
        chain, c = [], cpp
        while c in classes and c not in chain:
            chain.append(c)
            c = classes[c][2]
        fields = []
        for c in reversed(chain):
            if c == 'UObject':
                fields += [{'name': n, 'offset': o, 'size': s, 'type': t} for n, o, s, t in UOBJECT_CPP]
                fields += [f for f in script_fields('Object') if f['offset'] >= 0x18]
            else:
                start = classes[classes[c][2]][1] if classes[c][2] in classes else 0
                fields += [f for f in script_fields(classes[c][0]) if f['offset'] >= start
                           and f['offset'] + f['size'] <= classes[c][1]]
        fields += [{'name': n, 'offset': o, 'size': z, 'type': t} for n, o, z, t in EXTRAS.get(cpp, [])]
        layouts.append({'name': cpp, 'size': size, 'fields': fields})
    for owner, size in DB.execute("SELECT name, size FROM script_structs WHERE kind='Struct'"):
        layouts.append({'name': 'F' + owner.split('.')[-1], 'size': size,
                        'fields': script_fields(owner)})
    json.dump(layouts, open(os.path.join(ROOT, 'symbols', 'layouts.json'), 'w'), indent=0)
    print(f'{len(layouts)} layouts written')


if __name__ == '__main__':
    main()
