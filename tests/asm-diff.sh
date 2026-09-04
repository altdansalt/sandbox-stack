#!/bin/sh
# Developer check (needs binutils, not part of the TCB): assemble chibicc's x86 text with GNU as and
# compare the normalised disassembly with what cc/asm.c produced. $1 = TU, $2.. = -I flags.
set -e
cd "$(dirname "$0")/.."
TU=$1; shift
T=$(mktemp -d)
./build/chibicc-wasm -mx86 -S "$@" -o $T/a.s $TU
./build/chibicc-wasm -mx86 "$@" -o $T/a.elf $TU
as -o $T/ref.o $T/a.s
objcopy -O binary -j .text $T/ref.o $T/ref.bin
python3 - $T/a.elf $T/mine.bin <<'PY'
import struct, sys
d=open(sys.argv[1],'rb').read(); phoff=struct.unpack_from('<Q',d,32)[0]
p=struct.unpack_from('<IIQQQQQQ',d,phoff); open(sys.argv[2],'wb').write(d[0x1000:p[5]])
PY
norm() { objdump -D -b binary -m i386:x86-64 "$1" | grep -E '^\s+[0-9a-f]+:' | sed -E 's/^\s*[0-9a-f]+:\s*//; s/^([0-9a-f]{2} )+\s*//; s/0x[0-9a-f]+/N/g; s/-N/N/g; s/#.*//; s/\s+$//; s/^mov    N\(%rip\),%rax$/lea    N(%rip),%rax/'; }
norm $T/mine.bin > $T/mine.norm; norm $T/ref.bin > $T/ref.norm
echo "instructions: mine $(wc -l < $T/mine.norm), as $(wc -l < $T/ref.norm)"
if diff $T/mine.norm $T/ref.norm > $T/diff.txt; then echo "asm-diff: IDENTICAL instruction streams"; else echo "asm-diff: DIFFERENCES:"; head -20 $T/diff.txt; exit 1; fi
rm -rf $T
