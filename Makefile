# Build and check the TinyGo wasm build of liustack/quickjs. See README.md.

TINYGO   ?= tinygo
WASMTIME ?= wasmtime
NODE     ?= node
# no DWARF: 1.5 MB in place of 6.8 MB (a trap then shows no C line numbers)
TINYGOFLAGS ?= -no-debug

# the target: wasip1, no scheduler, a 4 MiB C stack (targets/wasip1-quickjs.json)
TARGET   := ./targets/wasip1-quickjs.json
# TinyGo checks #cgo CFLAGS against an allow list, as go does: these three
# are the flags the engine needs that are not on it (engine/test/flags.sh)
export CGO_CFLAGS_ALLOW := -fwrapv|-fno-math-errno|-ffp-contract=off
# a native wasm stack of 4 MiB, so that deep recursion through built-ins ends
# in a RangeError rather than a trap of wasmtime (README.md, limits)
WASMTIME_RUN := $(WASMTIME) run -W max-wasm-stack=4194304

.PHONY: all build test test-wasmtime test-node example vendor clean

all: build

build: build/qjs.wasm build/selftest.wasm

build/qjs.wasm: FORCE
	@mkdir -p build
	$(TINYGO) build $(TINYGOFLAGS) -target=$(TARGET) -o $@ ./cmd/qjs

build/selftest.wasm: FORCE
	@mkdir -p build
	$(TINYGO) build $(TINYGOFLAGS) -target=$(TARGET) -o $@ ./cmd/selftest

test: test-wasmtime test-node

test-wasmtime: build/selftest.wasm
	$(WASMTIME_RUN) build/selftest.wasm

test-node: build/selftest.wasm
	$(NODE) --no-warnings examples/node/run.mjs build/selftest.wasm

example: build/qjs.wasm
	$(WASMTIME_RUN) build/qjs.wasm -e '1 + 2'
	$(WASMTIME_RUN) build/qjs.wasm -e 'function add(a, b) { return a + b } add(40, 2)'
	$(NODE) --no-warnings examples/node/run.mjs build/qjs.wasm -e '[1, 2, 3].map(x => x * x)'

# re-create third_party/quickjs-engine and quickjs/internal/libm from upstream
vendor:
	./scripts/vendor-engine.sh

clean:
	rm -rf build

FORCE:
