#!/bin/sh
# Native x86-64 tests: each tests/x86c/*.c is compiled with -mx86 (no libc) and run; expected exit 0.
# Files listed in tests/x86c/must-fail must be rejected by the compiler.
cd "$(dirname "$0")/../.." || exit 1
fail=0
for src in tests/x86c/*.c; do
  name=$(basename $src .c); out=build/x86c/$name; mkdir -p build/x86c
  if grep -qx "$name" tests/x86c/must-fail; then
    if ./build/chibicc-wasm -mx86 -Itests/x86c -Ilibc/include -o $out $src >/dev/null 2>&1; then echo "FAIL x86c/$name (compiled, expected rejection)"; fail=1; else echo "PASS x86c/$name (rejected as expected)"; fi
    continue
  fi
  if ! ./build/chibicc-wasm -mx86 -Itests/x86c -Ilibc/include -o $out $src; then echo "FAIL x86c/$name (compile)"; fail=1; continue; fi
  $out; rc=$?
  if [ $rc -eq 0 ]; then echo "PASS x86c/$name"; else echo "FAIL x86c/$name (exit $rc)"; fail=1; fi
done
exit $fail
