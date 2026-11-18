# Testing

quickjs-napi is tested against the behaviour of Node.js itself: most tests
are Node.js's own Node-API tests and the node-addon-api test suite, and our
own JavaScript tests also run under Node.js as a reference. CTest runs about
170 tests per configuration.

```sh
ctest --preset unix-debug                   # everything
ctest --preset unix-debug -R '^naa/'        # the node-addon-api suite
ctest --test-dir build/unix-debug -R js_host --output-on-failure
```

- [Test suites](#test-suites)
- [Consistency checks](#consistency-checks)
- [Sanitizers and leak checks](#sanitizers-and-leak-checks)
- [Tools](#tools)
- [Workflows](#workflows)

## Test suites

| Suite | CTest names | Source | What it covers |
|---|---|---|---|
| Our JS tests | `js_hello`, `js_values`, `js_binary`, `js_node_api`, `js_host` | `test/js`, addons in `test/addons` | values, binary data, `node_api.h`, the executable's host features (child processes, `-e`, `-r`, `NODE_OPTIONS`) |
| Node.js parity | `node_parity_*` | the same `test/js` scripts | runs those tests under Node.js 22 with the same addon binaries: the expectations are checked against the reference implementation |
| Node.js conformance | `node/js-native-api/...`, `node/node-api/...` | `test/node` (Node.js v22.23.4) | 30 tests of `js-native-api` and 19 of `node-api`; see [test/node/README.md](../test/node/README.md) |
| node-addon-api | `naa/<module>` | `test/node-addon-api` (8.9.2) | 78 of the 83 test modules, each in the four binding variants (C++ exceptions, no exceptions, `Maybe`, custom namespace); see [the harness README](../test/node-addon-api/harness/README.md) |
| Embedding | `unit_embed` | `test/unit/test_embed.c` | what a JS test cannot observe: teardown order, handle-scope misuse, the error state, the finalizer queue of the embedding API |
| Fatal errors | `node/node-api/test_fatal/...` | `cmake/expect_abort.cmake` | runs a test that must abort (`napi_fatal_error`) and checks its output |

The test addons are built **exactly like node-gyp addons**: they do not link
against the library and resolve `napi_*` from the executable. The same
binaries load in Node.js, which is what makes the parity tests possible.

Tests not run, with the reason:

- node-addon-api modules that need `worker_threads`, `npm`, or test the npm
  package itself (5 of 83);
- Node.js tests that need `async_hooks` or `domain` (`test_async_context`,
  `test_make_callback_recurse`) or `worker_threads` (`test_worker_*`).

## Consistency checks

| Test | Checks |
|---|---|
| `stubs_up_to_date` | `src/stubs_generated.c` matches the set of functions implemented (`tools/gen_stubs.py --check`) |
| `exports_complete` | the executable exports every Node-API function (`tools/check_exports.py`) |

## Sanitizers and leak checks

- **Debug builds** use a Debug QuickJS, whose `JS_FreeRuntime` asserts that
  every object was released. Any `JSValue` leaked by the library aborts the
  test. This is why the library frees, at teardown, what addons never deleted.
- **`unix-asan`** runs everything under AddressSanitizer, UndefinedBehavior
  Sanitizer and LeakSanitizer, QuickJS included. `test/lsan.supp` suppresses
  the leaks that Node.js and node-addon-api tests make **by design** (each
  entry says why).
- **`unix-tsan`** runs everything under ThreadSanitizer, QuickJS and libuv
  included: async work and thread-safe functions are exercised from several
  threads. CI reduces ASLR entropy for it.

## Tools

| Tool | Purpose |
|---|---|
| `tools/gen_stubs.py` | regenerates `src/stubs_generated.c`: a stub (returning `napi_generic_failure` with a descriptive last-error message) for every function of the headers that no `src/*.c` defines. `--check` only verifies. |
| `tools/check_exports.py <executable>` | verifies that the executable exports every Node-API function (`nm` or `objdump`) |
| `tools/napi_decls.py` | parses the declarations of `js_native_api.h` and `node_api.h`; shared by the two tools above so that they agree on the function set |
| `tools/sync_node_tests.py <node-checkout>` | vendors Node.js's `test/js-native-api` and `test/node-api` tests into `test/node`, applies the declared patches and generates `tests.cmake` from each `binding.gyp` (the build needs neither Python nor node-gyp) |
| `tools/sync_node_addon_api.py <node-addon-api checkout>` | vendors the node-addon-api headers and test suite into `test/node-addon-api`, applies the declared patches and generates `tests.cmake` |

The vendored test directories are **generated**: do not edit them by hand.
Every change goes through the sync scripts, whose `PATCHES` / `SKIP_*` tables
give the reason for each difference from upstream.

## Workflows

### Implementing or changing a Node-API function

1. Implement it in the `src/` file of its area, following the Node.js source
   (`src/js_native_api_v8.cc`, `src/node_api.cc`) for statuses and edge
   cases. Use the macros of `qjsnapi_internal.h` (`NAPI_PREAMBLE`,
   `CHECK_ARG`, `CHECK_JS`, ...) so the error behaviour matches.
2. Run `python3 tools/gen_stubs.py` to drop its stub; `stubs_up_to_date` fails
   otherwise.
3. Enable the matching Node.js test in `tools/sync_node_tests.py` (`TESTS`) and
   re-run it, or add a case to a `test/js` script and its addon.
4. Run the suite in Debug (leak assertion) and under `unix-asan`.
5. Update the reference page in [doc/node-api](node-api/README.md) and, for a
   deviation, [compatibility.md](compatibility.md).

### Adding a JS test

Write it so that it runs **unchanged under both runtimes** (see
`test/js/test_hello.js`): it receives the addon directory as an argument and
throws on the first failed expectation. Register it in `CMakeLists.txt`
next to the other `js_*` tests; the `node_parity_*` variant is added the same
way.

### Updating the vendored suites

```sh
git clone --depth 1 --branch v22.23.4 --filter=blob:none --sparse https://github.com/nodejs/node
git -C node sparse-checkout set --no-cone /test/js-native-api/ /test/node-api/ /test/common/gc.js /test/common/tick.js /LICENSE
python3 tools/sync_node_tests.py node

python3 tools/sync_node_addon_api.py <node-addon-api checkout>
```
