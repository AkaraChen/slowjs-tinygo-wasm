# slowjs-tinygo-wasm

用 **TinyGo** 把 [liustack/quickjs](https://github.com/liustack/quickjs)（纯 Go、无 cgo 的 QuickJS 引擎）的 JavaScript 引擎编译成 **WebAssembly (wasip1)**，并提供一个最小加载器，在 wasm 里执行 JS 并打印结果。

```console
$ make example
tinygo build -no-debug -target=./targets/wasip1-quickjs.json -o build/qjs.wasm ./cmd/qjs
wasmtime run -W max-wasm-stack=4194304 build/qjs.wasm -e '1 + 2'
3
wasmtime run -W max-wasm-stack=4194304 build/qjs.wasm -e 'function add(a, b) { return a + b } add(40, 2)'
42
node --no-warnings examples/node/run.mjs build/qjs.wasm -e '[1, 2, 3].map(x => x * x)'
[1,4,9]
```

## 先说结论：编进 wasm 的是什么

上游 liustack/quickjs **没法直接用 TinyGo 编译**。它的引擎是 QuickJS 的 C 代码，由 LLVM 编成 **Go assembly**（`internal/cengine/*_amd64.s`、`*_arm64.s`，生成后提交在仓库里），Go 侧 API 通过这些汇编函数调用引擎。

- TinyGo 不支持 Go assembly。原生目标下 TinyGo 会直接忽略 `.s` 文件，于是链接失败：`linker could not find symbol github.com/liustack/quickjs/internal/cengine.JS_ToString` 等。
- 上游也只支持 linux/darwin 的 amd64/arm64 和 windows/amd64。`-target=wasip1` / `-target=wasm`（以及 `GOOS=wasip1 GOARCH=wasm go build`）会被 `internal/unsupported` 故意拦下：`undefined: the_C_engine_runs_only_on_linux_and_darwin_on_amd64_and_arm64_and_on_windows_amd64_this_target_is_not_supported`。

（上面的报错都实测过，见下文「已尝试的方法」。）

所以本仓库编的是**上游同一份引擎源码**：liustack/quickjs 的 `engine/` 目录，即 Bellard 的 QuickJS C 代码加上 liustack 的改动（寄存器 VM、uncatchable limits 等），也就是上游用来生成那些汇编的 C 源码。这份源码由 **TinyGo 自带的 cgo/clang** 直接编成 wasm32，再配一个 TinyGo 写的小 Go API（`quickjs` 包）和一个 C 宿主（`quickjs/host_wasm.c`），代替上游的 Go API 和 Go 宿主。上游的 Go API（`quickjs.VM`、`intl`、`temporal` 等）**没有**移植过来，原因见「做不到的部分」。

## 上游版本与 vendor 内容

| | |
| --- | --- |
| 上游仓库 | https://github.com/liustack/quickjs |
| 固定版本 | tag **v0.30.2**，commit `566d97c97a2fe7db6a3ddd347b742db9b91edeb7` |
| 引擎对应的 QuickJS | 2026-06-04（上游 `docs/upstream.md`：commit `3d5e064e9dd67c70f7962836505a7fa067bf0a4e`） |
| vendor 位置 | `third_party/quickjs-engine/`（来源记录见其中的 `UPSTREAM` 文件） |
| 补丁 | `patches/0001-wasm32-engine.patch` |
| 重新生成 | `make vendor`（即 `scripts/vendor-engine.sh`：clone 该 tag，校验 commit，拷贝文件，打补丁） |

只拷贝了需要的部分：`engine/*.c`、`engine/*.h`、`engine/libc` 的头文件和数学函数（`libc/math.c`、`libc/math/*.c`，来自 musl），以及上游的 LICENSE/NOTICE 文件（`UPSTREAM-*`）。没有带入上游的 git 历史、生成的汇编、Go API 和测试。`heap.c`/`tlsf.c` 也拷了过来，但没有编译（原因见下）。

### 对上游 C 代码的改动（`patches/0001-wasm32-engine.patch`）

上游引擎只在 64 位目标上编译和测试过。wasm32 的指针是 4 字节，需要四处小改动，每处都带有 `slowjs-tinygo-wasm:` 注释：

1. **`quickjs.h`**：在 32 位目标上，QuickJS 默认用 NaN-boxing，`JSValue` 是 8 字节的 `uint64_t`。liustack 的寄存器 VM 要求 `sizeof(JSValue) == 16`（`regvm.h` 里有 static assert），并且直接访问 `.u` / `.tag`。补丁新增开关 `JS_NO_NAN_BOXING`：打开后，32 位上也使用 16 字节的 `struct JSValue`。
2. **`regvm.h`**：`RVHeader` 必须正好占 3 个 slot（48 字节）。wasm32 上 5 个指针只占 20 字节，所以补上 padding。
3. **`heap.h`**：`JS_HEAP_MAX_REQUEST` 原为 `((size_t)1 << 32) - 8`，这在 32 位 `size_t` 上是越界移位，改为 `0x7ffffff8`。
4. **`quickjs.c`**：`JS_SetDirectCCalls(ctx, 0)` 里的 `(long)1 << 60` 溢出 32 位 `long`，改为 `LONG_MAX >> 1` 的等价写法。

此外，构建配置（`quickjs/engine_config.h`）还做了这些选择：

- **`JS_HEAP_SYSTEM_MALLOC`**：上游的堆（`heap.c` + TLSF）向宿主申请 64 KiB chunk，代码里有 `the heap is for 64 bit targets` 的 static assert。所以改用上游给 sanitizer 构建准备的另一种实现方式：每个块都从系统 malloc 来。`quickjs/heap_wasm.c` 是上游 `engine/test/heap-system.c` 针对本宿主的改写。TinyGo 的 malloc 没有 `malloc_usable_size`，所以块头里自己记录大小；计数方式与上游相同，`SetMemoryLimit` 照常生效。
- **`JS_HOST_POLL`**：上游的生成器会在汇编里插入中断轮询，C 构建没有，所以引擎每次读中断标志都会调用 `host_poll`（上游 native 测试宿主同样如此）。超时就在这里实现。
- 数学函数用上游 `engine/libc` 自带的 musl libm（`quickjs/internal/libm`），使 `Math.*` 的结果与上游一致。TinyGo 带的 wasi-libc 缺少部分 libm 符号（实测 `ceil fabs floor fmin sqrt trunc` 链接时未定义）。
- 其余 libc 函数（`memcpy`、`snprintf` 等）用 TinyGo 带的 wasi-libc；上游自己的 string/stdio 实现没有编译。

## 依赖工具（实测版本）

| 工具 | 版本 | 说明 |
| --- | --- | --- |
| TinyGo | **0.44.0**（LLVM 22.1.4） | `tinygo0.44.0.linux-amd64.tar.gz` |
| Go | **1.26.11**（另用 1.27.4 编过并跑通 selftest） | TinyGo 0.44 要求 Go 1.25 到 1.27；1.24 会报 `requires go version 1.25 through 1.27` |
| wasmtime | **50.0.1** | 运行 wasip1 模块 |
| Node.js | **24.20.0** | 可选，用内置的 `node:wasi` 运行 |
| 主机 | Linux 7.0 x86_64（AMD EPYC 9645） | |

构建不需要单独的 C 编译器：TinyGo 自带 clang 和 wasi-libc。

## 构建与运行

```sh
make build          # build/qjs.wasm 和 build/selftest.wasm
make test           # selftest 在 wasmtime 和 node 下各跑一遍
make example        # 上面开头那段示例
```

不用 make 时，等价的命令是：

```sh
export CGO_CFLAGS_ALLOW='-fwrapv|-fno-math-errno|-ffp-contract=off'
tinygo build -no-debug -target=./targets/wasip1-quickjs.json -o build/qjs.wasm ./cmd/qjs

wasmtime run -W max-wasm-stack=4194304 build/qjs.wasm -e '1 + 2'           # 3
echo '6 * 7' | wasmtime run build/qjs.wasm                                   # 从 stdin 读：42
wasmtime run --dir . build/qjs.wasm script.js                                # 读文件（预打开当前目录）
wasmtime run build/qjs.wasm -timeout 1s -e 'for (;;) {}'                     # InternalError: interrupted (eval timeout)
node --no-warnings examples/node/run.mjs build/qjs.wasm -e '1 + 2'           # 用 Node 的 WASI 运行
```

`qjs` 的参数：`-e code`、`file.js`（或从 stdin 读）、`-timeout 2s`、`-depth N`（最大调用深度，默认 1000）、`-q`（不打印结果）。结果的输出方式：数组和普通对象打印成 JSON，其他值打印 `String(v)`。异常打印到 stderr，带 stack，退出码 1。脚本里可以调用 `print(...)` / `console.log(...)`，这两个函数通过 C → Go 回调输出。设置环境变量 `QJS_TIME=1` 会额外打印 eval 耗时。

### 为什么选 wasip1 target

- **wasip1**：自带 stdout、参数和时钟，wasmtime 和 Node 都能直接运行，不需要胶水代码。引擎需要的东西（时钟、输出）都能通过 WASI import 拿到，C 宿主直接调用 `clock_time_get`。
- `-target=wasm`（GOOS=js）需要 `wasm_exec.js` 和浏览器/Node 的 JS 胶水，跑最小示例不需要它。没有测过。
- `targets/wasip1-quickjs.json` 继承 wasip1，只改了两项：
  - `"scheduler": "none"`：引擎用剩余栈字节数来限制递归（`__shadow_remaining`）。在 wasm 里，取了地址的 C 局部变量放在线性内存的影子栈上，C 宿主从 `__stack_low`/`__stack_high` 计算剩余空间。这个算法只在主栈上成立：asyncify 调度器的 goroutine 有各自独立的栈，C 无从得知它的边界，而且 asyncify 会对整个引擎做插桩。
  - `-zstack-size=4194304`：wasm-ld 默认的栈只有 64 KiB，而引擎每次调用要预留 64 KiB（`JS_STACK_MARGIN`），默认栈下连 `JS_NewContext` 都会失败（实测）。

### Go API（`quickjs` 包）

```go
vm, err := quickjs.NewVM()            // 一个 runtime + 一个 context，带 print/console.log
defer vm.Close()
vm.SetEvalTimeout(time.Second)        // 超时：脚本里的 catch 拦不住
vm.SetMemoryLimit(64 << 20)
vm.SetMaxCallDepth(1000)
vm.SetPrint(func(line string) { ... })
out, err := vm.Eval(`1 + 2`)          // "3"；JS 异常 → *quickjs.Error
```

`Eval` 执行完脚本后，会把 promise job 也执行完。这个包只能用 `tinygo build -target=./targets/wasip1-quickjs.json` 编译。其他构建方式（普通 `go build`、其他 target）会因为 `quickjs/unsupported.go` 故意编译失败。

## 测试（实际运行结果）

`go test` / `tinygo test` 跑不了：TinyGo 的 `testing` 包要启动 goroutine，而这里的构建没有调度器（`attempted to start a goroutine without a scheduler`）。所以测试写成了一个 wasm 程序 `cmd/selftest`，由 `make test` 分别在 wasmtime 和 node 下运行：

```console
$ make test
wasmtime run -W max-wasm-stack=4194304 build/selftest.wasm
ok   Eval (11ms)
ok   GlobalsPersist (1ms)
ok   PrintAndJobs (0s)
ok   Exception (0s)
ok   StackOverflow (1ms)
ok   Timeout (201ms)
ok   MemoryLimit (29ms)
ok   SeveralVMs (1ms)
PASS
node --no-warnings examples/node/run.mjs build/selftest.wasm
ok   Eval (52ms)
...
PASS
```

`Eval` 这一项覆盖了：算术、函数调用、字符串、数组/对象（JSON 输出）、浮点打印、`toFixed`、`Math.sin`/`pow`、BigInt、正则、私有字段、生成器、递归、`Date`、Unicode、TypedArray，以及 1 万个属性的对象。另外几项分别测：全局变量在多次 Eval 之间保留、`console.log` 回调到 Go、Promise job、异常文本和 stack、栈溢出抛 `RangeError`、超时（`try/catch` 也拦不住）、内存上限、多个 VM 互相隔离。

此外还手动跑过一批脚本，结果都正确，包括 class 继承、`async`/`await`、`Map`/`Set`、`2n ** 100n`、`for` 循环 1e6 次、`fib(25)`、`null.x` 的错误信息。

## 性能（粗测，仅供参考）

同一台机器，每项取多次运行中的典型值。native 指上游 v0.30.2 原样用 Go 1.26.11 在 linux/amd64 上跑（Go assembly 引擎）：

| 脚本 | native liustack/quickjs | TinyGo wasm @ wasmtime 50 | TinyGo wasm @ Node 24 |
| --- | --: | --: | --: |
| `fib(30)` | 42 ms | 77 ms | 135 ms |
| `for` 1e7 次累加 | 99 ms | 253 ms | 425 ms |
| 20 万个属性的对象 + `Object.keys` | 45 ms | 216–260 ms | 260 ms |

慢的原因：wasm 本身的开销，wasm 里 computed goto 只能编成 `br_table`，以及每个块都走 TinyGo 的 malloc（TinyGo 的 GC 堆加一个 map 记录分配），分配密集的脚本因此慢得最多。没有做任何调优（比如把上游 TLSF 堆移植到 32 位）。

## 二进制大小

| | 大小 |
| --- | --: |
| `build/qjs.wasm`（`-no-debug`，Makefile 默认） | 1,537,359 字节（1.5 MB），gzip -9 后 574 KB |
| 带 DWARF（去掉 `-no-debug`） | 6.8 MB |

完整构建（冷缓存）约 40–50 秒，大部分时间花在编译 `quickjs.c`（7 万行）上。

## 做不到 / 没做的部分

- **上游的 Go API 不能用**：`github.com/liustack/quickjs` 这个包本身（`VM`、`Value`、`Call`、`NewBuiltin`、`EvalSharedBytecode`、`intl`、`temporal`、`fmt` 等）在 TinyGo 下编不了，原因是：
  - 引擎调用全部经过生成的 Go assembly（amd64/arm64），TinyGo 不支持 Go asm，也没有 wasm 版本的汇编。
  - 上游用 build tag 显式拒绝其他平台，并且依赖 gc 编译器的函数序言和 fault 处理方式（README 原文：*It leans on how Go starts a function and handles a fault*）。

  本仓库的 `quickjs` 包是一个很小的替代 API：只有 Eval、print、超时、内存上限和调用深度。
- **没有 `Intl` / `Temporal`**：上游的实现在 Go 侧（`internal/gointl`，通过 `host_call` 的 magic 编号调用），没有移植。`toLocaleString` 等函数走引擎的默认行为。
- **没有 Go 写的 Builtin / 宿主函数注册 API**：只有内置的 `print`/`console.log`。机制已经打通（`JS_NewHostFunction` → `host_call` → `//export` 的 Go 函数），要加更多函数并不难，但不在最小版本的范围内。
- **只能 `-scheduler=none`**：不能使用 goroutine；`flag` 包，以及经过 `io.Writer` 接口的动态调用，会让 TinyGo 链进 `time.AfterFunc` 的 goroutine 而编译失败，所以 CLI 自己解析参数，`SetPrint` 用 func 而不是 `io.Writer`。一个模块最多同时开 64 个 VM。
- **wasm 的原生调用栈**：C 只能测量线性内存里的影子栈，测不到 wasm 引擎自己的调用栈。普通 JS 递归问题不大（实测 `-depth 10000` 时递归 9990 层，在 wasmtime 和 node 下都正常）。但经过原生内建函数的深递归（例如 Proxy `get` 套娃），在 wasmtime 默认 512 KiB 的 wasm 栈下，会在引擎抛 RangeError 之前就触发 trap：`wasm trap: call stack exhausted`（实测：Proxy 递归约 500 层）。加上 `-W max-wasm-stack=4194304`（Makefile 已默认加上）后，同一脚本会正常抛 RangeError。所以默认调用深度设为 1000（上游是 5000）。
- **中断粒度**：超时通过 `host_poll` 每 1024 次读标志才看一次时钟，实测 200 ms 超时约 201 ms 返回。超时后 VM 可以继续使用。大块内存 realloc 期间不能被中断（上游 `heap-system.c` 也有同样的限制）。
- **数值细节**：除了 `ceil fabs floor fmin sqrt trunc` 这类精确函数之外，都使用上游 musl libm，不过没有拿上游的 `engine/test/math.sh` 记录值做比对。也没有在 wasm 上跑上游的 test262 和 C 测试，只跑了 selftest 和上文的手测脚本。没有逐个审查 32 位下所有的 `-Wshorten-64-to-32` 警告（编译时有几十条，多数是 QuickJS 本身就有的）。
- **TinyGo 的几个坑**（都已绕过）：
  - `#cgo CFLAGS` 有白名单，`-fwrapv`（引擎依赖有符号溢出回绕）、`-fno-math-errno`、`-ffp-contract=off` 需要通过 `CGO_CFLAGS_ALLOW` 放行；`-ffreestanding`、`-nostdinc` 无法使用，所以靠 `-I` 让引擎的 libc 头文件优先。
  - TinyGo 0.44 把 `${SRCDIR}/..` 解析成 `${SRCDIR}` 本身（少退一级），所以 `-I` 用相对路径。
- **没有部署任何东西**，也没有改动上游仓库。

## 已尝试的方法（与结果）

1. 直接依赖上游模块，`tinygo build -target=wasip1` / `-target=wasm`：被 `internal/unsupported` 拦下，编译失败。
2. 用同样的代码 `tinygo build`（native linux/amd64）：编译能通过，但 `.s` 被忽略，链接报 `linker could not find symbol .../internal/cengine.JS_*`。
3. 用 gc 编译器 `GOOS=wasip1 GOARCH=wasm go build`：同样被 `internal/unsupported` 拦下。
4. 改为直接编译 `engine/` 的 C 源码，用 `tinygo clang --target=wasm32-unknown-wasip1`：遇到 `sizeof(JSValue) == 16`、`RVHeader` 大小、`the heap is for 64 bit targets` 三处 static assert 失败，由上面的补丁和 `JS_HEAP_SYSTEM_MALLOC` 解决。
5. 用 TinyGo 默认 wasip1 target（asyncify，64 KiB 栈）：`JS_NewContext` 失败（栈不够 64 KiB 的余量），改用自定义 target 后解决。
6. 默认 `-opt=z` 与 `-opt=2` 的速度在误差范围内，保留默认值。

## 目录

```
cmd/qjs/              最小加载器（CLI）
cmd/selftest/         在 wasm 里运行的检查
quickjs/              Go API + C 宿主（host_wasm.c、heap_wasm.c）+ 引擎编译单元
quickjs/internal/libm 引擎 libc 的 musl 数学函数（由脚本生成）
third_party/quickjs-engine/  上游 engine/（v0.30.2，已打补丁）
patches/              对上游 C 的补丁
targets/              TinyGo target（wasip1 + scheduler=none + 4 MiB 栈）
examples/node/        用 Node 的 WASI 运行
scripts/              vendor 脚本
```

## License

本仓库新写的代码（`quickjs/`、`cmd/`、`scripts/`、`examples/`、`targets/`、补丁）采用 MIT 许可证（见 `LICENSE`），与上游 liustack/quickjs、QuickJS、musl 一致。`third_party/quickjs-engine/` 保持上游各自的许可证：QuickJS（MIT，`LICENSE`）、liustack/quickjs（MIT，`UPSTREAM-LICENSE-MIT`）、musl（MIT，`libc/COPYRIGHT`）、TLSF 等，详见 `third_party/quickjs-engine/UPSTREAM-NOTICE.md`。
