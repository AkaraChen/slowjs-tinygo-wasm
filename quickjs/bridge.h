/* What the Go side of this package calls (host_wasm.c). Plain C types only:
   the cgo preamble includes this file and nothing of the engine. */
#ifndef SLOWJS_TINYGO_BRIDGE_H
#define SLOWJS_TINYGO_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct qjs_vm qjs_vm;

/* A runtime and one context, with print and console.log. seed starts
   Math.random. max_call_depth > 0 sets JS_SetMaxCallDepth. NULL when there is
   no memory or 64 VMs are open. */
qjs_vm *qjs_new(uint64_t seed, int max_call_depth);
/* why the last qjs_new returned NULL */
const char *qjs_new_error(void);
void qjs_free(qjs_vm *vm);
uint32_t qjs_id(qjs_vm *vm);
void qjs_set_memory_limit(qjs_vm *vm, size_t limit);
/* JS_SetMaxCallDepth: 0 when it took depth, else -1 */
int qjs_set_max_call_depth(qjs_vm *vm, int depth);

/* Evaluates the len bytes at src (src[len] must be 0) as a global script,
   then runs the pending jobs. Returns 1 with the result as text in *out, or 0
   with the exception as text. *out is freed with qjs_free_str. timeout_ns > 0
   stops the script after that long. */
int qjs_eval(qjs_vm *vm, const char *src, size_t len, const char *filename,
             int64_t timeout_ns, char **out, size_t *out_len);
void qjs_free_str(char *s);

#endif
