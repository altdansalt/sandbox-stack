#!/bin/sh
# Byte-exact assembler regression: assemble tests/asm/cases.s with cc/asm.c and with GNU as (developer tool,
# not TCB) and compare the normalised disassembly. GOTPCREL mov is expected to become lea.
cd "$(dirname "$0")/../.." || exit 1
T=$(mktemp -d)
./build/chibicc-wasm -masm -o $T/mine.elf tests/asm/cases.s || { echo "FAIL asm/cases (assembler error)"; exit 1; }
as -o $T/ref.o tests/asm/cases.s || { echo "FAIL asm/cases (GNU as rejected the test file)"; exit 1; }
objcopy -O binary -j .text $T/ref.o $T/ref.bin
python3 - $T/mine.elf $T/mine.bin <<'PY'
import struct, sys
d=open(sys.argv[1],'rb').read(); phoff=struct.unpack_from('<Q',d,32)[0]
p=struct.unpack_from('<IIQQQQQQ',d,phoff); open(sys.argv[2],'wb').write(d[0x1000:p[5]])
PY
norm() { objdump -D -b binary -m i386:x86-64 "$1" | grep -E '^\s+[0-9a-f]+:' | sed -E 's/^\s*[0-9a-f]+:\s*//; s/^([0-9a-f]{2} )+\s*//; s/0x[0-9a-f]+/N/g; s/-N/N/g; s/#.*//; s/\s+$//; s/^mov    N\(%rip\),%rax$/lea    N(%rip),%rax/'; }
norm $T/mine.bin > $T/mine.norm; norm $T/ref.bin > $T/ref.norm
fail=0
if diff $T/mine.norm $T/ref.norm > $T/d; then echo "PASS asm/cases ($(wc -l < $T/mine.norm) instructions identical to GNU as)"; else echo "FAIL asm/cases:"; head -20 $T/d; fail=1; fi
for r in tests/asm/reject/*.s; do
  if ./build/chibicc-wasm -masm -o $T/r.elf $r >/dev/null 2>&1; then echo "FAIL asm/reject/$(basename $r .s) (accepted)"; fail=1; else echo "PASS asm/reject/$(basename $r .s) (rejected)"; fi
done
rm -rf $T; exit $fail
