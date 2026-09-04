# Pipeline: guest/X.c --clang(untrusted)--> build/X.wasm --w2c2--> build/X/guest.c --tcc--> build/X/sandbox
WCC = clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -Wall -Wno-empty-body
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
