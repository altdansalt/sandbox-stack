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
CC = os.path.join(ROOT, 'cc')
CC_UP = os.path.join(HOME, 'src/chibicc')
def cc_files():
    return sorted(os.path.join(CC, f) for f in os.listdir(CC) if f.endswith('.c') or f.endswith('.h'))
COMPONENTS = [
    ('w2c2 core (vendored, patched; excludes tests and upstream w2c2_base.h)', w2c2_files),
    ('w2c2 runtime header (rt/w2c2_base.h, rewritten)', lambda: [os.path.join(ROOT, 'rt/w2c2_base.h')]),
    ('tcc (x86_64 Linux build inputs, commit 62c30a4a)', lambda: [os.path.join(TCC, f) for f in TCC_X86_64_LINUX]),
    ('minimal libc (libc/)', lambda: sorted(os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(ROOT, 'libc')) for f in fs)),
    ('host (host/main.c)', lambda: [os.path.join(ROOT, 'host/main.c')]),
    ('chibicc-wasm (cc/, Experiment 3; replaces clang)', cc_files),
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
print('| **total (tcc as host compiler)** | | **%d** | | **%d** | |' % (tl, tt))
tcc_row = [r for r in rows if r[0].startswith('tcc')][0]
print('| **total (chibicc x86-64 as host compiler, tcc excluded)** | | **%d** | | **%d** | |' % (tl - tcc_row[2], tt - tcc_row[4]))


# ---- Experiment 3: tokens changed versus upstream chibicc ----
import difflib
def toks(text): return len(enc.encode(text))
print()
print('| chibicc file | upstream tokens | ours | tokens removed | tokens added |')
print('|---|---:|---:|---:|---:|')
trem = tadd = 0
for f in sorted(set(os.listdir(CC_UP)) | set(os.listdir(CC))):
    if not (f.endswith('.c') or f.endswith('.h')): continue
    up = 'codegen.c' if f == 'codegen_x86.c' else f   # our x86 backend is upstream's codegen.c
    a = open(os.path.join(CC_UP, up)).read().splitlines(True) if os.path.exists(os.path.join(CC_UP, up)) else []
    b = open(os.path.join(CC, f)).read().splitlines(True) if os.path.exists(os.path.join(CC, f)) else []
    if not a and not b: continue
    rem = add = 0
    for line in difflib.unified_diff(a, b, n=0):
        if line.startswith('---') or line.startswith('+++') or line.startswith('@@'): continue
        if line.startswith('-'): rem += toks(line[1:])
        elif line.startswith('+'): add += toks(line[1:])
    if rem or add or not a or not b:
        print('| %s | %d | %d | %d | %d |' % (f, toks(''.join(a)), toks(''.join(b)), rem, add))
    trem += rem; tadd += add
print('| **total changed** | | | **%d** | **%d** |' % (trem, tadd))
