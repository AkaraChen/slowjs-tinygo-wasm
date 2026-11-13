//go:build !(tinygo && wasip1 && scheduler.none)

package quickjs

// This package builds only with TinyGo for wasip1 with -scheduler=none:
//
//	tinygo build -target=./targets/wasip1-quickjs.json ./cmd/qjs
//
// (see README.md). Another build stops here, on purpose.
var _ = build_with_tinygo_target_targets_wasip1_quickjs_json
