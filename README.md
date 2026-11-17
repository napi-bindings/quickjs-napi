# quickjs-napi

[![CI](https://github.com/napi-bindings/quickjs-napi/actions/workflows/ci.yml/badge.svg?branch=napi)](https://github.com/napi-bindings/quickjs-napi/actions/workflows/ci.yml)

Node-API implementation for QuickJS (2026-06-04, from `../quickjs-build`).
Addons built for Node.js (node-gyp, node-addon-api, napi-rs) are loaded
unchanged: they resolve `napi_*` from the `quickjs-napi` executable.

Status: milestone **M3** (M1 values, strings, objects, properties,
functions, classes, errors; M2 references, externals, wrapping, finalizers,
type tags, instance data, `napi_run_script`; M3 ArrayBuffer, TypedArray,
DataView, BigInt, Date, Promise). 129 of 155 Node-API functions are
implemented and pass 30 tests of the Node.js `js-native-api` suite (see
`test/node/README.md`); the others exist as generated stubs that return
`napi_generic_failure` with a "not implemented yet" message, so that every
addon still loads. `NAPI_EXPERIMENTAL` APIs are not provided. See
`PIANO-IMPLEMENTAZIONE.md` in the project for the plan.

## Lifetime model

- `napi_ref` follows `v8impl::Reference`: strong while its count is > 0; at
  count 0 an object, function or local symbol is held through a JS `WeakRef`
  (QuickJS reports the target dead as soon as its refcount drops to zero),
  other values are released.
- QuickJS has no weak callback in its C API. Each target of a reference, a
  wrap or a type tag gets a *holder* object in a `WeakMap` keyed by the
  target; QuickJS drops the entry of a dead key during `JS_RunGC`, and the
  holder's class finalizer plays the role of V8's weak callback. Wrapping
  therefore works on any object (frozen objects, proxies, functions) without
  adding properties.
- Finalizers never run inside the GC: as in Node.js they are queued and run
  from the event loop (`qjsnapi_host_run_finalizers`), or at teardown with
  JavaScript disabled (`napi_cannot_run_js`). `global.gc()` runs two GC
  passes, because QuickJS collects cycles after dropping weak entries.

## Binary data, BigInt, Date, Promise

- Builtin classes are recognised by class ID, found at startup by probing
  one instance of each (QuickJS does not export its class enum). DataView
  and TypedArray accessors, `Date.prototype.getTime` and the constructors
  are captured before any user code runs, so monkey-patching the globals
  does not affect Node-API.
- Semantics follow Node.js 22.23 / V8, checked against Node.js by
  `test/js/test_binary.js`: `napi_get_arraybuffer_info` and
  `napi_create_typedarray` accept a SharedArrayBuffer; an empty or detached
  buffer has `data == NULL`; `napi_is_detached_arraybuffer` is V8's
  `WasDetached()`; an external ArrayBuffer over `NULL` is detached; a
  detached or out-of-bounds view reports length 0; length-tracking views of
  resizable buffers report their current length; alignment, length and
  DataView range errors throw the same `RangeError`s with the same codes.
- External ArrayBuffers use the memory in place; the finalizer is queued
  when the buffer is collected, as Node.js defers it.
- `napi_*_bigint_words` go through a hexadecimal string (O(n)); 0n has no
  words and sign 0.
- Deviations: `napi_float16_array` is supported (QuickJS has
  `Float16Array`; Node.js 22 returns `napi_invalid_arg`); a BigInt is
  limited to 2^20 bits (16384 words; V8: 2^30 bits), beyond which
  `napi_create_bigint_words` throws `RangeError: Maximum BigInt size
  exceeded`, as V8 does past its own limit; for an out-of-bounds,
  non-detached view of a resizable buffer the byte offset is reported as 0
  (V8 reports the stored offset), since QuickJS does not expose it.

## Build

`quickjs-build` must be checked out next to this repository
(`../quickjs-build`, branch `feat/quickjs-2026-06-04`). The presets in
`CMakePresets.json` are the reference configurations, used by CI too; each
builds in `build/<preset>`:

| Preset | |
|---|---|
| `unix-debug` | Linux/macOS, Debug (QuickJS leak assertion at `JS_FreeRuntime`) |
| `unix-release` | Linux/macOS, Release |
| `unix-asan` | Linux/macOS, Debug + ASan/UBSan, QuickJS included |
| `windows-zig-debug`, `windows-zig-release` | Windows x64 with `zig cc` (see below) |

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --preset unix-debug
```

With Clang (`zig cc` included), Debug builds compile the QuickJS library
itself with `-O1`: at `-O0` its interpreter loop needs ~64 KiB of C stack per
JS call and QuickJS's 1 MiB stack limit would stop at ~16 nested calls. Our
own code keeps the Debug flags, and QuickJS keeps its assertions.

Without presets:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

CI (`.github/workflows/ci.yml`) runs gcc Debug/Release, clang, gcc with
ASan/UBSan and macOS arm64 with the `unix-*` presets, and Windows x64 with
the `windows-zig-*` presets; the Linux and macOS jobs also run the
`node_parity_*` tests under Node.js 22.

Options:

| Option | Default | |
|---|---|---|
| `QJSNAPI_QUICKJS_DIR` | `../quickjs-build` | QuickJS sources, built as a subproject |
| `QJSNAPI_QUICKJS_LIBRARY` | empty | prebuilt `libqjs.a` when the sources are not available |
| `QJSNAPI_SANITIZE` | OFF | ASan + UBSan for everything, QuickJS included |
| `QJSNAPI_BUILD_TESTS` | ON | test addons and CTest suite |
| `QJSNAPI_LIBUV` | OFF | event loop (milestone M5, not available yet) |

`include/quickjs.h` and `include/quickjs-libc.h` must be identical to the
ones in `quickjs-build`: configuration fails otherwise (JSValue ABI).

### Windows

QuickJS does not support Microsoft Visual C++: the Visual Studio generators
(and clang-cl) are rejected at configure time. Use a MinGW-w64 toolchain with
Ninja, as quickjs-build does:

- **zig** (version in `../quickjs-build/.zigversion`, e.g. `winget install zig.zig`;
  `zig` must be in `PATH`, or set the `ZIG` environment variable). Use the
  presets in `CMakePresets.json`, which pass the toolchain file:

  ```sh
  cmake --preset windows-zig-debug
  cmake --build --preset windows-zig-debug
  ctest --preset windows-zig-debug
  ```

  In VS Code (CMake Tools) select the configure preset *Windows x64 - zig -
  Debug*. Do not use a "zig" compiler **kit**: a kit sets `CMAKE_AR=zig;ar`
  without the toolchain file, and Ninja then runs the literal command `zig;ar`
  (`error: unknown command: ;ar`). The presets build in `build/<preset>`; if
  `build/` still holds a tree configured with a kit or a Visual Studio
  generator (`build/CMakeCache.txt`), delete it once before the first configure.

- **MSYS2**: from a UCRT64 shell (`pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja}`),
  the generic commands above.

Addons compiled with MSVC by node-gyp still load in the MinGW-built
`quickjs-napi.exe`: Node-API is a plain C ABI, and node-gyp's delay-load hook
resolves the `napi_*` imports from the running executable.

## Usage

```sh
quickjs-napi script.js [args...]
```

The script runs as a CommonJS module (`host/bootstrap.js`): `require`
loads `.js`, `.json` and `.node` files with Node.js resolution rules
(relative paths, directories with `package.json` "main" or `index.*`,
`node_modules`). Globals: a subset of `process` (`argv`, `platform`,
`exitCode`, `exit`, `on`/`emit` for `exit`, `uncaughtException` and
`unhandledRejection`, `nextTick`, `cwd`), `global`, `gc()`, timers
(`setTimeout`, `setInterval`, `setImmediate`, `queueMicrotask`; builtin
modules `timers` and `timers/promises`), and the quickjs-libc helpers
(`console.log`, `print`, `scriptArgs`). As in Node.js, an exception no
frame catches (a timer, a microtask, a Node-API finalizer) goes to
`process.on('uncaughtException')` or ends the process with exit code 1, and
an unhandled promise rejection is treated the same way. ES modules and
package `exports` maps are not supported yet.

Embedding: see `include/qjsnapi.h`
(`qjsnapi_host_create`, `qjsnapi_load_addon`, `qjsnapi_init_static_addon`,
`qjsnapi_host_destroy`). An embedder's event loop calls
`qjsnapi_host_run_finalizers` once per iteration and installs
`qjsnapi_host_set_uncaught_handler` for exceptions thrown by finalizers.

## Layout

| Path | |
|---|---|
| `include/` | Node-API headers (v10), QuickJS headers, `qjsnapi.h` |
| `src/env.c` | host and env lifecycle, last error, cleanup hooks |
| `src/lifetime.c` | references, externals, wrap, finalizers and their queue, type tags, instance data |
| `src/scope.c` | handle-scope arena (`napi_value` = pointer to an owned `JSValue` slot) |
| `src/function.c` | functions with `new.target` support (class with a `call` hook), calls, `napi_define_class`, `napi_run_script` |
| `src/value.c` | primitives, numbers, coercion, arrays, strict equality |
| `src/string.c` | strings (latin1/UTF-8/UTF-16, lossless through WTF-8/CESU-8), property keys, symbols |
| `src/object.c` | properties, `napi_define_properties`, property names, prototype, freeze/seal, `instanceof` |
| `src/error.c` | errors and exceptions |
| `src/builtins.c` | ArrayBuffer, TypedArray, DataView, BigInt, Date, Promise/deferred |
| `src/loader.c` | `.node` loading: `napi_register_module_v1`, API version, legacy `napi_module_register` |
| `src/stubs_generated.c` | generated by `tools/gen_stubs.py`, do not edit |
| `host/main.c`, `host/bootstrap.js` | the `quickjs-napi` executable: event loop, CommonJS loader, timers, `process` (embedded at build time by `cmake/embed.cmake`) |
| `test/addons`, `test/js` | our test addons; the JS tests also run under Node.js as reference |
| `test/node` | Node.js conformance tests (vendored, see its README) |
| `test/unit` | C embedding test (teardown order, scopes, error state, finalizer queue) |
| `test/lsan.supp` | LeakSanitizer suppressions for leaks the Node.js tests make by design |

After implementing a function, run `python3 tools/gen_stubs.py` to drop its
stub; the `stubs_up_to_date` test fails otherwise.
