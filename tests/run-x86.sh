#!/bin/sh
# Differential test of the host compiler: for every guest that has a tcc-built sandbox
# (probes and adversarial modules), build the same host+guest with chibicc's x86-64 backend
# and require identical stdout and exit code on the same input.
cd "$(dirname "$0")/.." || exit 1
fail=0; n=0
for dir in build/cat build/rot13 build/evil build/grow build/escape build/adv/*; do
  [ -f $dir/guest.c ] && [ -x $dir/sandbox ] || continue
  name=${dir#build/}; out=build/x86/$name; mkdir -p $out
  printf '#include "host/main.c"\n#include "%s/guest.c"\n' $dir > $out/host.c
  if ! ./build/chibicc-wasm -mx86 -I. -Irt -I$dir -DARENA_PAGES=256 -o $out/sandbox $out/host.c; then echo "FAIL x86/$name (compile)"; fail=1; continue; fi
  chmod +x $out/sandbox
  printf 'Hello, World\n' | $dir/sandbox > $out/tcc.out 2>&1; a=$?
  printf 'Hello, World\n' | $out/sandbox > $out/x86.out 2>&1; b=$?
  n=$((n+1))
  if [ $a -eq $b ] && cmp -s $out/tcc.out $out/x86.out; then echo "PASS x86/$name (exit $a, same output as tcc host)"; else echo "FAIL x86/$name (tcc exit $a, chibicc exit $b)"; diff $out/tcc.out $out/x86.out | head -3; fail=1; fi
done
echo "x86 host differential: $n cases"; [ $fail -eq 0 ] && echo "=== ALL PASS ===" ; exit $fail
