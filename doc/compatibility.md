# Compatibility with Node.js

quickjs-napi reproduces the Node-API behaviour of **Node.js 22.23.4**: same
statuses, same pending exceptions, same error codes and messages, same
finalizer ordering. This page lists every known difference, so that addon
authors know what to expect. Each item links to the function reference.

- [Deviations in the API](#deviations-in-the-api)
- [Differences that come from QuickJS](#differences-that-come-from-quickjs)
- [Behaviour that is defined here but not in Node.js](#behaviour-that-is-defined-here-but-not-in-nodejs)
- [Not provided](#not-provided)
- [The executable](#the-executable)

## Deviations in the API

| Function | Node.js 22 | quickjs-napi | Reference |
|---|---|---|---|
| `napi_define_class` | prototype methods have a V8 *signature*: calling one on a foreign object throws `TypeError: Illegal invocation` | the callback runs; it must validate `this` (e.g. with `napi_unwrap`) | [functions.md](node-api/functions.md#napi_define_class) |
| `napi_is_array` | `false` for every Proxy | `true` for a Proxy whose target is an array (as `Array.isArray`); `false` for a revoked Proxy | [values.md](node-api/values.md#napi_is_array) |
| `napi_create_buffer` | memory uninitialized | memory zero-filled | [node-specific.md](node-api/node-specific.md#napi_create_buffer) |
| `node_api_create_buffer_from_arraybuffer`, range out of bounds | `napi_ok` with the `RangeError` pending | `napi_pending_exception` with the same `RangeError` pending | [node-specific.md](node-api/node-specific.md#node_api_create_buffer_from_arraybuffer) |
| `napi_create_typedarray` / `napi_get_typedarray_info` with `napi_float16_array` | `napi_invalid_arg` | supported (QuickJS has `Float16Array`) | [binary-data.md](node-api/binary-data.md#typedarray) |
| `napi_get_typedarray_info`, out-of-bounds non-detached view of a resizable buffer | reports the stored byte offset | reports byte offset 0 (QuickJS does not expose it) | [binary-data.md](node-api/binary-data.md#napi_get_typedarray_info) |
| `napi_get_threadsafe_function_context` with `NULL` arguments | aborts | `napi_invalid_arg` | [async.md](node-api/async.md#napi_get_threadsafe_function_context) |
| `napi_module_register` outside of an addon load | registers a module | prints a warning and does nothing (use `qjsnapi_init_static_addon`) | [environment.md](node-api/environment.md#napi_module_register) |

## Differences that come from QuickJS

| Area | Effect |
|---|---|
| **Finalizer timing** | QuickJS frees an object as soon as its last reference goes away (cycles at `JS_RunGC`). Finalizers therefore usually run **earlier** than in Node.js, at the next loop iteration instead of at some later major GC. Code that assumes an object stays alive while only weakly referenced still works within the same macrotask (the kept-objects list), but not across macrotasks. See [architecture.md](architecture.md#object-lifetime). |
| **BigInt size** | at most 2^20 bits (16384 words; V8: 2^30). `napi_create_bigint_words` beyond that throws `RangeError: Maximum BigInt size exceeded`, as V8 does past its own limit. |
| **BigInt words** | `napi_*_bigint_words` convert through a hexadecimal string: O(n), slower than V8's direct access for very large values. |
| **External strings** | `node_api_create_external_string_*` always copy and run the finalizer immediately (`*copied = true`). Node.js does the same when V8 uses the pointer-compression sandbox, so addons handle it already. |
| **Property keys** | `node_api_create_property_key_*` create ordinary strings: QuickJS atomizes keys itself. Not observable. |
| **External memory** | `napi_adjust_external_memory` keeps the counter but does not influence garbage collection: QuickJS has no such heuristic. |
| **Basic finalizers** | all finalizers run outside the GC, so calling any Node-API function from a `node_api_basic_finalize` never triggers Node's fatal "Finalizer is calling a function that may affect GC state" error. |
| **Async context** | there is no `async_hooks`: `napi_async_init`, `napi_make_callback` and callback scopes work, but no async resource is tracked (`AsyncLocalStorage` and async stack traces do not follow native callbacks). |

## Behaviour that is defined here but not in Node.js

These cases are undefined behaviour or leaks in Node.js; quickjs-napi chose a
safe behaviour:

| Case | quickjs-napi |
|---|---|
| `napi_delete_async_work` on a queued item | the item is freed after its `complete` has run |
| async work items and user references never deleted | freed with the env at teardown (Node.js leaks them) |
| a user `napi_ref` whose finalizer ran but that the addon never deleted | freed at teardown |
| a handle scope left open by a function called from JavaScript | closed with the call's implicit scope (warning in Debug builds) |

## Not provided

- `NAPI_EXPERIMENTAL` APIs: the bundled headers stop at version 10 and
  declare no experimental function (`node_api_post_finalizer`, ...); an addon
  that needs one fails to load because the symbol is missing.
- `napi_would_deadlock` is never returned (Node.js 22 does not return it
  either); a blocking call of a full thread-safe function from the loop
  thread deadlocks.

## The executable

The `quickjs-napi` executable is not Node.js; it provides what addon packages
and their tests need ([host.md](host.md)). In particular:

- `process.version` is `v22.23.4` (the reproduced Node-API behaviour) but
  `process.versions.node` is not set;
- CommonJS only: no ES modules, no `package.json` `"exports"` maps;
- no `worker_threads`, `async_hooks`, `domain`, `fs`, `net`, `http`,
  `crypto`;
- `child_process` without a shell, and an asynchronous child's stdin is never
  a pipe.
