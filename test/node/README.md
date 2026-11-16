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

`common/` is ours: a minimal replacement of Node's `test/common` and of the
`assert` module, providing only what the enabled tests use.

## Enabled tests

| Milestone | Tests |
|---|---|
| M1 | `2_function_arguments`, `3_callbacks`, `4_object_factory`, `5_function_factory`, `test_array`, `test_constructor`, `test_conversions`, `test_error`, `test_handle_scope`, `test_new_target`, `test_number`, `test_properties`, `test_string`, `test_symbol` |

`test_exception`, `test_function` and `test_object` also exercise externals,
wrapping, references and type tags: they are enabled with milestone M2.

## Documented deviations

Applied by the script (`PATCHES`), each with its reason. Only differences of
the JavaScript engine are patched, never Node-API semantics:

| Test | Deviation |
|---|---|
| `test_constructor`, `test_properties` | The regular expressions matching V8's TypeError messages for read-only and getter-only assignments also accept QuickJS's wording. The error type and the failing assignment are still checked. |
| `test_string` | External strings are always copied (`copied == true`, finalizer already invoked): QuickJS strings cannot reference external memory. Node.js behaves the same when V8 runs with its sandbox; the content checks are unchanged. |
