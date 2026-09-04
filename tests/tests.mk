# Adversarial + probe test suite. `make test` builds each guest through the full
# pipeline (native w2c2 -> tcc -> seccomp host) and asserts exit codes.
# Trap exit code = 100 + trap enum (rt/w2c2_base.h): 106 OOB memory, 107 OOB table,
# 108 null function, 109 indirect-call type mismatch.
ADV = positive oob_load mem_init_oob elem_oob callind_typemismatch
ADV_WASM = $(ADV:%=build/adv/%.wasm)

$(ADV_WASM): tests/mkwasm.py
	@mkdir -p build/adv
	python3 tests/mkwasm.py build/adv

build/adv/%/sandbox: build/adv/%.wasm host/main.c rt/w2c2_base.h build/w2c2-native
	@mkdir -p build/adv/$*
	cp build/adv/$*.wasm build/adv/$*/guest.wasm
	./build/w2c2-native build/adv/$*/guest.wasm build/adv/$*/guest.c 2>/dev/null
	$(TCC) -nostdlib -static -nostdinc -Irt -Ibuild/adv/$* -DARENA_PAGES=256 -o $@ host/main.c build/adv/$*/guest.c

.PHONY: test
test: all build/w2c2-native $(ADV:%=build/adv/%/sandbox) \
      build/rot13/sandbox build/cat/sandbox build/evil/sandbox build/grow/sandbox \
      build/escape/sandbox build/escape/sandbox-demo
	@echo "=== functional ==="; \
	bash tests/run.sh

# chibicc-wasm regression tests (Experiment 3)
test: test-cc
test-cc: build/chibicc-wasm build/control
	./tests/cc/run.sh
.PHONY: test-cc
