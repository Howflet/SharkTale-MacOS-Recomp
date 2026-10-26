"""Build symbols/symbols.db (SQLite) from the original PE modules.

Usage: python3 -I tools/build_symdb.py <db_path> <pe> [<pe> ...]
Demangles MSVC names with llvm-undname (Homebrew LLVM).
"""
import os
import re
import sqlite3
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_symbols import PE  # noqa: E402

UNDNAME = '/opt/homebrew/opt/llvm/bin/llvm-undname'
CALLCONV = re.compile(r'__(?:thiscall|cdecl|stdcall|fastcall) ')

SCHEMA = '''
DROP VIEW IF EXISTS cross_module_use;
DROP TABLE IF EXISTS modules;
DROP TABLE IF EXISTS exports;
DROP TABLE IF EXISTS imports;
DROP TABLE IF EXISTS native_classes;
CREATE TABLE modules (name TEXT PRIMARY KEY, file TEXT, image_base INTEGER,
                      text_size INTEGER);
CREATE TABLE exports (module TEXT, ordinal INTEGER, rva INTEGER, va INTEGER,
                      mangled TEXT, demangled TEXT, qualname TEXT, class TEXT,
                      member TEXT, kind TEXT, is_virtual INTEGER,
                      is_static INTEGER);
CREATE TABLE imports (module TEXT, dll TEXT, hint TEXT, name TEXT);
CREATE TABLE native_classes (module TEXT, class TEXT);
CREATE INDEX exports_mangled ON exports (mangled);
CREATE INDEX exports_class ON exports (class);
CREATE INDEX imports_name ON imports (name);
CREATE VIEW cross_module_use AS
  SELECT i.module AS consumer, e.module AS provider, e.mangled, e.demangled,
         e.class, e.member, e.kind
  FROM imports i JOIN exports e
    ON i.name = e.mangled AND lower(i.dll) = lower(e.module) || '.dll';
'''


def demangle(names):
    """Map mangled -> demangled for MSVC '?' names; others pass through."""
    todo = [n for n in names if n.startswith('?')]
    out = {}
    if todo:
        res = subprocess.run([UNDNAME], input='\n'.join(todo) + '\n',
                             capture_output=True, text=True, check=True)
        for block in res.stdout.strip().split('\n\n'):
            lines = block.split('\n')
            if len(lines) >= 2 and not lines[1].startswith('error:'):
                out[lines[0]] = lines[1]
    return out


def qualified_name(demangled):
    """Pull 'Class::member' out of a demangled signature."""
    m = CALLCONV.search(demangled)
    if m:  # function: name runs from the calling convention to its '('
        rest = demangled[m.end():]
        depth, i = 0, 0
        while i < len(rest):
            if rest.startswith('operator', i) and (i == 0 or not rest[i - 1].isalnum()):
                # Skip the operator token so '<', '<<', '()' etc. aren't parsed as syntax.
                j = i + len('operator')
                if rest.startswith('()', j):
                    j += 2
                elif rest.startswith(' new', j) or rest.startswith(' delete', j):
                    j = rest.index(' ', j + 1) if rest.startswith('[]', rest.find('[', j)) else j + len(rest[j:].split('(')[0])
                elif not rest[j:j + 1].isspace():
                    while j < len(rest) and rest[j] in '<>=!+-*/%&|^~[],':
                        j += 1
                else:  # conversion operator: type up to '('
                    j = rest.index('(', j)
                i = j
                if depth == 0 and rest[i:i + 1] == '(':
                    return rest[:i], 'func'
                continue
            ch = rest[i]
            if ch == '<':
                depth += 1
            elif ch == '>':
                depth -= 1
            elif ch == '(' and depth == 0:
                return rest[:i], 'func'
            i += 1
        return rest, 'func'
    return demangled.split(' ')[-1].lstrip('*&'), 'data'  # data: name is the last token


def split_class(qualname):
    op = qualname.find('operator')
    if op >= 0:  # operator symbols contain '<', '>' and '::' never follows them
        head = qualname[:op]
        return (head[:-2], qualname[op:]) if head.endswith('::') else ('', qualname)
    depth = 0
    for i in range(len(qualname) - 1, 0, -1):
        ch = qualname[i]
        if ch == '>':
            depth += 1
        elif ch == '<':
            depth -= 1
        elif depth == 0 and qualname[i - 1:i + 1] == '::':
            return qualname[:i - 1], qualname[i + 1:]
    return '', qualname


def main():
    db_path, paths = sys.argv[1], sys.argv[2:]
    db = sqlite3.connect(db_path)  # other tools' tables (classes, script_*) are kept
    db.executescript(SCHEMA)
    for p in paths:
        pe = PE(p)
        mod = os.path.splitext(os.path.basename(p))[0]
        text = next((s for s in pe.sections if s[0] == 0x1000), (0, 0, 0))[1]
        db.execute('INSERT INTO modules VALUES (?,?,?,?)',
                   (mod, os.path.basename(p), pe.image_base, text))
        exports = pe.exports()
        names = demangle([n for _, _, n in exports])
        for ordinal, rva, name in exports:
            dem = names.get(name, name)
            if name in names:
                qual, kind = qualified_name(dem)
            else:
                qual, kind = name, ('data' if not name.startswith('_') else 'func')
            cls, member = split_class(qual)
            # Storage code after '@@' is authoritative: '3' = global variable.
            g = re.match(r'^\?([A-Za-z_]\w*)@((?:[A-Za-z_]\w*@)*)@([0-3])', name)
            if g and g.group(3) == '3':
                cls, member, qual, kind = '', g.group(1), g.group(1), 'data'
            elif g and g.group(2):  # '0'-'2': static data member; scopes are innermost-first
                scopes = g.group(2).rstrip('@').split('@')
                cls = '::'.join(reversed(scopes))
                member, qual, kind = g.group(1), f'{cls}::{g.group(1)}', 'data'
            else:
                # Plain member function ?name@Scope...@@<access>: take names from the mangling,
                # which stays unambiguous when the return type is a function pointer.
                f = re.match(r'^\?([A-Za-z_]\w*)@((?:[A-Za-z_]\w*@)+)@[A-Z]', name)
                if f:
                    cls = '::'.join(reversed(f.group(2).rstrip('@').split('@')))
                    member, qual = f.group(1), f'{cls}::{f.group(1)}'
            db.execute('INSERT INTO exports VALUES (?,?,?,?,?,?,?,?,?,?,?,?)',
                       (mod, ordinal, rva, pe.image_base + rva, name, dem, qual,
                        cls, member, kind, int('virtual ' in dem),
                        int(' static ' in f' {dem}')))
            if name.startswith('autoclass'):
                db.execute('INSERT INTO native_classes VALUES (?,?)',
                           (mod, name[len('autoclass'):]))
        db.executemany('INSERT INTO imports VALUES (?,?,?,?)',
                       [(mod, d, h, n) for d, h, n in pe.imports()])
        print(f'{mod}: {len(exports)} exports, {len(names)} demangled')
    db.commit()


if __name__ == '__main__':
    main()
