# Node.js conformance tests

`js-native-api/` and `node-api/` contain tests copied from the Node.js
repository (`test/js-native-api` and `test/node-api`, tag **v22.23.4**) by
`tools/sync_node_tests.py`. They are Node.js code under the MIT license in
`LICENSE.nodejs`. Do not edit them by hand: add the test to `TESTS` in the
script and run

```sh
git clone --depth 1 --branch v22.23.4 --filter=blob:none --sparse https://github.com/nodejs/node
git -C node sparse-checkout set --no-cone /test/js-native-api/ /test/node-api/ /test/common/gc.js /test/common/tick.js /LICENSE
python3 tools/sync_node_tests.py node
```

The script also generates `<suite>/tests.cmake` from each `binding.gyp`, so
the build needs neither Python nor node-gyp. CTest names the tests
`node/<suite>/<test>/<script>`.

`common/` is ours (a minimal replacement of Node's `test/common` and of the
`assert` module, providing only what the enabled tests use, and
`require-as-module.js`), except `common/gc.js` and `common/tick.js`, copied
verbatim from Node.js by the script.

Every `.js` file of an enabled test directory is a test script, as in
Node.js, with the exceptions declared in the script.

## Enabled tests

| Milestone | Suite | Tests |
|---|---|---|
| M1 | js-native-api | `2_function_arguments`, `3_callbacks`, `4_object_factory`, `5_function_factory`, `test_array`, `test_constructor`, `test_conversions`, `test_error`, `test_handle_scope`, `test_new_target`, `test_number`, `test_properties`, `test_string`, `test_symbol` |
| M2 | js-native-api | `6_object_wrap`, `7_factory_wrap`, `8_passing_wrapped` (C++), `test_cannot_run_js`, `test_exception`, `test_function`, `test_general`, `test_instance_data`, `test_object`, `test_reference`, `test_reference_double_free` |
| M3 | js-native-api | `test_bigint`, `test_dataview`, `test_date`, `test_promise`, `test_typedarray` (including `test_sharedarraybuffer.js`) |
| M4 | node-api | `1_hello_world`, `test_buffer`, `test_env_teardown_gc`, `test_exception`, `test_fatal_exception`, `test_general`, `test_init_order` (C++), `test_make_callback`, `test_null_init`, `test_reference_by_node_api_version` |

Not synced yet: the `node-api` tests that include `uv.h` arrive with the libuv
event loop (M5): `test_async`, `test_async_cleanup_hook`,
`test_callback_scope`, `test_cleanup_hook`, `test_fatal`,
`test_instance_data`, `test_threadsafe_function`, `test_uv_loop`,
`test_uv_threadpool_size`. Not planned: `test_async_context`,
`test_make_callback_recurse` (async_hooks, domain), `test_worker_*`
(worker_threads).

Not built or not run (`SKIP_TARGETS`, `SKIP_SCRIPTS`):

| Test | Reason |
|---|---|
| `js-native-api/test_sharedarraybuffer` (directory) | Built with `NAPI_EXPERIMENTAL` (`node_api_create_sharedarraybuffer`, `node_api_is_sharedarraybuffer`). SharedArrayBuffer support of the stable APIs is covered by `test_typedarray/test_sharedarraybuffer.js`. |
| `js-native-api/test_finalizer` (directory), `js-native-api/6_object_wrap/test-basic-finalizer.js` | Built with `NAPI_EXPERIMENTAL` (`node_api_post_finalizer`, finalizers run inside the GC): the project headers do not declare the experimental APIs. |
| `node-api/test_make_callback/test-async-hooks.js` | Needs `async_hooks`. |

Run in child mode (`CHILD_RUNS`): these scripts re-run themselves with
`child_process.spawnSync`, which quickjs-napi does not provide. CTest runs the
child mode directly and checks the output the parent would check:

| Script | Check |
|---|---|
| `js-native-api/test_exception/testFinalizerException.js` | `child` mode prints `Error during Finalize` (the uncaught exception of a finalizer) |
| `js-native-api/test_general/testEnvCleanup.js` | `child` mode prints the finalizer lines of `simple wrap` and `second wrap` only |
| `js-native-api/test_instance_data/test.js` | loaded with `require()` (`module !== require.main`): prints `deleting addon data` at teardown, no failure. The worker-thread variant is not run (no `worker_threads`). |

## Documented deviations

Applied by the script (`PATCHES`) right after copying each test, each with
its reason. Only differences of the JavaScript engine, or capabilities the
quickjs-napi host does not have, are patched, never Node-API semantics:

| Test | Deviation |
|---|---|
| `js-native-api/test_constructor`, `test_properties` | The regular expressions matching V8's TypeError messages for read-only and getter-only assignments also accept QuickJS's wording. The error type and the failing assignment are still checked. |
| `js-native-api/test_string` | External strings are always copied (`copied == true`, finalizer already invoked): QuickJS strings cannot reference external memory. Node.js behaves the same when V8 runs with its sandbox; the content checks are unchanged. |
| `js-native-api/6_object_wrap`, `test_object` | Same as above for the getter-only, read-only, non-extensible and non-configurable TypeError messages. |
| `js-native-api/test_object/test_object.c` | Test bug: `VALIDATE_TYPE_INDEX` threw a RangeError and went on reading `type_tags[index]` out of bounds (undefined behaviour, trapped by UBSan); it now returns after throwing, which is what the test expects. |
| `node-api/1_hello_world` | The `worker_threads` part is removed (no workers). Reloading the addon after deleting its `require.cache` entry is still checked. |
| `node-api/test_buffer` | `newExternalSharedArrayBuffer` (`node_api_create_external_sharedarraybuffer`, `NAPI_EXPERIMENTAL`) and its check are removed; the target is built with `NAPI_VERSION=10` instead of `NAPI_EXPERIMENTAL`, which the project headers ignore (it would select version 8 and hide `node_api_create_buffer_from_arraybuffer`). |
| `node-api/test_general` | The block that copies the addon to a `foo%#bar` directory and reloads it from a child process is disabled (no `child_process`, `fs`, `path`); the same file-URL encoding is checked by `test/js/test_node_api.js`. |

With `QJSNAPI_SANITIZE`, `test/lsan.supp` suppresses the leaks the tests make
by design (a user-owned `napi_ref` the addon never deletes, mostly because a
finalizer returns early on an expected failure; Node.js leaks it too). Each
entry names the function or file that creates the reference.
