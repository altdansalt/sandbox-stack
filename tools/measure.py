#!/usr/bin/env python3
"""Count lines (tokei) and tokens (tiktoken o200k_base) of each trusted component.
Usage: .venv/bin/python tools/measure.py  -> prints a markdown table."""
import json, os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.expanduser('~')
TCC = os.path.join(HOME, 'src/tinycc')
TCC_X86_64_LINUX = ['tcc.c', 'tcctools.c', 'libtcc.c', 'tccpp.c', 'tccgen.c', 'tccelf.c', 'tccasm.c', 'tccrun.c',
                    'tcc.h', 'libtcc.h', 'tcctok.h', 'x86_64-gen.c', 'x86_64-link.c', 'i386-asm.c', 'x86_64-asm.h',
                    'i386-asm.h', 'i386-tok.h', 'elf.h', 'stab.h', 'stab.def']
W2C2 = os.path.join(ROOT, 'w2c2')
def w2c2_files():
    return sorted(os.path.join(W2C2, f) for f in os.listdir(W2C2)
                  if (f.endswith('.c') or f.endswith('.h')) and not f.endswith('_test.c') and not f.endswith('_test.h')
                  and f != 'test.c' and f != 'w2c2_base.h')
COMPONENTS = [
    ('w2c2 core (vendored, patched; excludes tests and upstream w2c2_base.h)', w2c2_files),
    ('w2c2 runtime header (rt/w2c2_base.h, rewritten)', lambda: [os.path.join(ROOT, 'rt/w2c2_base.h')]),
    ('tcc (x86_64 Linux build inputs, commit 62c30a4a)', lambda: [os.path.join(TCC, f) for f in TCC_X86_64_LINUX]),
    ('minimal libc (libc/)', lambda: sorted(os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(ROOT, 'libc')) for f in fs)),
    ('host (host/main.c)', lambda: [os.path.join(ROOT, 'host/main.c')]),
]
import tiktoken
enc = tiktoken.get_encoding('o200k_base')
def tokei(files):
    out = subprocess.run(['tokei', '-o', 'json'] + files, capture_output=True, text=True).stdout
    d = json.loads(out)
    return sum(v['code'] for k, v in d.items() if k != 'Total'), sum(v['comments'] for k, v in d.items() if k != 'Total'), sum(v['blanks'] for k, v in d.items() if k != 'Total')
rows, tl, tt = [], 0, 0
for name, fn in COMPONENTS:
    files = [f for f in fn() if os.path.exists(f)]
    if not files: rows.append((name, 0, 0, 0, 0, 0)); continue
    code, com, blank = tokei(files)
    toks = sum(len(enc.encode(open(f, errors='replace').read())) for f in files)
    b = sum(os.path.getsize(f) for f in files)
    rows.append((name, len(files), code, com, toks, b)); tl += code; tt += toks
print('| component | files | code lines | comment lines | tokens (o200k_base) | bytes |')
print('|---|---:|---:|---:|---:|---:|')
for r in rows: print('| %s | %d | %d | %d | %d | %d |' % r)
print('| **total** | | **%d** | | **%d** | |' % (tl, tt))
