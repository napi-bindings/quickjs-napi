# quickjs-napi

[![CI](https://github.com/napi-bindings/quickjs-napi/actions/workflows/ci.yml/badge.svg?branch=napi)](https://github.com/napi-bindings/quickjs-napi/actions/workflows/ci.yml)

Node-API implementation for QuickJS (2026-06-04, from `../quickjs-build`).
Addons built for Node.js (node-gyp, node-addon-api, napi-rs) are loaded
unchanged: they resolve `napi_*` from the `quickjs-napi` executable.

Status: milestone **M6** in progress (M1 values, strings, objects, properties,
functions, classes, errors; M2 references, externals, wrapping, finalizers,
type tags, instance data, `napi_run_script`; M3 ArrayBuffer, TypedArray,
DataView, BigInt, Date, Promise; M4 the synchronous `node_api.h`: Buffer,
async context, `napi_make_callback`, callback scopes,
`napi_fatal_exception`; M5 the libuv event loop: async work, thread-safe
functions, async cleanup hooks, `napi_get_uv_event_loop`; M6 real-world
conformance). All 155 Node-API functions are implemented and pass 30 tests
of the Node.js `js-native-api` suite, 19 of its `node-api` suite (see
`test/node/README.md`) and 78 of the 83 test modules of the node-addon-api
8.9.2 suite (the others need `worker_threads`, `npm` or test the npm
package itself), in its four binding variants (C++ exceptions, no exceptions,
`Maybe`, custom namespace; see `test/node-addon-api/harness/README.md`).
`NAPI_EXPERIMENTAL` APIs are not provided. See `PIANO-IMPLEMENTAZIONE.md`
in the project for the plan.

## Lifetime model

- `napi_ref` follows `v8impl::Reference`: strong while its count is > 0; at
  count 0 an object, function or local symbol is held through a JS `WeakRef`
  (QuickJS reports the target dead as soon as its refcount drops to zero),
  other values are released.
- V8 keeps the target of a weak handle until a garbage collection; QuickJS
  frees an object when its refcount drops to zero, so an object created by
  a native call and only referenced weakly would die when the call returns
  (node-addon-api relies on the V8 behaviour: `Napi::Weak(Object::New(env))`
  used later in the same synchronous code). When a reference becomes weak its
  target is therefore also put on a list of *kept objects*, like the
  `[[KeptAlive]]` list of ECMAScript WeakRefs, cleared at the end of every
  macrotask (the library's checkpoint), by `gc()` and at teardown;
  embedders call `qjsnapi_host_clear_kept_objects` at the end of their own
  macrotasks.
- `napi_define_class`: as with V8's prototype template, a later instance
  property with the same key replaces an earlier one, whatever its
  attributes (static properties are defined like `napi_define_properties`,
  where such a redefinition fails, in Node.js too).
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

## Buffer and the synchronous node_api.h

- Buffers are instances of a minimal Node.js `Buffer` (`src/buffer.js`,
  embedded in the library): a `Uint8Array` subclass with `Buffer.from`,
  `alloc`, `allocUnsafe`, `isBuffer`, `byteLength`, `concat`, `compare` and
  `toString`/`write`/`equals`/`copy`/`fill`/`slice`/`toJSON`/`indexOf` for
  utf8, utf16le, latin1, ascii, hex, base64 and base64url. The library
  installs no global; `qjsnapi_host_get_buffer()` gives the class to the
  embedder. `napi_create_buffer` returns zero-filled memory (Node.js:
  uninitialized). As in Node.js 22, `napi_is_buffer` and
  `napi_get_buffer_info` accept any ArrayBufferView; external buffers use
  the memory in place and their finalizer is queued after collection.
- No async_hooks: an async context only records that it exists. What
  remains of Node's callback scopes is the microtask checkpoint when the
  outermost scope closes with no JavaScript below the native code; from a
  Node-API callback called by JavaScript (the usual case) nothing runs
  early, as in Node.js. `napi_fatal_exception` reaches
  `process.on('uncaughtException')` synchronously.
- `napi_get_node_version` reports 22.23.4, the Node.js release whose
  behaviour is reproduced, with release name `quickjs-napi`
  (`qjsnapi_node_version()`; the executable's `process.version` matches).
- Every `qjsnapi_load_addon` runs the addon's initializer again with a new
  env, like `process.dlopen` (re-`require` after deleting the cache entry
  gives new exports); a legacy `napi_module` registered by the first
  `dlopen` is reused. File URLs (`node_api_get_module_file_name`) are
  encoded like Node's `url.pathToFileURL`.
- Deviation: `node_api_create_buffer_from_arraybuffer` with an
  out-of-range offset + length returns `napi_pending_exception`; Node.js
  returns `napi_ok` with the RangeError pending and no result.

## Event loop, async work, thread-safe functions

- The host runs on a libuv loop (v1.51.0, built statically and linked
  into the executable, whose `uv_*` symbols are exported like `napi_*`, so
  addons that use libuv directly load too). `qjsnapi_host_create` owns a
  loop; `qjsnapi_host_create_with_loop` uses the embedder's.
  `napi_get_uv_event_loop` returns it.
- Finalizers queued by the GC run from a `uv_check` handle of the library
  (an idle handle keeps the loop alive while some are queued), each followed
  by a checkpoint: microtasks by default, or the embedder's
  (`qjsnapi_host_set_checkpoint`).
- Async work runs `execute` on libuv's thread pool (`UV_THREADPOOL_SIZE`
  applies) and `complete` on the loop thread. `napi_cancel_async_work` maps
  to `uv_cancel`. A work item deleted while queued is freed after its
  completion (Node.js: undefined behaviour).
- Thread-safe functions are a port of Node's `ThreadSafeFunction`: mutex,
  bounded queue with a condition variable, `uv_async_t` dispatch of at most
  1000 calls per iteration, `napi_tsfn_abort`, ref/unref of the handle, and
  closing at env teardown (an internal cleanup hook) with `call_js_cb(env =
  NULL)` for the items still queued.
- An exception thrown by `complete`, `call_js_cb` or a finalizer goes to
  `process.on('uncaughtException')`. For thread-safe functions of an addon
  built with `NAPI_VERSION` < 10 it is dropped with a DEP0168 warning, as in
  Node.js, unless `--force-node-api-uncaught-exceptions-policy` is given
  (`qjsnapi_host_set_force_uncaught_exceptions_policy`).
- Teardown (`qjsnapi_host_destroy`) mirrors Node's: cleanup hooks newest
  first (env and async ones, thread-safe functions closed there), queued
  work cancelled, the loop run until every in-flight work item, closing
  thread-safe function and async cleanup hook is done, then the reference
  finalizers with JavaScript disabled. Async work items and user references
  the addon never deleted are freed with the env (Node.js leaks them).
- `process.exit()` and a fatal exception end the executable like Node.js
  (`DefaultProcessExitHandlerInternal`): the `exit` event is emitted, the
  thread pool is shut down, and the process exits without tearing the
  environment down (no cleanup hook or finalizer runs, work in flight is not
  waited for). A normal end of the script tears everything down as above.
- `napi_fatal_error` prints `FATAL ERROR: <location> <message>` and aborts
  (`_exit(134)` on Windows, as Node.js).

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
| `unix-tsan` | Linux/macOS, Debug + ThreadSanitizer, QuickJS and libuv included |
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
ASan/UBSan, gcc with ThreadSanitizer and macOS arm64 with the `unix-*`
presets, and Windows x64 with the `windows-zig-*` presets; the Linux and
macOS jobs without sanitizers also run the `node_parity_*` tests under
Node.js 22.

libuv is fetched at configure time (`FetchContent`, tag v1.51.0). To build
offline, pass `-DFETCHCONTENT_SOURCE_DIR_LIBUV=<libuv checkout>`.

Options:

| Option | Default | |
|---|---|---|
| `QJSNAPI_QUICKJS_DIR` | `../quickjs-build` | QuickJS sources, built as a subproject |
| `QJSNAPI_QUICKJS_LIBRARY` | empty | prebuilt `libqjs.a` when the sources are not available |
| `QJSNAPI_SANITIZE` | OFF | ASan + UBSan for everything, QuickJS included |
| `QJSNAPI_TSAN` | OFF | ThreadSanitizer for everything (exclusive with `QJSNAPI_SANITIZE`) |
| `QJSNAPI_BUILD_TESTS` | ON | test addons and CTest suite |
| `QJSNAPI_NODE_ADDON_API_TESTS` | ON | the node-addon-api suite (8 C++ bindings, about 2 minutes of build) |

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
quickjs-napi [options] script.js [args...]
quickjs-napi [options] -e <code> [args...]
```

Options: `-e`/`--eval <code>` (run the code instead of a script,
`process.argv` is `[execPath, args...]`), `-r`/`--require <module>`
(loaded before the script, repeatable),
`--force-node-api-uncaught-exceptions-policy[=true|false]` and
`--no-force-node-api-uncaught-exceptions-policy` (see above), `--expose-gc`
(accepted and ignored: `gc()` is always defined). An unknown option exits
with code 9, as Node.js. `NODE_OPTIONS` is read first, with the same
options separated by spaces; the options only Node.js knows are ignored
there (the variable is shared with Node.js).

The script runs as a CommonJS module (`host/bootstrap.js`): `require`
loads `.js`, `.json` and `.node` files with Node.js resolution rules
(relative paths, directories with `package.json` "main" or `index.*`,
`node_modules`). Globals: a subset of `process` (`argv`, `platform`,
`exitCode`, `exit`, `on`/`emit` for `exit`, `uncaughtException` and
`unhandledRejection`, `nextTick`, `cwd`), `global`, `gc()`, timers
(`setTimeout`, `setInterval`, `setImmediate`, `queueMicrotask`; builtin
modules `timers` and `timers/promises`), `Buffer` (also `require('buffer')`),
minimal builtin modules `util` (`promisify`, `types`), `url`
(`pathToFileURL`, `fileURLToPath`) and `vm` (`runInNewContext`, a new realm
of the same runtime), `child_process` (`spawnSync` and `spawn` without a
shell; stdio `pipe`, `inherit` or `ignore`, an asynchronous child's stdin is
never a pipe), `events` (a minimal `EventEmitter`), `process.env` (the real
environment), `process.version`, and the quickjs-libc helpers
(`console.log`, `print`, `scriptArgs`). As in Node.js, an exception no
frame catches (a timer, a microtask, a Node-API finalizer) goes to
`process.on('uncaughtException')` or ends the process with exit code 1, and
an unhandled promise rejection is treated the same way. ES modules and
package `exports` maps are not supported yet.

Embedding: see `include/qjsnapi.h`
(`qjsnapi_host_create`, `qjsnapi_host_create_with_loop`,
`qjsnapi_host_get_loop`, `qjsnapi_load_addon`, `qjsnapi_init_static_addon`,
`qjsnapi_host_get_buffer`, `qjsnapi_node_version`, `qjsnapi_host_destroy`).
The embedder runs the host's libuv loop (`uv_run`), which also runs the
queued finalizers; it installs `qjsnapi_host_set_uncaught_handler` for
exceptions thrown by native callbacks and may install
`qjsnapi_host_set_checkpoint` to drain microtasks, rejections and its own
timers after each one, and calls `qjsnapi_host_clear_kept_objects` at the
end of each macrotask it runs itself and before an explicit GC.

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
| `src/buffer.c`, `src/buffer.js` | Buffer APIs and the minimal Buffer class (embedded at build time) |
| `src/node_api.c` | async context, `napi_make_callback`, callback scopes, `napi_fatal_exception`, the frame of loop-originated callbacks |
| `src/loop.c` | libuv loop ownership, finalizer check/idle handles, drain and close at teardown, `napi_get_uv_event_loop` |
| `src/async.c` | `napi_async_work` on the libuv thread pool |
| `src/tsfn.c` | thread-safe functions |
| `src/loader.c` | `.node` loading: `napi_register_module_v1`, API version, legacy `napi_module_register` |
| `src/stubs_generated.c` | generated by `tools/gen_stubs.py`, do not edit (empty now: every function is implemented) |
| `host/main.c`, `host/bootstrap.js` | the `quickjs-napi` executable: options, libuv event loop (timers, immediates, `beforeExit`), CommonJS loader, `process` (embedded at build time by `cmake/embed.cmake`), `buffer`, `util`, `url`, `vm`, `child_process`, `events` |
| `host/spawn.c` | child processes (`spawnSync` on a private loop, `spawn` on the main one) |
| `test/addons`, `test/js` | our test addons; the JS tests also run under Node.js as reference |
| `test/node` | Node.js conformance tests (vendored, see its README) |
| `test/node-addon-api` | node-addon-api headers and test suite (vendored by `tools/sync_node_addon_api.py`); `harness/` is ours (see its README) |
| `cmake/node_addon_api_tests.cmake` | builds the 8 node-addon-api test bindings, one CTest per test module |
| `test/unit` | C embedding test (teardown order, scopes, error state, finalizer queue) |
| `test/lsan.supp` | LeakSanitizer suppressions for leaks the Node.js and node-addon-api tests make by design |
| `cmake/expect_abort.cmake` | runs a Node.js test that must abort (`napi_fatal_error`) and checks its output |

After implementing a function, run `python3 tools/gen_stubs.py` to drop its
stub; the `stubs_up_to_date` test fails otherwise.
