#!/bin/sh
# Fixpoint check for a translator wasm: $1 = w2c2 guest wasm, $2 = tag, HOSTCC=tcc|chibicc (host compiler).
# Translates rot13 (cc-built) and the translator itself, in the sandbox and under wazero,
# and diffs against native w2c2 (build/w2c2-native) on the same inputs.
set -e
W=$1; TAG=$2; D=build/fix-$TAG; rm -rf $D; mkdir -p $D/w2c2
cp $W $D/w2c2/guest.wasm
./build/w2c2-native $D/w2c2/guest.wasm $D/w2c2/guest.c 2>/dev/null
hostcc() { # $1 = dir with guest.c/guest.h, $2 = output binary
  if [ "${HOSTCC:-tcc}" = chibicc ]; then
    printf '#include "host/main.c"\n#include "%s/guest.c"\n' $1 > $1/host_tu.c
    ./build/chibicc-wasm -mx86 -I. -Irt -I$1 -DARENA_PAGES=4096 -o $2 $1/host_tu.c && chmod +x $2
  else
    tcc -nostdlib -static -nostdinc -Irt -I$1 -DARENA_PAGES=4096 -o $2 host/main.c $1/guest.c
  fi
}
hostcc $D/w2c2 $D/sandbox
for input in build/cc/rot13.wasm $W; do
  name=$(basename $input .wasm); mkdir -p $D/native-$name $D/sbx-$name $D/ctl-$name
  cp $input $D/native-$name/guest.wasm
  ./build/w2c2-native $D/native-$name/guest.wasm $D/native-$name/guest.c 2>/dev/null
  rm $D/native-$name/guest.wasm
  ./$D/sandbox < $input > $D/sbx-$name.out 2>/dev/null; python3 tools/split.py $D/sbx-$name.out $D/sbx-$name >/dev/null
  ./build/control $W < $input > $D/ctl-$name.out 2>/dev/null; python3 tools/split.py $D/ctl-$name.out $D/ctl-$name >/dev/null
  if diff -r $D/native-$name $D/sbx-$name >/dev/null; then echo "[$TAG] $name: sandbox == native ($(wc -c < $D/native-$name/guest.c) bytes of C)"; else echo "[$TAG] $name: sandbox DIFFERS from native"; fi
  if diff -r $D/native-$name $D/ctl-$name >/dev/null; then echo "[$TAG] $name: wazero  == native"; else echo "[$TAG] $name: wazero DIFFERS from native"; fi
done
# gen-2: compile the C the sandboxed translator produced for itself, compare binaries
mkdir -p $D/gen2 && cp $D/sbx-$(basename $W .wasm)/guest.c $D/sbx-$(basename $W .wasm)/guest.h $D/gen2/
hostcc $D/gen2 $D/gen2/sandbox
cmp $D/gen2/sandbox $D/sandbox && echo "[$TAG] gen-2 sandbox binary identical to gen-1"
