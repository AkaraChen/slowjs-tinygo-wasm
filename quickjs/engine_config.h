/* The defines every C file of this package is built with, on top of the flags
   in cgo.go (cgo cannot pass a quoted string in CFLAGS, so they live here). */
#ifndef SLOWJS_TINYGO_ENGINE_CONFIG_H
#define SLOWJS_TINYGO_ENGINE_CONFIG_H

/* the version of upstream QuickJS the engine is (engine/test/flags.sh) */
#define CONFIG_VERSION "2026-06-04"

/* wasm32 has 4-byte pointers: keep the 16-byte struct JSValue that the
   register VM of liustack/quickjs needs (patches/0001-wasm32-engine.patch) */
#define JS_NO_NAN_BOXING 1

/* engine/heap.c and tlsf.c are written for 64-bit targets only: every block
   comes from heap_wasm.c instead, on top of the C malloc of TinyGo */
#define JS_HEAP_SYSTEM_MALLOC 1

/* no generator adds polls to this build: the engine calls host_poll each
   time it reads its interrupt flag (cutils.h) */
#define JS_HOST_POLL 1

#endif
