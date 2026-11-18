# quickjs-napi

[![CI](https://github.com/napi-bindings/quickjs-napi/actions/workflows/ci.yml/badge.svg?branch=napi)](https://github.com/napi-bindings/quickjs-napi/actions/workflows/ci.yml)

Node-API implementation for [QuickJS](https://bellard.org/quickjs/)
(2026-06-04, from `../quickjs-build`). Addons built for Node.js (node-gyp,
node-addon-api, napi-rs) are loaded **unchanged**: they resolve `napi_*` from
the `quickjs-napi` executable, exactly as they do from `node`.

```sh
quickjs-napi -e "console.log(require('./build/Release/addon.node').hello())"
```

## Status

Milestone **M6** in progress (real-world conformance). Done: M1 values,
strings, objects, properties, functions, classes, errors; M2 references,
externals, wrapping, finalizers, type tags, instance data, `napi_run_script`;
M3 ArrayBuffer, TypedArray, DataView, BigInt, Date, Promise; M4 the
synchronous `node_api.h` (Buffer, async context, `napi_make_callback`,
callback scopes, `napi_fatal_exception`); M5 the libuv event loop (async
work, thread-safe functions, async cleanup hooks,
`napi_get_uv_event_loop`). See `PIANO-IMPLEMENTAZIONE.md` in the project for
the plan.

- All **155 Node-API functions** (version 10) are implemented.
- They pass 30 tests of the Node.js `js-native-api` suite, 19 of its
  `node-api` suite, and 78 of the 83 test modules of the **node-addon-api
  8.9.2** suite in its four binding variants (the others need
  `worker_threads`, `npm` or test the npm package itself).
- Behaviour follows **Node.js 22.23.4**; the few differences are listed in
  [compatibility.md](doc/compatibility.md). `NAPI_EXPERIMENTAL` APIs are not
  provided.

## Quick start

`quickjs-build` must be checked out next to this repository
(`../quickjs-build`, branch `feat/quickjs-2026-06-04`).

```sh
cmake --preset unix-debug            # windows-zig-debug on Windows
cmake --build --preset unix-debug
ctest --preset unix-debug
```

```sh
quickjs-napi [options] script.js [args...]
quickjs-napi [options] -e <code> [args...]
```

Details: [building](doc/building.md), [the executable](doc/host.md).

## Documentation

| Document | Contents |
|---|---|
| [Architecture](doc/architecture.md) | goals, components, host and env, handle-scope arena, function calls, error model, object lifetime on a reference-counting engine, event loop, addon loading, teardown, design decisions; with diagrams |
| [Embedding API](doc/embedding.md) | `qjsnapi.h`: embedding the library in a QuickJS application, every `qjsnapi_*` function, embedder responsibilities, symbol export |
| [Node-API reference](doc/node-api/README.md) | conventions (statuses, pending exceptions, threads), writing an addon, and an index of all 155 functions |
| ↳ [Environment](doc/node-api/environment.md) | last error, versions, module file name, cleanup hooks, `napi_fatal_error`, legacy registration |
| ↳ [Values](doc/node-api/values.md) | singletons, numbers, booleans, `typeof`, coercion, arrays, strict equality |
| ↳ [Strings](doc/node-api/strings.md) | latin1 / UTF-8 / UTF-16 strings, external strings, property keys, symbols |
| ↳ [Objects](doc/node-api/objects.md) | properties by key, name and index, `napi_define_properties`, key enumeration, prototype, freeze/seal, `instanceof` |
| ↳ [Functions](doc/node-api/functions.md) | native functions, `napi_get_cb_info`, calls, `new.target`, classes, `napi_run_script` |
| ↳ [Errors](doc/node-api/errors.md) | error objects, throwing, pending exceptions |
| ↳ [Handle scopes](doc/node-api/scopes.md) | handle and escapable scopes |
| ↳ [Lifetime](doc/node-api/lifetime.md) | references, wrapping, finalizers, externals, type tags, instance data, external memory |
| ↳ [Binary data](doc/node-api/binary-data.md) | ArrayBuffer, TypedArray, DataView, BigInt, Date, Promise |
| ↳ [Node.js-specific](doc/node-api/node-specific.md) | Buffer, async context, `napi_make_callback`, callback scopes, `napi_fatal_exception` |
| ↳ [Asynchronous APIs](doc/node-api/async.md) | async work, thread-safe functions, `napi_get_uv_event_loop` |
| [The executable](doc/host.md) | command line, `NODE_OPTIONS`, CommonJS modules, globals, builtin modules, `child_process`, event loop, exit |
| [Building](doc/building.md) | prerequisites, presets, options, Windows (zig, MSYS2), build outputs, CI |
| [Testing](doc/testing.md) | test suites, Node.js parity, sanitizers, tools, development workflows |
| [Compatibility](doc/compatibility.md) | every known difference from Node.js 22 |

Test-suite specific notes: [test/node/README.md](test/node/README.md)
(Node.js conformance tests) and
[test/node-addon-api/harness/README.md](test/node-addon-api/harness/README.md)
(node-addon-api suite).

## Repository layout

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
| `host/main.c`, `host/bootstrap.js` | the `quickjs-napi` executable: options, event loop, CommonJS loader, `process` and builtin modules (`bootstrap.js` embedded at build time by `cmake/embed.cmake`) |
| `host/spawn.c` | child processes (`spawnSync` on a private loop, `spawn` on the main one) |
| `test/addons`, `test/js` | our test addons; the JS tests also run under Node.js as reference |
| `test/node` | Node.js conformance tests (vendored) |
| `test/node-addon-api` | node-addon-api headers and test suite (vendored by `tools/sync_node_addon_api.py`); `harness/` is ours |
| `test/unit` | C embedding test (teardown order, scopes, error state, finalizer queue) |
| `test/lsan.supp` | LeakSanitizer suppressions for leaks the Node.js and node-addon-api tests make by design |
| `cmake/node_addon_api_tests.cmake` | builds the 8 node-addon-api test bindings, one CTest per test module |
| `cmake/expect_abort.cmake` | runs a Node.js test that must abort (`napi_fatal_error`) and checks its output |
| `tools/` | stub generation, export check, test vendoring ([testing.md](doc/testing.md#tools)) |
| `doc/` | this documentation |
