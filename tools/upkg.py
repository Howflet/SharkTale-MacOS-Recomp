"""Shark Tale (UE2, package version 129 / licensee 3) package reader.

Reads the name/import/export tables and deserializes the parts of UStruct,
UState, UClass and UProperty exports needed to rebuild class layouts. Then
links properties the way UE2's UStruct::Link does and reports offsets.

Usage:
  python3 -I tools/upkg.py summary <pkg.u>
  python3 -I tools/upkg.py layout  <System dir> <Package> <Class>
  python3 -I tools/upkg.py export-db <System dir> <db_path> <Package> [<Package> ...]

Serialization formats were taken from the decompiled Core.dll serializers
(see docs/decomp/): UObject/UField/UStruct/UProperty::Serialize.
"""
import os
import sqlite3
import struct
import sys

PACKAGE_TAG = 0x9E2A83C1
RF_HAS_STACK = 0x02000000
CPF_NET = 0x20

# Property element sizes once linked (4-byte packing, 32-bit pointers).
FIXED_SIZES = {
    'ByteProperty': 1, 'IntProperty': 4, 'BoolProperty': 4, 'FloatProperty': 4,
    'ObjectProperty': 4, 'ClassProperty': 4, 'NameProperty': 4, 'PointerProperty': 4,
    'StrProperty': 12, 'ArrayProperty': 12, 'DelegateProperty': 8,
    'MapProperty': 12,  # never seen in shipped scripts; placeholder
}


class Reader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def u8(self):
        v = self.data[self.pos]
        self.pos += 1
        return v

    def unpack(self, fmt):
        v = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += struct.calcsize(fmt)
        return v[0] if len(v) == 1 else v

    def i32(self):
        return self.unpack('<i')

    def u32(self):
        return self.unpack('<I')

    def compact(self):
        b0 = self.u8()
        neg = b0 & 0x80
        value = b0 & 0x3F
        if b0 & 0x40:
            shift = 6
            while True:
                b = self.u8()
                value |= (b & 0x7F) << shift
                shift += 7
                if not b & 0x80:
                    break
        return -value if neg else value

    def fstring(self):
        n = self.compact()
        if n == 0:
            return ''
        if n > 0:
            s = self.data[self.pos:self.pos + n].decode('latin1')
            self.pos += n
        else:
            s = self.data[self.pos:self.pos - 2 * n].decode('utf-16-le')
            self.pos += -2 * n
        return s.rstrip('\0')


class Package:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.data = open(path, 'rb').read()
        r = Reader(self.data)
        tag, ver = r.u32(), r.u32()
        if tag != PACKAGE_TAG:
            raise ValueError(f'{path}: not an Unreal package')
        self.version, self.licensee = ver & 0xFFFF, ver >> 16
        (self.flags, name_count, name_off, export_count, export_off,
         import_count, import_off) = r.unpack('<7I')

        r.pos = name_off
        self.names = []
        for _ in range(name_count):
            self.names.append(r.fstring())
            r.u32()  # name flags

        r.pos = import_off
        self.imports = []
        for _ in range(import_count):
            cls_pkg, cls_name = self.names[r.compact()], self.names[r.compact()]
            outer = r.i32()
            obj = self.names[r.compact()]
            self.imports.append({'class_package': cls_pkg, 'class': cls_name,
                                 'outer': outer, 'name': obj})

        r.pos = export_off
        self.exports = []
        for _ in range(export_count):
            cls, sup, outer = r.compact(), r.compact(), r.i32()
            name = self.names[r.compact()]
            flags = r.u32()
            size = r.compact()
            offset = r.compact() if size > 0 else 0
            self.exports.append({'class_ref': cls, 'super_ref': sup, 'outer': outer,
                                 'name': name, 'flags': flags, 'size': size,
                                 'offset': offset})

    # Object references: >0 export (1-based), <0 import, 0 null.
    def ref_name(self, ref):
        if ref > 0:
            return self.exports[ref - 1]['name']
        if ref < 0:
            return self.imports[-ref - 1]['name']
        return None

    def ref_path(self, ref):
        """(package, object name, object class, immediate outer name or None)."""
        if ref > 0:
            e = self.exports[ref - 1]
            return (self.name, e['name'], self.class_of(e), self.outer_name(e))
        if ref < 0:
            imp = self.imports[-ref - 1]
            parent = imp['outer']
            outer = self.imports[-parent - 1] if parent < 0 else None
            top = parent
            while top < 0 and self.imports[-top - 1]['outer'] != 0:
                top = self.imports[-top - 1]['outer']
            pkg = self.imports[-top - 1]['name'] if top < 0 else self.name
            inner_outer = outer['name'] if outer and outer['class'] != 'Package' else None
            return (pkg, imp['name'], imp['class'], inner_outer)
        return None

    def class_of(self, export):
        return self.ref_name(export['class_ref']) or 'Class'

    def outer_name(self, export):
        o = export['outer']
        return self.exports[o - 1]['name'] if o > 0 else None

    def skip_tagged_properties(self, r):
        """UObject tagged properties; class/property exports normally hold only None."""
        while True:
            name = self.names[r.compact()]
            if name == 'None':
                return
            info = r.u8()
            ptype, size_code, is_array = info & 0x0F, (info >> 4) & 7, info & 0x80
            if ptype == 10:  # StructProperty: struct name follows
                r.compact()
            size = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}.get(size_code)
            if size is None:
                size = {5: r.u8, 6: lambda: r.unpack('<H'), 7: r.i32}[size_code]()
            if is_array and ptype != 3:  # BoolProperty stores the value in the flag
                b = r.u8()
                if b & 0x80:
                    r.u8()
                    if b & 0x40:
                        r.pos += 2
            r.pos += size

    def read_object_header(self, r, export, cls):
        if export['flags'] & RF_HAS_STACK:
            node = r.compact()
            r.compact()          # StateNode
            r.pos += 8           # ProbeMask
            r.i32()              # LatentAction
            if node:
                r.compact()      # code offset
        if cls != 'Class':
            self.skip_tagged_properties(r)

    def read_field(self, index):
        """Deserialize the leading part of a field export (struct or property)."""
        e = self.exports[index]
        cls = self.class_of(e)
        r = Reader(self.data, e['offset'])
        self.read_object_header(r, e, cls)
        out = {'name': e['name'], 'class': cls, 'super': r.compact(), 'next': r.compact()}
        if cls in ('Class', 'State', 'Function', 'Struct'):
            out['script_text'] = r.compact()
            out['children'] = r.compact()
            out['friendly_name'] = self.names[r.compact()]
            out['cpp_text'] = r.compact()
            out['line'] = r.i32()
            out['text_pos'] = r.i32()
        elif cls.endswith('Property'):
            out['array_dim'] = r.i32()
            out['flags'] = r.u32()
            out['category'] = self.names[r.compact()]
            if out['flags'] & CPF_NET:
                out['rep_offset'] = r.unpack('<H')
            if self.licensee > 2:
                out['licensee_string'] = r.fstring()
            if cls in ('ObjectProperty', 'ClassProperty'):
                out['property_class'] = r.compact()
                if cls == 'ClassProperty':
                    out['meta_class'] = r.compact()
            elif cls == 'StructProperty':
                out['struct'] = r.compact()
            elif cls == 'ByteProperty':
                out['enum'] = r.compact()
            elif cls in ('ArrayProperty', 'FixedArrayProperty'):
                out['inner'] = r.compact()
                if cls == 'FixedArrayProperty':
                    out['count'] = r.i32()
            elif cls == 'DelegateProperty':
                out['function'] = r.compact()
        return out

    def find_export(self, name, cls=None, outer=None):
        for i, e in enumerate(self.exports):
            if e['name'] == name and (cls is None or self.class_of(e) == cls) \
                    and (outer is None or self.outer_name(e) == outer):
                return i
        return None

    def children(self, index):
        """Own properties (and other fields) of a struct export, in Children order."""
        field = self.read_field(index)
        ref, out = field['children'], []
        while ref > 0:
            child = self.read_field(ref - 1)
            out.append(child)
            ref = child['next']
        return field, out


class Linker:
    """Computes UE2 property layouts across packages (UStruct::Link)."""

    def __init__(self, system_dir, native_sizes=None):
        self.system_dir = system_dir
        self.packages = {}
        self.cache = {}
        # Base classes whose size is fixed by C++ rather than script.
        self.native_sizes = native_sizes or {}

    def package(self, name):
        if name not in self.packages:
            for fn in os.listdir(self.system_dir):
                if fn.lower() == f'{name.lower()}.u':
                    self.packages[name] = Package(os.path.join(self.system_dir, fn))
                    break
            else:
                raise FileNotFoundError(f'{name}.u not in {self.system_dir}')
        return self.packages[name]

    def resolve(self, pkg, ref):
        """Map an object reference to (package, export index)."""
        if ref > 0:
            return pkg, ref - 1
        path = pkg.ref_path(ref)
        if path is None:
            return None
        other = self.package(path[0])
        idx = other.find_export(path[1], path[2], path[3])
        if idx is None:
            raise KeyError(f'{path[0]}.{path[1]} not found')
        return other, idx

    def layout(self, pkg, index):
        key = (pkg.name, index)
        if key in self.cache:
            return self.cache[key]
        field, props = pkg.children(index)
        size, super_name = 0, None
        if field['super']:
            spkg, sidx = self.resolve(pkg, field['super'])
            super_layout = self.layout(spkg, sidx)
            size, super_name = super_layout['size'], f"{spkg.name}.{spkg.exports[sidx]['name']}"
        # Root Object declares ObjectInternal[6] over the vtable and C++-only
        # fields, so its script layout starts at offset 0 like every other struct.
        members, bool_slot = [], None
        for p in props:
            cls = p['class']
            if not cls.endswith('Property'):
                continue
            if cls == 'BoolProperty' and bool_slot and bool_slot[1] < 0x80000000:
                offset, mask = bool_slot[0], bool_slot[1] << 1
                bool_slot = (offset, mask)
                members.append(self.member(pkg, p, offset, 4, mask))
                continue
            elem = self.element_size(pkg, p)
            align = 1 if cls == 'ByteProperty' else 4
            offset = (size + align - 1) & ~(align - 1)
            size = offset + elem * p['array_dim']
            bool_slot = (offset, 1) if cls == 'BoolProperty' else None
            members.append(self.member(pkg, p, offset, elem, 1 if cls == 'BoolProperty' else None))
        size = (size + 3) & ~3  # UStruct::Link rounds PropertiesSize up to 4
        result = {'package': pkg.name, 'name': field['name'], 'kind': field['class'],
                  'super': super_name, 'size': size, 'members': members}
        self.cache[key] = result
        return result

    def element_size(self, pkg, p):
        cls = p['class']
        if cls == 'StructProperty':
            spkg, sidx = self.resolve(pkg, p['struct'])
            return self.layout(spkg, sidx)['size']
        if cls == 'FixedArrayProperty':
            inner = self.resolve(pkg, p['inner'])
            return self.element_size(inner[0], inner[0].read_field(inner[1])) * p['count']
        if cls not in FIXED_SIZES:
            raise KeyError(f'unknown property class {cls}')
        return FIXED_SIZES[cls]

    def member(self, pkg, p, offset, elem, bitmask):
        cls = p['class']
        detail = None
        for key in ('struct', 'property_class', 'enum', 'inner', 'function'):
            if p.get(key):
                detail = pkg.ref_name(p[key])
                if key == 'inner':
                    detail = f"{pkg.read_field(self.resolve(pkg, p[key])[1])['class']}:" \
                        f"{self.inner_detail(pkg, p[key])}" if p[key] > 0 else detail
                break
        return {'name': p['name'], 'type': cls, 'detail': detail, 'offset': offset,
                'size': elem, 'array_dim': p['array_dim'], 'bitmask': bitmask,
                'flags': p['flags'], 'category': p['category']}

    def inner_detail(self, pkg, ref):
        inner = pkg.read_field(ref - 1)
        for key in ('struct', 'property_class', 'enum'):
            if inner.get(key):
                return pkg.ref_name(inner[key])
        return None


def native_sizes_from_db(db_path):
    if not os.path.exists(db_path):
        return {}
    db = sqlite3.connect(db_path)
    return {f'{pkg}.{name}': size for pkg, name, size in
            db.execute("SELECT package, name, size FROM class_sizes WHERE name != '?'")}


def main():
    cmd = sys.argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if cmd == 'summary':
        pkg = Package(sys.argv[2])
        print(f'{pkg.name}: v{pkg.version}/lic{pkg.licensee} names={len(pkg.names)} '
              f'imports={len(pkg.imports)} exports={len(pkg.exports)}')
        kinds = {}
        for e in pkg.exports:
            kinds[pkg.class_of(e)] = kinds.get(pkg.class_of(e), 0) + 1
        for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])[:15]:
            print(f'  {v:6d} {k}')
    elif cmd == 'layout':
        system_dir, pkg_name, cls = sys.argv[2:5]
        linker = Linker(system_dir, native_sizes_from_db(os.path.join(root, 'symbols', 'symbols.db')))
        pkg = linker.package(pkg_name)
        idx = pkg.find_export(cls, 'Class') or pkg.find_export(cls)
        lay = linker.layout(pkg, idx)
        print(f"{lay['package']}.{lay['name']} ({lay['kind']}) : {lay['super']}  size={lay['size']}")
        for m in lay['members']:
            arr = f"[{m['array_dim']}]" if m['array_dim'] > 1 else ''
            bit = f" bit={m['bitmask']:#x}" if m['bitmask'] else ''
            print(f"  {m['offset']:#06x} {m['type']:<18} {m['name']}{arr} {m['detail'] or ''}{bit}")
    elif cmd == 'export-db':
        system_dir, db_path, *names = sys.argv[2:]
        linker = Linker(system_dir, native_sizes_from_db(db_path))
        db = sqlite3.connect(db_path)
        db.executescript('''
            DROP TABLE IF EXISTS script_structs; DROP TABLE IF EXISTS script_members;
            CREATE TABLE script_structs (package TEXT, name TEXT, kind TEXT, super TEXT, size INTEGER);
            CREATE TABLE script_members (package TEXT, owner TEXT, name TEXT, type TEXT, detail TEXT,
                offset INTEGER, size INTEGER, array_dim INTEGER, bitmask INTEGER, flags INTEGER,
                category TEXT);''')
        for name in names:
            pkg = linker.package(name)
            for i, e in enumerate(pkg.exports):
                if pkg.class_of(e) not in ('Class', 'Struct'):
                    continue
                try:
                    lay = linker.layout(pkg, i)
                except Exception as ex:  # report and continue with the rest
                    print(f'  ! {name}.{e["name"]}: {ex}')
                    continue
                owner = e['name'] if pkg.class_of(e) == 'Class' else f"{pkg.outer_name(e)}.{e['name']}"
                db.execute('INSERT INTO script_structs VALUES (?,?,?,?,?)',
                           (name, owner, lay['kind'], lay['super'], lay['size']))
                db.executemany('INSERT INTO script_members VALUES (?,?,?,?,?,?,?,?,?,?,?)',
                               [(name, owner, m['name'], m['type'], m['detail'], m['offset'],
                                 m['size'], m['array_dim'], m['bitmask'], m['flags'],
                                 m['category']) for m in lay['members']])
            print(f'{name}: done')
        db.commit()


if __name__ == '__main__':
    main()
