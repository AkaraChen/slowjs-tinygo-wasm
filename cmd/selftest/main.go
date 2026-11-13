// Command selftest checks the quickjs package inside the wasm module, under
// wasmtime or node: go test and tinygo test cannot run it, since TinyGo's
// testing package starts goroutines and this build has no scheduler
// (-scheduler=none, see quickjs/host_wasm.c). It exits with 1 when a check
// fails.
//
//	make test
package main

import (
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	. "github.com/AkaraChen/slowjs-tinygo-wasm/quickjs"
)

// T is the little of testing.T that the checks use.
type T struct {
	failed  bool
	cleanup []func()
}

type fatal struct{}

func (t *T) Errorf(format string, args ...any) {
	t.failed = true
	fmt.Printf("    "+format+"\n", args...)
}

func (t *T) Fatalf(format string, args ...any) {
	t.Errorf(format, args...)
	panic(fatal{})
}

func (t *T) Fatal(err error) { t.Fatalf("%v", err) }

func newVM(t *T) *VM {
	vm, err := NewVM()
	if err != nil {
		t.Fatal(err)
	}
	t.cleanup = append(t.cleanup, vm.Close)
	return vm
}

func run(name string, f func(*T)) (ok bool) {
	t := &T{}
	start := time.Now()
	defer func() {
		if r := recover(); r != nil {
			if _, isFatal := r.(fatal); !isFatal {
				panic(r)
			}
		}
		for _, c := range t.cleanup {
			c()
		}
		status := "ok  "
		if t.failed {
			status = "FAIL"
		}
		fmt.Printf("%s %s (%v)\n", status, name, time.Since(start).Round(time.Millisecond))
		ok = !t.failed
	}()
	f(t)
	return
}

func main() {
	failed := 0
	for _, c := range []struct {
		name string
		f    func(*T)
	}{
		{"Eval", TestEval},
		{"GlobalsPersist", TestGlobalsPersist},
		{"PrintAndJobs", TestPrintAndJobs},
		{"Exception", TestException},
		{"StackOverflow", TestStackOverflow},
		{"Timeout", TestTimeout},
		{"MemoryLimit", TestMemoryLimit},
		{"SeveralVMs", TestSeveralVMs},
	} {
		if !run(c.name, c.f) {
			failed++
		}
	}
	if failed > 0 {
		fmt.Printf("FAIL: %d checks failed\n", failed)
		os.Exit(1)
	}
	fmt.Println("PASS")
}

func TestEval(t *T) {
	vm := newVM(t)
	for _, c := range []struct{ src, want string }{
		{`1+2`, `3`},
		{`function add(a, b) { return a + b } add(40, 2)`, `42`},
		{`"a" + "b".repeat(3)`, `abbb`},
		{`[1, 2, 3].map(x => x * 2)`, `[2,4,6]`},
		{`({a: 1, b: [true, null, "s"]})`, `{"a":1,"b":[true,null,"s"]}`},
		{`0.1 + 0.2`, `0.30000000000000004`},
		{`(123.456).toFixed(2)`, `123.46`},
		{`Math.sin(1)`, `0.8414709848078965`},
		{`Math.pow(2, 0.5)`, `1.4142135623730951`},
		{`2n ** 100n`, `1267650600228229401496703205376`},
		{`"abc".replace(/b/g, "X") + /(\d+)-(\d+)/.exec("10-20")[2]`, `aXc20`},
		{`class A { #x = 1; get x() { return this.#x } }; new A().x`, `1`},
		{`function* g() { yield 1; yield 2 } [...g()]`, `[1,2]`},
		{`function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2) } fib(20)`, `6765`},
		{`new Date(0).toISOString()`, `1970-01-01T00:00:00.000Z`},
		{`"中文".length + " " + "😀".codePointAt(0)`, `2 128512`},
		{`new Uint8Array([1, 2, 300]).join()`, `1,2,44`},
		{`let o = {}; for (let i = 0; i < 10000; i++) o["k" + i] = i; Object.keys(o).length`, `10000`},
		{`undefined`, `undefined`},
	} {
		got, err := vm.Eval(c.src)
		if err != nil {
			t.Errorf("%s: %v", c.src, err)
			continue
		}
		if got != c.want {
			t.Errorf("%s = %q, want %q", c.src, got, c.want)
		}
	}
}

func TestGlobalsPersist(t *T) {
	vm := newVM(t)
	if _, err := vm.Eval(`var counter = 10; function inc() { return ++counter }`); err != nil {
		t.Fatal(err)
	}
	vm.Eval(`inc()`)
	if got, _ := vm.Eval(`inc()`); got != "12" {
		t.Fatalf("got %q, want 12", got)
	}
}

func TestPrintAndJobs(t *T) {
	vm := newVM(t)
	var lines []string
	vm.SetPrint(func(line string) { lines = append(lines, line) })
	got, err := vm.Eval(`console.log("hello", 1, true); Promise.resolve(5).then(v => print("then", v * 2)); "sync"`)
	if err != nil || got != "sync" {
		t.Fatalf("got %q, %v", got, err)
	}
	if strings.Join(lines, "|") != "hello 1 true|then 10" {
		t.Fatalf("printed %q", lines)
	}
}

func TestException(t *T) {
	vm := newVM(t)
	_, err := vm.Eval(`throw new TypeError("boom")`)
	var e *Error
	if !errors.As(err, &e) || !strings.HasPrefix(e.Message, "TypeError: boom\n") {
		t.Fatalf("got %v", err)
	}
	_, err = vm.Eval(`null.x`)
	if err == nil || !strings.Contains(err.Error(), "cannot read property 'x' of null") {
		t.Fatalf("got %v", err)
	}
	// the VM is still good
	if got, _ := vm.Eval(`1+1`); got != "2" {
		t.Fatalf("got %q", got)
	}
}

func TestStackOverflow(t *T) {
	vm := newVM(t)
	_, err := vm.Eval(`function f(n) { return f(n + 1) } f(0)`)
	if err == nil || !strings.HasPrefix(err.Error(), "RangeError: Maximum call stack size exceeded") {
		t.Fatalf("got %v", err)
	}
}

func TestTimeout(t *T) {
	vm := newVM(t)
	vm.SetEvalTimeout(200 * time.Millisecond)
	start := time.Now()
	_, err := vm.Eval(`try { for (;;) {} } catch (e) { "caught" }`)
	if err == nil || !strings.Contains(err.Error(), "interrupted") {
		t.Fatalf("got %v", err)
	}
	if d := time.Since(start); d > 5*time.Second {
		t.Fatalf("took %v", d)
	}
	vm.SetEvalTimeout(0)
	if got, _ := vm.Eval(`"after"`); got != "after" {
		t.Fatalf("got %q", got)
	}
}

func TestMemoryLimit(t *T) {
	vm := newVM(t)
	vm.SetMemoryLimit(8 << 20)
	_, err := vm.Eval(`let a = []; for (;;) a.push(new Array(1000).fill(1))`)
	if err == nil || !strings.Contains(err.Error(), "out of memory") {
		t.Fatalf("got %v", err)
	}
}

func TestSeveralVMs(t *T) {
	a, b := newVM(t), newVM(t)
	a.Eval(`var x = "a"`)
	b.Eval(`var x = "b"`)
	ga, _ := a.Eval(`x`)
	gb, _ := b.Eval(`x`)
	if ga != "a" || gb != "b" {
		t.Fatalf("got %q %q", ga, gb)
	}
}
