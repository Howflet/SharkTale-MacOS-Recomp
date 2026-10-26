"""Check a generated header against the original module's exports.

  python3 -I tools/check_headers.py <Module>

Parses src/<Module>/Inc/<Module>Classes.h with clang-cl (i686 MSVC ABI),
collects the mangled name of every declared function and variable, and
compares them with the module's MSVC-mangled exports in symbols.db. Also
compares each class's vftable layout (clang's dump) with the binary's.
Prints missing/extra names and a match summary.
"""
import json
import os
import re
import sqlite3
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLANG = '/opt/homebrew/opt/llvm/bin/clang-cl'
SCRATCH = os.environ.get('TMPDIR', '/tmp')


def clang(module, extra):
    header = os.path.join(ROOT, 'src', module, 'Inc', f'{module}Classes.h')
    incs = []
    for d in os.listdir(os.path.join(ROOT, 'src')):
        inc = os.path.join(ROOT, 'src', d, 'Inc')
        if os.path.isdir(inc):
            incs.append(f'-I{inc}')
    cmd = [CLANG, '--target=i686-pc-windows-msvc', '/std:c++17', '/TP',
           '-Wno-microsoft-enum-forward-reference', '-Wno-everything', *incs, *extra, '--', header]
    return subprocess.run(cmd, capture_output=True, text=True)


def mangled_names(module):
    res = clang(module, ['/Zs', '-Xclang', '-ast-dump=json', '-Xclang', '-ast-dump-filter=',
                         '-Xclang', f'-ast-dump-decl-types'] if False else
                ['/Zs', '-Xclang', '-ast-dump=json'])
    names = set()
    # Stream-scan instead of json.loads: the dump is large.
    for m in re.finditer(r'"mangledName":\s*"([^"]+)"', res.stdout):
        names.add(m.group(1).encode().decode('unicode_escape'))
    return names, res.stderr


def vtable_check(module, db):
    """Compare clang's MSVC vftable for each class with the binary's slots."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from gen_headers import Vtables  # noqa: E402
    vt = Vtables()
    classes = [m[4:-5] for (m,) in db.execute(
        "SELECT mangled FROM exports WHERE module=? AND mangled LIKE '??_7%@@6B@'", (module,))]
    classes += [m[4:m.index('@@6B')] for (m,) in db.execute(
        "SELECT mangled FROM exports WHERE module=? AND mangled LIKE '??_7%@@6BUObject@@@'", (module,))]
    classes = [c for c in classes if '@' not in c and '?' not in c]
    tu = os.path.join(SCRATCH, f'vt_{module}.cpp')
    with open(tu, 'w') as f:
        f.write(f'#include "{module}Classes.h"\n')
        for c in classes:
            # A derived probe with an out-of-line ctor forces the full vftable.
            f.write(f'struct Probe_{c} : public {c} {{ Probe_{c}(); }};\n'
                    f'Probe_{c}::Probe_{c}() : {c}(*({c}*)0) {{}}\n')
    incs = [f'-I{os.path.join(ROOT, "src", d, "Inc")}' for d in os.listdir(os.path.join(ROOT, 'src'))]
    res = subprocess.run([CLANG, '--target=i686-pc-windows-msvc', '/std:c++17', '/c',
                          f'/Fo{tu}.obj', '-Wno-everything', '-DGEN_NO_SIZE_ASSERTS', *incs,
                          '-Xclang', '-fdump-vtable-layouts', '--', tu],
                         capture_output=True, text=True)
    dumps = {}
    cur = None
    for line in res.stdout.splitlines():
        m = re.match(r"VFTable for '([^']+)'((?: in '[^']+')*) \((\d+) entries\)", line)
        if m:
            chain = re.findall(r"'([^']+)'", m.group(2))
            owner = (chain[-2] if len(chain) >= 2 else m.group(1)) if chain and chain[-1].startswith('Probe_') \
                else (chain[-1] if chain else m.group(1))
            owner = owner.replace('Probe_', '', 1)
            if chain and chain[-1].startswith('Probe_'):
                owner = chain[-1].replace('Probe_', '', 1)
            cur = dumps.setdefault(owner, []) if owner not in dumps else None
            continue
        m = re.match(r'\s+(\d+) \| (.*)', line)
        if m and cur is not None:
            cur.append(m.group(2))
        elif not line.strip():
            cur = None
    ok = bad = 0
    report = []
    for c in classes:
        slots = vt.slots(module, c) or []
        got = [e for e in dumps.get(c, []) if 'RTTI' not in e]
        if not got:
            report.append(f'{c}: no clang vftable (not dumped)')
            bad += 1
            continue
        mismatch = None
        if len(got) != len(slots):
            mismatch = f'{len(got)} slots vs {len(slots)} in binary'
        else:
            for i, ((_, qual, dem), entry) in enumerate(zip(slots, got)):
                if not qual:
                    continue
                entry = entry.replace('Probe_', '')
                name = re.sub(r'^.*?([A-Za-z_]\w*(?:<[^()]*>)?::~?[\w<>=!+\-*/%&|^\[\]()]+)\(.*$', r'\1', entry)
                if name != qual and not (qual.endswith('::~' + c) and '~' in entry):
                    mismatch = f'slot {i}: clang {name} vs binary {qual}'
                    break
        if mismatch:
            bad += 1
            report.append(f'{c}: {mismatch}')
        else:
            ok += 1
    return ok, bad, report


def main():
    module = sys.argv[1]
    db = sqlite3.connect(os.path.join(ROOT, 'symbols', 'symbols.db'))
    want = {m for (m,) in db.execute(
        "SELECT mangled FROM exports WHERE module=? AND mangled LIKE '?%' "
        "AND mangled NOT LIKE '??_7%' AND mangled NOT LIKE '??_R%' AND mangled NOT LIKE '??_8%'",
        (module,))}
    have, err = mangled_names(module)
    errors = [l for l in err.splitlines() if 'error' in l]
    if errors:
        print(f'{len(errors)} compile errors, first: {errors[0]}')
    matched = want & have
    missing = sorted(want - have)
    print(f'{module}: {len(matched)}/{len(want)} exports declared with identical mangling '
          f'({100 * len(matched) / max(1, len(want)):.1f}%)')
    dem = dict(db.execute('SELECT mangled, demangled FROM exports WHERE module=?', (module,)))
    out = os.path.join(ROOT, 'symbols', f'{module}.mangling_missing.txt')
    with open(out, 'w') as f:
        for m in missing:
            f.write(f'{m}\t{dem.get(m, "")}\n')
    print(f'missing list: {out}')
    ok, bad, report = vtable_check(module, db)
    print(f'{module}: vftables matching binary slot-for-slot: {ok}/{ok + bad}')
    vout = os.path.join(ROOT, 'symbols', f'{module}.vtable_mismatch.txt')
    with open(vout, 'w') as f:
        f.write('\n'.join(report) + '\n')
    if report:
        print(f'vtable mismatches: {vout}')


if __name__ == '__main__':
    main()
