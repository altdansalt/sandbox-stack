# Pipeline: guest/X.c --clang(untrusted)--> build/X.wasm --w2c2--> build/X/guest.c --tcc--> build/X/sandbox
WCC = clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -Wall -Wno-empty-body -Ilibc
WLD = -Wl,--no-entry -Wl,--export=_start -Wl,--export=memory -Wl,--allow-undefined \
      -Wl,--initial-memory=1048576 -Wl,--max-memory=16777216 -Wl,-z,stack-size=65536
GUESTS = cat rot13 evil grow escape
W2C2 ?= $(HOME)/src/w2c2/w2c2/w2c2
TCC = tcc
ARENA_PAGES ?= 256

all: $(GUESTS:%=build/%/sandbox) build/control build/escape/sandbox-demo

build/escape/sandbox-demo: build/escape/guest.c host/main.c rt/w2c2_base.h
	$(TCC) -nostdlib -static -nostdinc -Irt -Ibuild/escape -DARENA_PAGES=$(ARENA_PAGES) -DDEMO_ESCAPE -o $@ host/main.c $<

build/%.wasm: guest/%.c libc/libc.c libc/libc.h
	@mkdir -p build
	$(WCC) $(WLD) -o $@ guest/$*.c libc/libc.c

build/%/guest.c: build/%.wasm
	@mkdir -p build/$*
	cp $< build/$*/guest.wasm
	$(W2C2) build/$*/guest.wasm $@

build/%/sandbox: build/%/guest.c host/main.c rt/w2c2_base.h
	$(TCC) -nostdlib -static -nostdinc -Irt -Ibuild/$* -DARENA_PAGES=$(ARENA_PAGES) -o $@ host/main.c $<

build/control: control/main.go
	cd control && go build -o ../build/control .

clean:
	rm -rf build
.PHONY: all clean
.SECONDARY:

# ---- Experiment 2: w2c2 itself as a guest ----
W2C2_SRC = $(filter-out w2c2/main.c, $(wildcard w2c2/*.c)) w2c2/main.c
W2C2_CFLAGS = -std=c89 -Wno-long-long -DHAS_UNISTD=1
W2C2_WLD = -Wl,--no-entry -Wl,--export=_start -Wl,--export=memory -Wl,--allow-undefined \
      -Wl,--initial-memory=4194304 -Wl,--max-memory=268435456 -Wl,-z,stack-size=1048576

# native w2c2 from the vendored, patched source (build tool; compiled with clang)
build/w2c2-native: $(W2C2_SRC) $(wildcard w2c2/*.h)
	@mkdir -p build
	clang -O2 $(W2C2_CFLAGS) -Wno-unused-function -o $@ $(W2C2_SRC)

build/w2c2.wasm: $(W2C2_SRC) $(wildcard w2c2/*.h) libc/libc.c libc/stdio.c $(wildcard libc/include/*.h)
	@mkdir -p build
	$(WCC) -nostdinc -Ilibc/include $(W2C2_CFLAGS) -Wno-unused-function -DGUEST_ARGV='"w2c2","guest.wasm","guest.c"' \
	    $(W2C2_WLD) -o $@ $(W2C2_SRC) libc/libc.c libc/stdio.c

build/w2c2/sandbox: build/w2c2.wasm host/main.c rt/w2c2_base.h build/w2c2-native
	@mkdir -p build/w2c2
	cp build/w2c2.wasm build/w2c2/guest.wasm
	./build/w2c2-native build/w2c2/guest.wasm build/w2c2/guest.c
	$(TCC) -nostdlib -static -nostdinc -Irt -Ibuild/w2c2 -DARENA_PAGES=4096 -o $@ host/main.c build/w2c2/guest.c
include tests/tests.mk

# ---- Experiment 3: C -> wasm with the chibicc backend (no clang) ----
CC_SRCS = $(wildcard cc/*.c cc/*.h)
build/chibicc-wasm: $(CC_SRCS)
	@mkdir -p build
	clang -O2 -Wno-unused-function -Wno-unused-variable -Wno-switch -o $@ $(filter %.c,$(CC_SRCS))

# single translation unit per guest: the guest source plus the libc
build/cc/%.c: guest/%.c libc/libc.c
	@mkdir -p build/cc
	printf '#include "guest/$*.c"\n#include "libc/libc.c"\n' > $@

build/cc/%.wasm: build/cc/%.c build/chibicc-wasm libc/libc.h
	./build/chibicc-wasm -I. -Ilibc -Ilibc/include -mmaxpages=256 -o $@ $<

build/cc/%/guest.c: build/cc/%.wasm build/w2c2-native
	@mkdir -p build/cc/$*
	cp $< build/cc/$*/guest.wasm
	./build/w2c2-native build/cc/$*/guest.wasm $@

build/cc/%/sandbox: build/cc/%/guest.c host/main.c rt/w2c2_base.h
	$(TCC) -nostdlib -static -nostdinc -Irt -Ibuild/cc/$* -DARENA_PAGES=$(ARENA_PAGES) -o $@ host/main.c $<

build/cc/w2c2.c: $(W2C2_SRC) libc/libc.c libc/stdio.c
	@mkdir -p build/cc
	(printf '#include "libc/libc.c"\n#include "libc/stdio.c"\n'; for f in $(W2C2_SRC); do printf '#include "%s"\n' $$f; done) > $@

build/cc/w2c2.wasm: build/cc/w2c2.c build/chibicc-wasm $(wildcard w2c2/*.h) $(wildcard libc/include/*.h)
	./build/chibicc-wasm -I. -Ilibc -Ilibc/include -Iw2c2 -DHAS_UNISTD=1 -DGUEST_ARGV='"w2c2","guest.wasm","guest.c"' -mstack=4194304 -mmaxpages=4096 -o $@ $<

exp3-guests: $(GUESTS:%=build/cc/%/sandbox)
.PHONY: exp3-guests
