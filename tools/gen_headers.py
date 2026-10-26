"""Generate C++ declarations for a module from symbols.db.

  python3 -I tools/gen_headers.py <Module> [<Module> ...]

Writes src/<Module>/Inc/<Module>Classes.h containing, for that module:
  * forward declarations for every type its export signatures mention,
  * script structs (F*) with members at their linked offsets,
  * native classes with script members (size-checked against the binary),
  * every exported member function / static with its exact signature, virtual
    functions in vtable-slot order so the MSVC vftable layout matches,
  * exported free functions and globals.

Data sources: exports + classes + class_sizes + script_structs/members tables
(build_symdb.py, ExportClassSizes.java, upkg.py) and the vtables in the PE.
Verify with tools/check_headers.py.
"""
import json
import os
import re
import sqlite3
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_symbols import PE  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB = sqlite3.connect(os.path.join(ROOT, 'symbols', 'symbols.db'))

# Module -> modules whose generated headers it includes (from cross_module_use).
DEPENDS = defaultdict(list)
for consumer, provider in DB.execute(
        'SELECT DISTINCT consumer, provider FROM cross_module_use'):
    if provider != consumer:
        DEPENDS[consumer].append(provider)

# Types defined by hand in src/Core/Inc/CorePrelude.h.
PRELUDE_TYPES = {'TArray', 'TLazyArray', 'TMap', 'TMultiMap', 'TTransArray', 'FLazyLoader', 'FArray'}

# Exported Core types whose data layout is written by hand (key, bases, members, size).
HAND_LAYOUTS = {
    'FName': ('class', '', ['INT Index;'], 4),
    'FString': ('class', ' : public TArray<TCHAR>', [], 12),
    'FScriptDelegate': ('struct', '', ['class UObject* Object;', 'FName FunctionName;'], 8),
}

ACCESS = re.compile(r'^(public|protected|private): ')
TYPE_REF = re.compile(r'\b(class|struct|union|enum) ([A-Za-z_][\w:]*(?:<[^()]*?>)?)')


def module_file(module):
    return DB.execute('SELECT file FROM modules WHERE name=?', (module,)).fetchone()[0]


# --------------------------------------------------------------------------
# Signature handling.
# --------------------------------------------------------------------------

def split_signature(dem, qual):
    """Return (access, prefix, name, rest) for a demangled member signature.
    prefix holds 'virtual'/'static', return type and calling convention."""
    access = 'public'
    m = ACCESS.match(dem)
    if m:
        access, dem = m.group(1), dem[m.end():]
    idx = dem.find(qual)
    return access, dem[:idx], dem[idx + len(qual):]


def member_decl(dem, qual, member):
    access, prefix, rest = split_signature(dem, qual)
    if member.startswith('operator ') and member[9:10] not in ('=', '<', '>', '!', '[', '(', '+', '-', '*', '/', '&', '|', '~', '^', '%'):
        if not member.startswith(('operator new', 'operator delete')):
            # Conversion operator: drop the return type, keep virtual/static and the CC.
            keep = [w for w in prefix.split() if w in ('virtual', 'static') or w.startswith('__')]
            prefix = ' '.join(keep) + ' ' if keep else ''
    return access, f'{prefix}{member}{rest};'


def type_refs(text):
    """(key, name) pairs for class/struct/enum names used in a signature."""
    out = set()
    for key, name in TYPE_REF.findall(text):
        out.add((key, name))
        # Template arguments may name more types.
        if '<' in name:
            out |= type_refs(name[name.index('<') + 1:-1])
    return out


# --------------------------------------------------------------------------
# Vtables.
# --------------------------------------------------------------------------

class Vtables:
    def __init__(self):
        self.pe = {}
        self.bounds = {}
        self.purecall = {}
        self.cache = {}
        self.all_classes = None
        self.unnamed_cache = {}
        self.names = dict(DB.execute('SELECT va, demangled FROM exports'))
        self.quals = dict(DB.execute('SELECT va, qualname FROM exports'))
        # Identical-code folding gives several functions one address: keep all.
        # Keyed by (module, va): several DLLs share the default 0x10000000 image base.
        self.cands = defaultdict(list)
        for mod, va, qual, dem in DB.execute(
                "SELECT module, va, qualname, demangled FROM exports WHERE kind='func'"):
            self.cands[(mod, va)].append((qual, dem))
        self.native_bases = dict(DB.execute('SELECT cpp, base FROM classes'))

    def slots(self, module, cls):
        """Binary vftable slots (target VA, qualname, demangled). A vftable ends at the
        next exported vftable of the module (no RTTI, so they are contiguous);
        trailing zero entries are padding."""
        if not module:
            return None
        key = (module, cls)
        if key in self.cache:
            return self.cache[key]
        self.cache[key] = result = self._slots(module, cls)
        return result

    def _slots(self, module, cls, mangled=None):
        if mangled:
            row = DB.execute('SELECT va FROM exports WHERE module=? AND mangled=?',
                             (module, mangled)).fetchone()
        else:
            row = DB.execute('SELECT va FROM exports WHERE module=? AND mangled=?',
                             (module, f'??_7{cls}@@6B@')).fetchone()
            if not row:
                # Multiple inheritance: the primary vftable is tagged with the U/A base
                # that introduced it (UObject, UChannel, ULevelBase, ...); F* tags are interfaces.
                for va, mangled in DB.execute('SELECT va, mangled FROM exports WHERE module=? AND mangled LIKE ?',
                                              (module, f'??_7{cls}@@6B%@@@')):
                    if mangled[len(f'??_7{cls}@@6B'):][:1] in ('U', 'A'):
                        row = (va,)
                        break
        if not row:
            return None
        if module not in self.pe:
            self.pe[module] = PE(os.path.join(ROOT, 'orig', 'System', module_file(module)))
            self.bounds[module] = sorted(va for (va,) in DB.execute(
                "SELECT va FROM exports WHERE module=? AND (mangled LIKE '??_7%' OR kind='data')",
                (module,)))
        pe = self.pe[module]
        end = next((v for v in self.bounds[module] if v > row[0]), row[0] + 4 * 512)
        text = next(sec for sec in pe.sections if sec[0] == 0x1000)
        lo, hi = pe.image_base + text[0], pe.image_base + text[0] + text[1]
        out = []
        for va in range(row[0], end, 4):
            target = struct.unpack_from('<I', pe.data, pe.off(va - pe.image_base))[0]
            if not lo <= target < hi:  # zero padding or data: the vftable has ended
                break
            qual, dem = self.pick(module, target, cls)
            out.append((target, qual, dem))
        return out

    def pick(self, module, target, cls):
        """Name for a slot target, preferring cls or an ancestor when code was folded."""
        cands = self.cands.get((module, target))
        if not cands:
            return None, ''
        chain, c, seen = [], cls, set()
        while c and c not in seen:
            seen.add(c)
            chain.append(c)
            c = self.native_bases.get(c)
        for owner in chain:
            for qual, dem in cands:
                if qual and qual.rsplit('::', 1)[0] == owner:
                    return qual, dem
        if len(cands) == 1:
            return cands[0]
        return None, ''  # ambiguous fold of unrelated classes: treat as unnamed

    def purecall_targets(self, module):
        """Unnamed slot targets shared by many vtables: the _purecall stub."""
        if module in self.purecall:
            return self.purecall[module]
        counts = defaultdict(int)
        for (m,) in DB.execute("SELECT mangled FROM exports WHERE module=? AND mangled LIKE '??_7%@@6B@'", (module,)):
            for target, qual, _ in self.slots(module, m[4:-5]) or []:
                if not qual:
                    counts[target] += 1
        self.purecall[module] = {t for t, n in counts.items() if n >= 3}
        return self.purecall[module]

    def name_for_unnamed(self, module, cls, index):
        """Signature for an unexported slot, taken from a class that overrides it."""
        if (cls, index) in self.unnamed_cache:
            return self.unnamed_cache[(cls, index)]
        if self.all_classes is None:
            self.all_classes = [(mod, m[4:m.index('@@6B')]) for mod, m in DB.execute(
                "SELECT module, mangled FROM exports WHERE mangled LIKE '??_7%@@6B@' "
                "OR mangled LIKE '??_7%@@6BU%@@@' OR mangled LIKE '??_7%@@6BA%@@@'")
                if '@' not in m[4:m.index('@@6B')]]
        mine = self.slots(module, cls) or []
        result = None
        # Best source: a derived class's vftable for our subobject (??_7X@@6B<cls>@@@)
        # holds overriders of exactly our slots.
        for mod, mangled in DB.execute("SELECT module, mangled FROM exports WHERE mangled LIKE ?",
                                       (f'??_7%@@6B{cls}@@@',)):
            sub = self._slots(mod, None, mangled) or []
            if index < len(sub) and sub[index][1]:
                self.unnamed_cache[(cls, index)] = sub[index]
                return sub[index]
        native_bases = dict(DB.execute('SELECT cpp, base FROM classes'))

        def descends(other):
            seen = set()
            while other in native_bases and other not in seen:
                seen.add(other)
                other = native_bases[other]
                if other == cls:
                    return True
            return False
        candidates = self.all_classes
        if cls in native_bases:
            # Natives: only true descendants may name our slots.
            candidates = [(mod, o) for mod, o in self.all_classes if descends(o)]
        for mod, other in candidates:
            if other == cls:
                continue
            theirs = self.slots(mod, other) or []
            if len(theirs) <= index or not theirs[index][1]:
                continue
            # Proven descendants (native hierarchy) qualify outright; otherwise our
            # named slots must agree on member names, and we need at least one.
            if cls not in native_bases and not any(q for _, q, _ in mine):
                break
            if cls in native_bases or all(not q or (j < len(theirs) and theirs[j][1] and
                             theirs[j][1].split('::')[-1] == q.split('::')[-1])
                   for j, (_, q, _) in enumerate(mine)):
                result = theirs[index]
                break
        self.unnamed_cache[(cls, index)] = result
        return result

    @staticmethod
    def pe_contains(pe, va):
        return pe.image_base <= va < pe.image_base + 0x01000000


# --------------------------------------------------------------------------
# Script layouts -> C++ members.
# --------------------------------------------------------------------------

class Scripts:
    def __init__(self):
        self.class_cpp = {}
        for cpp, script, base in DB.execute('SELECT cpp, script, base FROM classes'):
            self.class_cpp[script] = cpp
        self.supers = {}
        for name, kind, sup in DB.execute(
                "SELECT name, kind, super FROM script_structs WHERE kind='Class'"):
            self.supers[name] = sup.split('.')[-1] if sup else None
        self.struct_keys = {}
        for (dem,) in DB.execute("SELECT demangled FROM exports WHERE demangled LIKE '% F%'"):
            for key, name in TYPE_REF.findall(dem):
                if name.startswith('F'):
                    self.struct_keys.setdefault(name, key)

    def is_actor(self, name):
        seen = set()
        while name and name not in seen:
            if name == 'Actor':
                return True
            seen.add(name)
            name = self.supers.get(name)
        return False

    def cpp_class(self, script_name):
        if script_name in self.class_cpp:
            return self.class_cpp[script_name]
        return ('A' if self.is_actor(script_name) else 'U') + script_name

    def struct_key(self, cpp):
        return self.struct_keys.get(cpp, 'struct')

    def member_type(self, m):
        t, d = m['type'], m['detail']
        if t == 'ByteProperty':
            return 'BYTE'
        if t == 'IntProperty':
            return 'INT'
        if t == 'FloatProperty':
            return 'FLOAT'
        if t == 'BoolProperty':
            return 'BITFIELD'
        if t == 'NameProperty':
            return 'FName'
        if t == 'StrProperty':
            return 'FString'
        if t == 'PointerProperty':
            return 'void*'
        if t == 'DelegateProperty':
            return 'FScriptDelegate'
        if t == 'ClassProperty':
            return 'class UClass*'
        if t == 'ObjectProperty':
            return f'class {self.cpp_class(d)}*' if d else 'class UObject*'
        if t == 'StructProperty':
            cpp = 'F' + d
            return f'{self.struct_key(cpp)} {cpp}'
        if t == 'ArrayProperty':
            inner_type, inner = (d or ':').split(':', 1)
            inner_cpp = self.member_type({'type': inner_type, 'detail': inner or None})
            return f'TArray<{inner_cpp}>'
        raise KeyError(t)


def emit_members(scripts, members, start, limit, lines, indent='\t'):
    """Members between offset `start` and `limit` with explicit padding."""
    pos = start
    for m in members:
        end = m['offset'] + m['size'] * m['array_dim']
        if m['offset'] < start or end > limit:
            continue
        if m['bitmask'] and m['bitmask'] != 1:
            lines.append(f"{indent}BITFIELD {m['name']}:1;")
            continue
        if m['offset'] > pos:
            lines.append(f'{indent}BYTE _Pad{pos:X}[{m["offset"] - pos}];')
        ctype = scripts.member_type(m)
        if m['type'] == 'BoolProperty':
            lines.append(f"{indent}BITFIELD {m['name']}:1;  // {m['offset']:#x}")
        else:
            arr = f"[{m['array_dim']}]" if m['array_dim'] > 1 else ''
            lines.append(f"{indent}{ctype} {m['name']}{arr};  // {m['offset']:#x}")
        pos = end
    if limit > pos:
        lines.append(f'{indent}BYTE _Pad{pos:X}[{limit - pos}];')


# --------------------------------------------------------------------------
# Generation.
# --------------------------------------------------------------------------

def generate(module):
    vt = Vtables()
    scripts = Scripts()
    api = f'{module.upper()}_API'
    exports = DB.execute(
        "SELECT mangled, demangled, qualname, class, member, kind, is_virtual "
        "FROM exports WHERE module=? AND mangled LIKE '?%' AND demangled NOT LIKE '%`%'",
        (module,)).fetchall()

    by_class = defaultdict(list)
    free = []
    for mangled, dem, qual, cls, member, kind, virt in exports:
        if cls and not cls.startswith(('std::', '`')):
            by_class[cls].append((mangled, dem, qual, member, kind, virt))
        elif not cls:
            free.append((mangled, dem, qual, kind))

    natives = {cpp: (size, (None if base == cpp else base), script)
               for cpp, size, base, script in DB.execute(
                   'SELECT cpp, size, base, script FROM classes WHERE module=?', (module,))}
    if 'UObject' in natives:
        natives['UObject'] = (natives['UObject'][0], 'FUnknown', natives['UObject'][2])
    script_members = defaultdict(list)
    for owner, name, typ, detail, off, size, dim, bit in DB.execute(
            'SELECT owner, name, type, detail, offset, size, array_dim, bitmask '
            'FROM script_members ORDER BY rowid'):
        script_members[owner].append({'name': name, 'type': typ, 'detail': detail,
                                      'offset': off, 'size': size, 'array_dim': dim,
                                      'bitmask': bit})
    script_sizes = {(pkg, name): (size, sup) for pkg, name, size, sup in DB.execute(
        'SELECT package, name, size, super FROM script_structs')}

    # Every type mentioned anywhere: forward declarations.
    fwd = set()
    for rows in by_class.values():
        for _, dem, *_ in rows:
            fwd |= type_refs(dem)
    for _, dem, _, _ in free:
        fwd |= type_refs(dem)

    out = [f'/* {module}Classes.h: GENERATED by tools/gen_headers.py from symbols.db. Do not edit. */',
           '#pragma once', '#include "CorePrelude.h"']
    out += [f'#include "{d}Classes.h"' for d in sorted(DEPENDS[module])
            if os.path.exists(os.path.join(ROOT, 'src', d, 'Inc', f'{d}Classes.h')) or d == 'Core']
    out += [f'#ifndef {api}', f'#define {api} DLL_IMPORT', '#endif', '#pragma pack(push, 4)', '']
    out += ['#ifndef SIZE_CHECK', '#ifdef GEN_NO_SIZE_ASSERTS', '#define SIZE_CHECK(T, S)',
            '#else', '#define SIZE_CHECK(T, S) static_assert(sizeof(T) == (S), "sizeof " #T)',
            '#endif', '#endif', '']

    templates = {}
    plain = []
    nested = defaultdict(set)
    for key, name in sorted(fwd):
        if '<' in name:
            base = name[:name.index('<')]
            if base not in PRELUDE_TYPES:
                arity = len(split_template_args(name[name.index('<') + 1:-1]))
                templates.setdefault(base, (key, arity))
        elif '::' in name:
            outer, inner = name.rsplit('::', 1)
            nested[outer].add((key, inner))
        elif name not in PRELUDE_TYPES:
            plain.append((key, name))
    for key, name in plain:
        out.append(f'enum {name} : int;' if key == 'enum' else f'{key} {name};')
    NESTED.clear()
    NESTED.update(nested)
    BY_CLASS.clear()
    BY_CLASS.update(by_class)
    by_class = BY_CLASS
    for base, (key, arity) in sorted(templates.items()):
        params = ', '.join(f'class T{i}' for i in range(arity))
        out.append(f'template<{params}> {key} {base};')
    out.append('')

    # Hand-laid-out core types come first (script structs embed them).
    for cpp, (key, bases, fields, size) in HAND_LAYOUTS.items():
        if cpp not in by_class and module != 'Core':
            continue
        lines = [f'{key} {api} {cpp}{bases}', '{', 'public:'] + [f'\t{f}' for f in fields]
        lines += method_lines(by_class.pop(cpp, []), cpp, None)
        lines += ['};', f'SIZE_CHECK({cpp}, {size});', '']
        out += lines

    # Script structs owned by this module's package(s).
    packages = {p for (p,) in DB.execute('SELECT DISTINCT package FROM class_sizes WHERE module=?', (module,))}
    emitted_structs = set()
    # A script struct goes to the module that exports its C++ methods (e.g. FColor
    # is declared in Core script but implemented in Engine); otherwise to its package.
    struct_owner = {cls: mod for cls, mod in DB.execute(
        "SELECT class, MIN(module) FROM exports WHERE class LIKE 'F%' GROUP BY class")}
    struct_rows = []
    for (pkg, name), (size, _) in script_sizes.items():
        if '.' not in name:
            continue
        cpp = 'F' + name.split('.')[-1]
        owner = struct_owner.get(cpp)
        if (owner == module) or (owner is None and pkg in packages):
            struct_rows.append((pkg, name, size))
    for pkg, owner_name, size in order_structs(struct_rows, script_members):
        cpp = 'F' + owner_name.split('.')[-1]
        if cpp in emitted_structs or cpp in PRELUDE_TYPES:
            continue
        emitted_structs.add(cpp)
        key = scripts.struct_key(cpp)
        lines = [f'{key} {api} {cpp}', '{', 'public:']
        emit_members(scripts, script_members[owner_name], 0, size, lines)
        lines += method_lines(by_class.pop(cpp, []), cpp, None)
        lines += ['};', f'SIZE_CHECK({cpp}, {size});', '']
        out += lines

    # Bases for plain C++ classes: the most-derived class owning inherited slots.
    vt_cache = {}

    def slots_of(c):
        if c not in vt_cache:
            vt_cache[c] = vt.slots(module_of(c), c) or [] if module_of(c) else []
        return vt_cache[c]

    def infer_base(c):
        owners = []
        for _, qual, _ in slots_of(c):
            owner = qual.rsplit('::', 1)[0] if qual and '::' in qual else None
            if owner and owner != c and owner not in owners:
                owners.append(owner)
        # Prefer the longest vtable that fits in ours; on a tie, the more-derived
        # candidate (whose own slots mention more of the other candidates).
        def rank(o):
            o_owners = {q.rsplit('::', 1)[0] for _, q, _ in slots_of(o) if q and '::' in q}
            return (len(slots_of(o)), len(o_owners & set(owners)))
        fits = [o for o in owners if len(slots_of(o)) <= len(slots_of(c))]
        return max(fits, key=rank) if fits else None

    # Remaining exported C++ classes without script layout (methods only).
    plain_classes = [c for c in by_class
                     if not ('<' in c or '::' in c or c in PRELUDE_TYPES or c in natives)]
    plain_bases = {c: infer_base(c) for c in plain_classes}
    ordered, seen = [], set()

    def visit_plain(c):
        if c in seen:
            return
        seen.add(c)
        if plain_bases.get(c) in plain_bases:
            visit_plain(plain_bases[c])
        ordered.append(c)
    for c in sorted(plain_classes):
        visit_plain(c)
    # One topological order over plain and native classes (either may derive from the other).
    all_bases = dict(plain_bases)
    all_bases.update({c: natives[c][1] for c in natives})
    emit_order, emitted = [], set()

    def visit_any(c):
        if c in emitted or c not in all_bases:
            return
        emitted.add(c)
        visit_any(all_bases[c])
        emit_order.append(c)
    for c in sorted(all_bases):
        visit_any(c)

    for cpp in emit_order:
        if cpp in natives:
            size, base, script = natives[cpp]
            rows = by_class.pop(cpp, [])
            extra = [t for t in secondary_tags(cpp) if t not in ancestors_tags(base, all_bases)]
            base_decl = ' : ' + ', '.join(f'public {b}' for b in [base] + extra) if base else ''
            lines = [f'class {api} {cpp}{base_decl}', '{', 'public:']
            base_size = class_size(base) if base else 0
            if cpp == 'UObject':
                # ObjectInternal[6] in script overlays the vptr and these fields.
                lines += ['\tINT Index;  // 0x4', '\tclass UObject* HashNext;  // 0x8',
                          '\tstruct FStateFrame* StateFrame;  // 0xc', '\tclass ULinkerLoad* _Linker;  // 0x10',
                          '\tINT _LinkerIndex;  // 0x14']
                base_size = 0x18
            members = script_members.get(script, [])
            if base_size is not None and extra:
                # Each extra interface adds a vptr right after the primary base; script
                # declares a placeholder int for it (e.g. ExecVtbl), which we skip.
                base_size += 4 * len(extra)
            if members and base_size is not None:
                emit_members(scripts, members, base_size, size, lines)
            elif base_size is not None and size > base_size:
                lines.append(f'\tBYTE _Unknown[{size - base_size}];')
            lines += method_lines(rows, cpp, (vt, module, base))
            lines += ['};', f'SIZE_CHECK({cpp}, {size});', '']
        else:
            key = next((k for k, n in fwd if n == cpp), 'class')
            base = plain_bases[cpp]
            base_decl = f' : public {base}' if base else ''
            lines = [f'{key} {api} {cpp}{base_decl}', '{', 'public:', '\t// layout not recovered']
            lines += method_lines(by_class.pop(cpp), cpp, (vt, module, base))
            lines += ['};', '']
        out += lines

    for outer, inners in sorted(NESTED.items()):
        if '::' in outer or '<' in outer:
            continue
        key = next((k for k, n in fwd if n == outer), 'class')
        out += [f'{key} {outer}', '{', 'public:'] + [
            f'\t{k} {i};' if k != 'enum' else f'\tenum {i} : int;' for k, i in sorted(inners)] + ['};', '']
    NESTED.clear()
    out.append('// Free functions and globals.')
    for mangled, dem, qual, kind in free:
        if '::' in qual or ('<' in qual and not qual.startswith('operator')):
            continue
        decl = re.sub(r'^(public|protected|private): ', '', dem)
        if kind == 'data':
            out.append(f'extern {api} {decl};')
        else:
            out.append(f'{api} {decl};')
    out += ['', '#pragma pack(pop)', '']

    path = os.path.join(ROOT, 'src', module, 'Inc', f'{module}Classes.h')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write('\n'.join(out))
    print(f'{module}: wrote {path}')


def split_template_args(text):
    args, depth, cur = [], 0, ''
    for ch in text:
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur)
            cur = ''
        else:
            cur += ch
    return args + [cur]


def class_size(cpp):
    row = DB.execute('SELECT size FROM classes WHERE cpp=?', (cpp,)).fetchone()
    return row[0] if row else None


def topo_classes(natives):
    order, seen = [], set()

    def visit(c):
        if c in seen or c not in natives:
            return
        seen.add(c)
        visit(natives[c][1])
        order.append(c)
    for c in sorted(natives):
        visit(c)
    return order


def order_structs(rows, members):
    """Script structs ordered so embedded structs come first."""
    by_short = {name.split('.')[-1]: (pkg, name, size) for pkg, name, size in rows}
    order, seen = [], set()

    def visit(short):
        if short in seen or short not in by_short:
            return
        seen.add(short)
        pkg, name, size = by_short[short]
        for m in members.get(name, []):
            if m['type'] == 'StructProperty':
                visit(m['detail'])
            if m['type'] == 'ArrayProperty' and m['detail'] and m['detail'].startswith('StructProperty:'):
                visit(m['detail'].split(':', 1)[1])
        order.append(by_short[short])
    for short in sorted(by_short):
        visit(short)
    return order


NESTED = defaultdict(set)
BY_CLASS = {}
SLOT_OVERRIDES = {k: v for k, v in json.load(open(os.path.join(ROOT, 'tools', 'slot_overrides.json'))).items()
                  if not k.startswith('_')}


def method_lines(rows, cpp, vtinfo):
    """Declarations: virtuals in slot order (MSVC overload grouping), then the rest."""
    lines, done = [], set()
    for key, inner in sorted(NESTED.pop(cpp, ())):
        nested_rows = BY_CLASS.pop(f'{cpp}::{inner}', None)
        if key == 'enum':
            lines.append(f'public:\n\tenum {inner} : int;')
        elif nested_rows:
            body = method_lines(nested_rows, f'{cpp}::{inner}', None)
            # Members are declared with their unqualified name inside the nested class.
            lines.append(f'public:\n\t{key} {inner}\n\t{{\n\t' + '\n\t'.join(body) + '\n\t};')
        else:
            lines.append(f'public:\n\t{key} {inner};')
    if vtinfo:
        vt, module, base = vtinfo
        slots = vt.slots(module, cpp) or []
        base_count = len(vt.slots(module_of(base), base) or []) if base else 0
        new_slots = slots[base_count:]
        # Declare new virtuals in slot order; MSVC places overloads of one name
        # together in reverse declaration order, so reverse each same-name run.
        groups = []
        for target, qual, dem in new_slots:
            name = qual.split('::')[-1] if qual else None
            if groups and name and groups[-1][0] == name:
                groups[-1][1].append((target, qual, dem))
            else:
                groups.append((name, [(target, qual, dem)]))
        index = len(slots) - len(new_slots)
        dtor = next((r for r in rows if r[3].startswith('~') and r[5]), None)
        if dtor and base and DB.execute(
                "SELECT 1 FROM exports WHERE mangled LIKE ?", (f'??1{base}@@U%',)).fetchone():
            dtor = None  # inherited virtual dtor: an override, not a new slot
        for name, members in groups:
            for target, qual, dem in reversed(members):
                if qual and qual.startswith(cpp + '::') and dem:
                    access, decl = member_decl(dem, qual, qual.split('::')[-1])
                    lines.append(f'{access}:\n\t{decl}')
                    done.add(dem)
                elif dem and 'scalar deleting destructor' not in dem and qual:
                    if not base:
                        lines.append(f'public:\n\tvirtual void _InheritedSlot{index}();  // {qual}')
                elif dtor and dtor[1] not in done:
                    access, decl = member_decl(dtor[1], dtor[2], dtor[3])
                    lines.append(f'{access}:\n\t{decl}  // slot {index}: deleting dtor')
                    done.add(dtor[1])
                elif cpp in SLOT_OVERRIDES and index < len(SLOT_OVERRIDES[cpp]):
                    lines.append(f'public:\n\t{SLOT_OVERRIDES[cpp][index]}  // slot {index}: tools/slot_overrides.json')
                else:
                    found = vt.name_for_unnamed(module, cpp, index)
                    pure = ' = 0' if target in vt.purecall_targets(module) else ''
                    own = {r[1].replace(f'{cpp}::', '#::'): r for r in rows}
                    sig = found[2].replace(f"{found[1].rsplit('::', 1)[0]}::", '#::') \
                        if found and found[1] and found[2] else None
                    if sig in own and own[sig][1] not in done:
                        # Pure virtual with a body: the slot is _purecall, yet the class
                        # exports the function (called non-virtually by subclasses).
                        r = own[sig]
                        done.add(r[1])
                        access, decl = member_decl(r[1], r[2], r[3])
                        lines.append(f'{access}:\n\t{decl.rstrip(";")}{pure};  // slot {index}: pure with body')
                        index += 1
                        continue
                    if sig in own:
                        found = None
                    if found and found[2] and found[2] in done:
                        lines.append(f'public:\n\tvirtual void _Slot{index}(){pure};  // dup of {found[1]}')
                    elif found and found[2]:
                        done.add(found[2])
                        access, decl = member_decl(found[2], found[1], found[1].split('::')[-1])
                        decl = decl.rstrip(';') + pure + ';'
                        lines.append(f'{access}:\n\t{decl}  // slot {index}: named from {found[1]}')
                    else:
                        lines.append(f'public:\n\tvirtual void _Slot{index}(){pure};  // not exported')
                index += 1
    for mangled, dem, qual, member, kind, virt in rows:
        if dem in done:
            continue
        access, decl = member_decl(dem, qual, member)
        lines.append(f'{access}:\n\t{decl}')
    return lines


def module_of(cls):
    row = DB.execute("SELECT module FROM exports WHERE mangled=? OR mangled LIKE ?",
                     (f'??_7{cls}@@6B@', f'??_7{cls}@@6B%@@@')).fetchone()
    return row[0] if row else None


def secondary_tags(cls):
    """Interfaces (F* tags) with their own vptr in cls (from ??_7cls@@6B<Tag>@@@ exports)."""
    return [m[len(f'??_7{cls}@@6B'):-3] for (m,) in DB.execute(
        "SELECT mangled FROM exports WHERE mangled LIKE ?", (f'??_7{cls}@@6B%@@@',))
        if m[len(f'??_7{cls}@@6B'):][:1] == 'F']


def ancestors_tags(cls, bases):
    seen = set()
    while cls:
        seen |= set(secondary_tags(cls))
        cls = bases.get(cls)
    return seen


Vtables.slots_for_any = lambda self, cls: self.slots(module_of(cls), cls) or []

if __name__ == '__main__':
    for mod in sys.argv[1:]:
        generate(mod)
