# Node.js conformance tests

`js-native-api/` contains tests copied from the Node.js repository
(`test/js-native-api`, tag **v22.23.4**) by `tools/sync_node_tests.py`. They
are Node.js code under the MIT license in `LICENSE.nodejs`. Do not edit them
by hand: add the test to `TESTS` in the script and run

```sh
git clone --depth 1 --branch v22.23.4 --filter=blob:none --sparse https://github.com/nodejs/node
git -C node sparse-checkout set --no-cone /test/js-native-api/ /LICENSE
python3 tools/sync_node_tests.py node
```

The script also generates `js-native-api/tests.cmake` from each
`binding.gyp`, so the build needs neither Python nor node-gyp.

`common/` is ours (a minimal replacement of Node's `test/common` and of the
`assert` module, providing only what the enabled tests use, and
`require-as-module.js`), except `common/gc.js`, copied verbatim from Node.js
by the script.

Every `.js` file of an enabled test directory is a test script, as in
Node.js, with the exceptions declared in the script.

## Enabled tests

| Milestone | Tests |
|---|---|
| M1 | `2_function_arguments`, `3_callbacks`, `4_object_factory`, `5_function_factory`, `test_array`, `test_constructor`, `test_conversions`, `test_error`, `test_handle_scope`, `test_new_target`, `test_number`, `test_properties`, `test_string`, `test_symbol` |
| M2 | `6_object_wrap`, `7_factory_wrap`, `8_passing_wrapped` (C++), `test_cannot_run_js`, `test_exception`, `test_function`, `test_general`, `test_instance_data`, `test_object`, `test_reference`, `test_reference_double_free` |
| M3 | `test_bigint`, `test_dataview`, `test_date`, `test_promise`, `test_typedarray` (including `test_sharedarraybuffer.js`) |

Not built or not run (`SKIP_TARGETS`, `SKIP_SCRIPTS`):

| Test | Reason |
|---|---|
| `test_sharedarraybuffer` (directory) | Built with `NAPI_EXPERIMENTAL` (`node_api_create_sharedarraybuffer`, `node_api_is_sharedarraybuffer`). SharedArrayBuffer support of the stable APIs is covered by `test_typedarray/test_sharedarraybuffer.js`. |
| `test_finalizer` (directory), `6_object_wrap/test-basic-finalizer.js` | Built with `NAPI_EXPERIMENTAL` (`node_api_post_finalizer`, finalizers run inside the GC): the project headers do not declare the experimental APIs. |

Run in child mode (`CHILD_RUNS`): these scripts re-run themselves with
`child_process.spawnSync`, which quickjs-napi does not provide. CTest runs the
child mode directly and checks the output the parent would check:

| Script | Check |
|---|---|
| `test_exception/testFinalizerException.js` | `child` mode prints `Error during Finalize` (the uncaught exception of a finalizer) |
| `test_general/testEnvCleanup.js` | `child` mode prints the finalizer lines of `simple wrap` and `second wrap` only |
| `test_instance_data/test.js` | loaded with `require()` (`module !== require.main`): prints `deleting addon data` at teardown, no failure. The worker-thread variant is not run (no `worker_threads`). |

## Documented deviations

Applied by the script (`PATCHES`), each with its reason. Only differences of
the JavaScript engine are patched, never Node-API semantics:

| Test | Deviation |
|---|---|
| `test_constructor`, `test_properties` | The regular expressions matching V8's TypeError messages for read-only and getter-only assignments also accept QuickJS's wording. The error type and the failing assignment are still checked. |
| `test_string` | External strings are always copied (`copied == true`, finalizer already invoked): QuickJS strings cannot reference external memory. Node.js behaves the same when V8 runs with its sandbox; the content checks are unchanged. |
| `6_object_wrap`, `test_object` | Same as above for the getter-only, read-only, non-extensible and non-configurable TypeError messages. |
| `test_object/test_object.c` | Test bug: `VALIDATE_TYPE_INDEX` threw a RangeError and went on reading `type_tags[index]` out of bounds (undefined behaviour, trapped by UBSan); it now returns after throwing, which is what the test expects. |

With `QJSNAPI_SANITIZE`, `test/lsan.supp` suppresses two leaks the tests make
by design (a user-owned `napi_ref` the addon never deletes; Node.js leaks it
too). Each entry names the function that creates the reference.
