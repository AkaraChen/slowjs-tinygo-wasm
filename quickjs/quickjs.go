//go:build tinygo && wasip1 && scheduler.none

// Package quickjs runs JavaScript in the C engine of liustack/quickjs, built
// by TinyGo's cgo for wasip1.
//
// The engine is the C of github.com/liustack/quickjs v0.30.2 (engine/), with
// the patches of this repository for wasm32 (third_party/quickjs-engine and
// patches/). Upstream's Go API is not used: it calls the engine through Go
// assembly generated for amd64 and arm64, which TinyGo cannot build. This
// package is a small API of its own over the C API of the engine (host_wasm.c).
//
// Build with: tinygo build -target=wasip1 -scheduler=none
package quickjs

/*
// relative -I paths: TinyGo 0.44 resolves ${SRCDIR}/.. to ${SRCDIR} itself
#cgo CFLAGS: -I../third_party/quickjs-engine/libc -I../third_party/quickjs-engine
#cgo CFLAGS: -fno-stack-protector -funsigned-char -fwrapv -fno-math-errno -ffp-contract=off
#cgo CFLAGS: -O2 -Wno-array-bounds -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
#include "bridge.h"
*/
import "C"

import (
	"errors"
	"os"
	"time"

	// the math functions of the engine's libc (TinyGo's wasi-libc has no libm)
	_ "github.com/AkaraChen/slowjs-tinygo-wasm/quickjs/internal/libm"
)

// DefaultMaxCallDepth bounds the recursion of JavaScript (JS_SetMaxCallDepth).
// Upstream's default is 5000, which the native stack of a wasm runtime such as
// wasmtime (512 KiB by default) does not always hold: each level of JS calls
// also takes frames of the wasm engine's own stack, which C cannot measure.
const DefaultMaxCallDepth = 1000

// VM is one JavaScript runtime with one context. It is not safe for several
// goroutines (this build has none: -scheduler=none).
type VM struct {
	p       *C.qjs_vm
	id      uint32
	print   func(line string)
	timeout time.Duration
}

// Error is a JavaScript exception: its text, with the stack when it has one.
type Error struct {
	Message string
}

func (e *Error) Error() string { return e.Message }

var vms = map[uint32]*VM{}

// NewVM makes a VM with print and console.log, which write a line to
// os.Stdout until SetPrint says otherwise.
func NewVM() (*VM, error) {
	p := C.qjs_new(C.uint64_t(uint64(time.Now().UnixNano())), C.int(DefaultMaxCallDepth))
	if p == nil {
		return nil, errors.New("quickjs: cannot make a VM: " + C.GoString(C.qjs_new_error()))
	}
	vm := &VM{p: p, id: uint32(C.qjs_id(p)), print: printStdout}
	vms[vm.id] = vm
	return vm, nil
}

// Close frees the VM. It is a no-op the second time.
func (vm *VM) Close() {
	if vm.p == nil {
		return
	}
	C.qjs_free(vm.p)
	delete(vms, vm.id)
	vm.p = nil
}

// SetPrint sets what print and console.log call, with their arguments joined
// by spaces (nil drops them). It is a func rather than an io.Writer on
// purpose: with -scheduler=none, a call through io.Writer makes TinyGo link
// every Write method of the program, and one of them starts a goroutine,
// which fails the build.
func (vm *VM) SetPrint(f func(line string)) { vm.print = f }

func printStdout(line string) {
	os.Stdout.Write([]byte(line + "\n"))
}

// SetMemoryLimit limits the memory of the VM to n bytes (0 for none).
func (vm *VM) SetMemoryLimit(n uint32) {
	limit := C.size_t(n)
	if n == 0 {
		limit = ^C.size_t(0)
	}
	C.qjs_set_memory_limit(vm.p, limit)
}

// SetMaxCallDepth sets how deep JavaScript may recurse (JS_SetMaxCallDepth,
// upstream allows 1 to 10000). Past it a call throws a RangeError.
func (vm *VM) SetMaxCallDepth(depth int) error {
	if C.qjs_set_max_call_depth(vm.p, C.int(depth)) != 0 {
		return errors.New("quickjs: call depth out of range")
	}
	return nil
}

// SetEvalTimeout stops an Eval that runs longer than d (0 for none).
func (vm *VM) SetEvalTimeout(d time.Duration) { vm.timeout = d }

// Eval runs src as a global script, then the jobs it queued (promise
// reactions), and returns its completion value as text: JSON for arrays and
// plain objects, String(value) otherwise. A thrown exception comes back as an
// *Error.
func (vm *VM) Eval(src string) (string, error) {
	return vm.EvalFile(src, "<eval>")
}

// EvalFile is Eval with the file name that errors and stacks show.
func (vm *VM) EvalFile(src, filename string) (string, error) {
	if vm.p == nil {
		return "", errors.New("quickjs: VM is closed")
	}
	// the engine wants a NUL after the source
	csrc := C.CString(src)
	defer C.qjs_free_str(csrc)
	cname := C.CString(filename)
	defer C.qjs_free_str(cname)

	var out *C.char
	var n C.size_t
	ok := C.qjs_eval(vm.p, csrc, C.size_t(len(src)), cname, C.int64_t(vm.timeout.Nanoseconds()), &out, &n)
	text := ""
	if out != nil {
		text = C.GoStringN(out, C.int(n))
		C.qjs_free_str(out)
	}
	if ok == 0 {
		// a stack ends with a newline
		for len(text) > 0 && text[len(text)-1] == '\n' {
			text = text[:len(text)-1]
		}
		return "", &Error{Message: text}
	}
	return text, nil
}

//export qjsGoPrint
func qjsGoPrint(id C.uint32_t, s *C.char, n C.size_t) {
	vm := vms[uint32(id)]
	if vm == nil || vm.print == nil {
		return
	}
	vm.print(C.GoStringN(s, C.int(n)))
}
