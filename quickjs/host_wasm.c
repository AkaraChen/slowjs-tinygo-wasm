/* The host of the engine (engine/host.h) for TinyGo on wasip1, and the few
   C entry points the Go side of this package calls (bridge.h).

   Upstream liustack/quickjs implements host.h in Go for its generated
   assembly, and in C for its native test host (engine/test/host.c). Neither
   builds here: this is the smallest host that runs scripts. One context per
   VM, clocks from WASI, an optional deadline polled through host_poll, and
   one host function, print / console.log, which hands its text to Go. */
#include "engine_config.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../third_party/quickjs-engine/quickjs.h"
#include "../third_party/quickjs-engine/host.h"
#include "bridge.h"

void *malloc(size_t size);
void free(void *ptr);

/* Go (quickjs.go, //export qjsGoPrint): the text of one print call */
extern void qjsGoPrint(uint32_t vm, char *s, size_t n);

/* ---- clocks: the WASI import itself, no libc in between ---- */

uint16_t qjs_wasi_clock_time_get(uint32_t id, uint64_t precision, uint64_t *time)
    __attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_time_get")));

static int64_t wasi_now(uint32_t clock_id)
{
    uint64_t t = 0;
    if (qjs_wasi_clock_time_get(clock_id, 1, &t) != 0)
        return 0;
    return (int64_t)t;
}

int64_t host_now(uint32_t vm, int kind)
{
    (void)vm;
    /* WASI clock ids: 0 realtime, 1 monotonic */
    return wasi_now(kind == HOST_NOW_MONOTONIC ? 1 : 0);
}

/* ---- the VMs ---- */

#define QJS_MAX_VMS 64

struct qjs_vm {
    uint32_t id;
    JSRuntime *rt;
    JSContext *ctx;
    int64_t deadline; /* monotonic ns, 0 for none */
    uint32_t polls;
    int timed_out;
};

static qjs_vm *vms[QJS_MAX_VMS + 1]; /* by id, 1 to QJS_MAX_VMS */

static qjs_vm *vm_of(uint32_t id)
{
    return id >= 1 && id <= QJS_MAX_VMS ? vms[id] : NULL;
}

/* ---- the stack ----

   The engine bounds its recursion by the bytes of stack left. In the
   generated assembly of upstream that is the room on the VM's shadow stack.
   In wasm, C keeps the locals whose address is taken and every alloca on the
   shadow stack in linear memory, below __stack_pointer: TinyGo links with
   --stack-first, so that stack runs from __stack_high down to __stack_low.
   This is right only while the engine runs on that stack, the main stack of
   the module, which is why the package needs -scheduler=none (a goroutine of
   the asyncify scheduler runs on a stack of its own). The other stack of wasm,
   the one of the engine running the module, is out of reach of C: a deep
   recursion is bounded there by JS_SetMaxCallDepth (qjs_new). */
extern unsigned char __stack_low;
extern unsigned char __stack_high;

long __shadow_remaining(void)
{
    volatile unsigned char here = 0;
    uintptr_t sp = (uintptr_t)&here;
    uintptr_t low = (uintptr_t)&__stack_low;
    uintptr_t high = (uintptr_t)&__stack_high;

    if (sp <= low || sp > high)
        return 0;
    return (long)(sp - low);
}

/* ---- polls: the deadline of an eval ---- */

/* The VMs whose eval has a deadline, found by their interrupt flag. The
   engine reads its flag at every call and backward jump, so the common case,
   no deadline at all, must cost one load and one compare. */
static int deadlines;

int host_poll(const volatile int *flag)
{
    if (deadlines) {
        /* every 1024 reads of the flag of a VM with a deadline, look at the
           clock, and stop the VM when it is past */
        for (uint32_t i = 1; i <= QJS_MAX_VMS; i++) {
            qjs_vm *vm = vms[i];
            if (!vm || !vm->deadline || JS_GetInterruptFlag(vm->rt) != flag)
                continue;
            if ((++vm->polls & 1023) == 0 && wasi_now(1) >= vm->deadline) {
                vm->timed_out = 1;
                *(volatile int *)flag = 1;
            }
            break;
        }
    }
    return *flag;
}

/* ---- host functions ---- */

enum { QJS_FN_PRINT = 0 };

static JSValue host_print(qjs_vm *vm, int argc, JSValueConst *argv)
{
    /* the arguments joined by spaces, then one call of Go */
    size_t cap = 256, n = 0;
    char *buf = malloc(cap);

    if (!buf)
        return JS_ThrowOutOfMemory(vm->ctx);
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *s = JS_ToCStringLen(vm->ctx, &len, argv[i]);
        if (!s) {
            free(buf);
            return JS_EXCEPTION;
        }
        if (n + len + 2 > cap) {
            size_t ncap = (n + len + 2) * 2;
            char *nb = malloc(ncap);
            if (!nb) {
                JS_FreeCString(vm->ctx, s);
                free(buf);
                return JS_ThrowOutOfMemory(vm->ctx);
            }
            memcpy(nb, buf, n);
            free(buf);
            buf = nb;
            cap = ncap;
        }
        if (i > 0)
            buf[n++] = ' ';
        memcpy(buf + n, s, len);
        n += len;
        JS_FreeCString(vm->ctx, s);
    }
    qjsGoPrint(vm->id, buf, n);
    free(buf);
    return JS_UNDEFINED;
}

JSValue host_call(uint32_t id, const JSValue *this_val, int argc, JSValueConst *argv, int magic)
{
    qjs_vm *vm = vm_of(id);

    (void)this_val;
    if (vm && magic == QJS_FN_PRINT)
        return host_print(vm, argc, argv);
    /* HOST_CALL_ABORT, or a request this host never asked for: the
       engine cannot go on, and wasm has no better way to say so */
    __builtin_trap();
}

void host_release(uint32_t vm, uint32_t handle)
{
    (void)vm;
    (void)handle;
}

/* ---- bridge.h ---- */

static int add_print(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue print = JS_NewHostFunction(ctx, "print", 5, 1, QJS_FN_PRINT);
    JSValue console;
    int ok = 0;

    if (JS_IsException(print))
        goto done;
    if (JS_SetPropertyStr(ctx, global, "print", JS_DupValue(ctx, print)) < 0)
        goto done;
    console = JS_NewObject(ctx);
    if (JS_IsException(console))
        goto done;
    if (JS_SetPropertyStr(ctx, console, "log", JS_DupValue(ctx, print)) < 0 ||
        JS_SetPropertyStr(ctx, global, "console", console) < 0)
        goto done;
    ok = 1;
done:
    JS_FreeValue(ctx, print);
    JS_FreeValue(ctx, global);
    return ok;
}

static const char *new_error = "";

const char *qjs_new_error(void)
{
    return new_error;
}

qjs_vm *qjs_new(uint64_t seed, int max_call_depth)
{
    uint32_t id;
    qjs_vm *vm;

    for (id = 1; id <= QJS_MAX_VMS && vms[id]; id++)
        ;
    if (id > QJS_MAX_VMS) {
        new_error = "64 VMs are open";
        return NULL;
    }
    vm = malloc(sizeof(*vm));
    if (!vm) {
        new_error = "out of memory";
        return NULL;
    }
    memset(vm, 0, sizeof(*vm));
    vm->id = id;
    vm->rt = JS_NewRuntime(id, seed);
    if (!vm->rt) {
        new_error = "JS_NewRuntime failed";
        free(vm);
        return NULL;
    }
    vms[id] = vm;
    if (max_call_depth > 0)
        JS_SetMaxCallDepth(vm->rt, max_call_depth);
    vm->ctx = JS_NewContext(vm->rt);
    if (!vm->ctx || !add_print(vm->ctx)) {
        new_error = vm->ctx ? "cannot add print" : "JS_NewContext failed";
        qjs_free(vm);
        return NULL;
    }
    return vm;
}

void qjs_free(qjs_vm *vm)
{
    if (!vm)
        return;
    if (vm->ctx)
        JS_FreeContext(vm->ctx);
    JS_FreeRuntime(vm->rt);
    vms[vm->id] = NULL;
    free(vm);
}

uint32_t qjs_id(qjs_vm *vm)
{
    return vm->id;
}

void qjs_set_memory_limit(qjs_vm *vm, size_t limit)
{
    JS_SetMemoryLimit(vm->rt, limit);
}

int qjs_set_max_call_depth(qjs_vm *vm, int depth)
{
    return JS_SetMaxCallDepth(vm->rt, depth);
}

/* a copy of the n bytes at s, with a NUL after them, in *out */
static int copy_out(const char *s, size_t n, char **out, size_t *out_len)
{
    char *p = malloc(n + 1);
    if (!p)
        return 0;
    memcpy(p, s, n);
    p[n] = 0;
    *out = p;
    *out_len = n;
    return 1;
}

static int copy_value(JSContext *ctx, JSValueConst v, char **out, size_t *out_len)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    int ok;

    if (!s)
        return 0;
    ok = copy_out(s, n, out, out_len);
    JS_FreeCString(ctx, s);
    return ok;
}

/* The exception as text: "Name: message", and its stack when it has one. */
static void exception_text(qjs_vm *vm, char **out, size_t *out_len)
{
    JSContext *ctx = vm->ctx;
    JSValue e = JS_GetException(ctx);

    if (vm->timed_out) {
        copy_out("InternalError: interrupted (eval timeout)", 41, out, out_len);
    } else if (!copy_value(ctx, e, out, out_len)) {
        JSValue none = JS_GetException(ctx);
        JS_FreeValue(ctx, none);
        copy_out("<exception that cannot be converted to a string>", 48, out, out_len);
    } else if (JS_IsError(ctx, e)) {
        JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
        size_t n;
        const char *s = JS_IsException(stack) || JS_IsUndefined(stack) ? NULL : JS_ToCStringLen(ctx, &n, stack);
        if (s && n > 0) {
            char *joined = malloc(*out_len + 1 + n + 1);
            if (joined) {
                memcpy(joined, *out, *out_len);
                joined[*out_len] = '\n';
                memcpy(joined + *out_len + 1, s, n);
                joined[*out_len + 1 + n] = 0;
                free(*out);
                *out = joined;
                *out_len += 1 + n;
            }
        }
        if (s)
            JS_FreeCString(ctx, s);
        else if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, e);
}

/* The result as text: JSON for arrays and plain objects (what a REPL shows),
   String(v) for the rest. */
static int result_text(JSContext *ctx, JSValueConst v, char **out, size_t *out_len)
{
    if (JS_IsObject(v) && !JS_IsFunction(ctx, v) && !JS_IsError(ctx, v)) {
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        int ok;
        if (JS_IsException(json))
            return 0;
        if (JS_IsUndefined(json)) {
            JS_FreeValue(ctx, json);
            return copy_value(ctx, v, out, out_len);
        }
        ok = copy_value(ctx, json, out, out_len);
        JS_FreeValue(ctx, json);
        return ok;
    }
    return copy_value(ctx, v, out, out_len);
}

int qjs_eval(qjs_vm *vm, const char *src, size_t len, const char *filename,
             int64_t timeout_ns, char **out, size_t *out_len)
{
    JSContext *ctx = vm->ctx;
    JSValue v;
    int ok;

    *out = NULL;
    *out_len = 0;
    vm->timed_out = 0;
    vm->polls = 0;
    vm->deadline = timeout_ns > 0 ? wasi_now(1) + timeout_ns : 0;
    if (vm->deadline)
        deadlines++;
    /* JS_Eval wants src[len] == 0: Go hands over a copy that has it */
    v = JS_Eval(ctx, src, len, filename, JS_EVAL_TYPE_GLOBAL);
    /* run the jobs the script queued (promise reactions) */
    while (!JS_IsException(v) && JS_IsJobPending(vm->rt)) {
        JSContext *jctx;
        if (JS_ExecutePendingJob(vm->rt, &jctx) < 0) {
            JS_FreeValue(ctx, v);
            v = JS_EXCEPTION;
        }
    }
    if (JS_IsException(v)) {
        exception_text(vm, out, out_len);
        ok = 0;
    } else {
        ok = result_text(ctx, v, out, out_len);
        if (!ok)
            exception_text(vm, out, out_len);
    }
    JS_FreeValue(ctx, v);
    if (vm->deadline)
        deadlines--;
    vm->deadline = 0;
    if (vm->timed_out) {
        /* the host clears its stop before it runs code in the VM again
           (JS_GetInterruptFlag) */
        *JS_GetInterruptFlag(vm->rt) = 0;
    }
    return ok;
}

void qjs_free_str(char *s)
{
    free(s);
}
