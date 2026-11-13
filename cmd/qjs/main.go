// Command qjs is the minimal loader: it runs JavaScript in the liustack/quickjs
// engine inside a wasip1 module built by TinyGo, and prints the result.
//
//	qjs -e 'code'      evaluate code
//	qjs file.js        evaluate a file (the runtime must preopen its directory)
//	qjs                evaluate standard input
//
// Options: -timeout 2s stops a script that runs longer, -depth N sets the
// most nested JavaScript calls (default 1000), -q does not print the
// completion value. (The flag package is not used: under -scheduler=none it
// pulls in time.AfterFunc, which starts a goroutine.)
//
// Build:  tinygo build -target=wasip1 -scheduler=none -o qjs.wasm ./cmd/qjs
// Run:    wasmtime run qjs.wasm -e '1 + 2'
package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"time"

	"github.com/AkaraChen/slowjs-tinygo-wasm/quickjs"
)

func main() {
	var (
		expr    string
		timeout time.Duration
		quiet   bool
		depth   int
		rest    []string
	)
	args := os.Args[1:]
	for i := 0; i < len(args); i++ {
		switch a := args[i]; {
		case (a == "-e" || a == "-timeout" || a == "-depth") && i+1 < len(args):
			i++
			if a == "-e" {
				expr = args[i]
				break
			}
			if a == "-depth" {
				var err error
				if depth, err = strconv.Atoi(args[i]); err != nil {
					fmt.Fprintln(os.Stderr, "qjs: -depth:", err)
					os.Exit(2)
				}
				break
			}
			d, err := time.ParseDuration(args[i])
			if err != nil {
				fmt.Fprintln(os.Stderr, "qjs:", err)
				os.Exit(2)
			}
			timeout = d
		case a == "-q":
			quiet = true
		case a == "-h" || a == "-help" || a == "--help":
			fmt.Fprintln(os.Stderr, "usage: qjs [-timeout 2s] [-depth 1000] [-q] (-e code | file.js | < stdin)")
			os.Exit(2)
		default:
			rest = append(rest, a)
		}
	}

	src, name, err := source(expr, rest)
	if err != nil {
		fmt.Fprintln(os.Stderr, "qjs:", err)
		os.Exit(2)
	}

	vm, err := quickjs.NewVM()
	if err != nil {
		fmt.Fprintln(os.Stderr, "qjs:", err)
		os.Exit(1)
	}
	defer vm.Close()
	vm.SetEvalTimeout(timeout)
	if depth > 0 {
		if err := vm.SetMaxCallDepth(depth); err != nil {
			fmt.Fprintln(os.Stderr, "qjs:", err)
			os.Exit(2)
		}
	}

	start := time.Now()
	out, err := vm.EvalFile(src, name)
	if os.Getenv("QJS_TIME") != "" {
		fmt.Fprintf(os.Stderr, "qjs: eval took %v\n", time.Since(start))
	}
	var jsErr *quickjs.Error
	if errors.As(err, &jsErr) {
		fmt.Fprintln(os.Stderr, jsErr.Message)
		os.Exit(1)
	} else if err != nil {
		fmt.Fprintln(os.Stderr, "qjs:", err)
		os.Exit(1)
	}
	if !quiet {
		fmt.Println(out)
	}
}

func source(expr string, args []string) (src, name string, err error) {
	switch {
	case expr != "":
		return expr, "<cmdline>", nil
	case len(args) > 0:
		b, err := os.ReadFile(args[0])
		return string(b), args[0], err
	default:
		b, err := io.ReadAll(os.Stdin)
		return string(b), "<stdin>", err
	}
}
