#!/bin/sh
# Compile each tests/cc/*.c with chibicc-wasm (plus the libc) and run it under wazero; expect exit 0.
# Files listed in tests/cc/must-fail are expected to be rejected by the compiler.
cd "$(dirname "$0")/../.." || exit 1
fail=0
for src in tests/cc/*.c; do
  name=$(basename $src .c); tu=build/cc/test-$name.c
  printf '#include "%s"\n#include "libc/libc.c"\n' $src > $tu
  if grep -qx "$name" tests/cc/must-fail; then
    if ./build/chibicc-wasm -I. -Ilibc -Ilibc/include -o build/cc/test-$name.wasm $tu >/dev/null 2>&1; then echo "FAIL cc/$name (compiled, expected rejection)"; fail=1; else echo "PASS cc/$name (rejected as expected)"; fi
    continue
  fi
  if ! ./build/chibicc-wasm -I. -Ilibc -Ilibc/include -o build/cc/test-$name.wasm $tu; then echo "FAIL cc/$name (compile)"; fail=1; continue; fi
  ./build/control build/cc/test-$name.wasm </dev/null; rc=$?
  if [ $rc -eq 0 ]; then echo "PASS cc/$name"; else echo "FAIL cc/$name (exit $rc)"; fail=1; fi
done
exit $fail
