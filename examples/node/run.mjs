// Runs a wasip1 module of this repository (qjs.wasm or selftest.wasm) in
// Node's built-in WASI, as wasmtime would:
//
//   node examples/node/run.mjs build/qjs.wasm -e '1 + 2'
//
// The module reads its arguments, writes to stdout and stderr, and reads the
// clocks. The current directory is preopened, so `qjs file.js` can read a file
// under it.
import { readFile } from "node:fs/promises";
import { WASI } from "node:wasi";
import process from "node:process";

const [wasmPath, ...args] = process.argv.slice(2);
if (!wasmPath) {
  console.error("usage: node examples/node/run.mjs module.wasm [args...]");
  process.exit(2);
}

const wasi = new WASI({
  version: "preview1",
  args: [wasmPath, ...args],
  env: process.env,
  preopens: { ".": process.cwd() },
  returnOnExit: true,
});
const module = await WebAssembly.compile(await readFile(wasmPath));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
process.exitCode = wasi.start(instance);
