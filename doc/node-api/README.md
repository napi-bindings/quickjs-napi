# Node-API reference

quickjs-napi implements all **155 functions** of Node-API version 10
(`include/js_native_api.h` and `include/node_api.h`, the headers of Node.js
22). This reference documents what each function does **in quickjs-napi**:
its contract, the statuses it returns, and where its behaviour depends on the
QuickJS implementation. The upstream
[Node-API documentation](https://nodejs.org/docs/latest-v22.x/api/n-api.html)
remains the normative text; differences from Node.js are collected in
[compatibility.md](../compatibility.md).

- [Conventions](#conventions)
- [Writing an addon](#writing-an-addon)
- [Function index](#function-index)

| Area | File | Functions |
|---|---|---|
| Environment, versions, cleanup hooks, module registration | [environment.md](environment.md) | 10 |
| Primitives, numbers, booleans, coercion, arrays | [values.md](values.md) | 24 |
| Strings, property keys, symbols | [strings.md](strings.md) | 13 |
| Objects and properties | [objects.md](objects.md) | 19 |
| Functions, calls, classes, scripts | [functions.md](functions.md) | 7 |
| Errors and exceptions | [errors.md](errors.md) | 12 |
| Handle scopes | [scopes.md](scopes.md) | 5 |
| References, externals, wrap, finalizers, type tags, instance data | [lifetime.md](lifetime.md) | 16 |
| ArrayBuffer, TypedArray, DataView, BigInt, Date, Promise | [binary-data.md](binary-data.md) | 25 |
| Buffer, async context, `napi_make_callback`, callback scopes | [node-specific.md](node-specific.md) | 12 |
| Async work, thread-safe functions, the event loop | [async.md](async.md) | 12 |

## Conventions

### Status codes and the last error

Every function returns a `napi_status`. `napi_ok` means success; anything else
is a failure and is also recorded as the env's last error, which
`napi_get_last_error_info` returns with a message:

| Status | Message | Typical cause |
|---|---|---|
| `napi_ok` | | success |
| `napi_invalid_arg` | Invalid argument | a `NULL` pointer argument, an out-of-range enum, a value of the wrong kind where no specific status exists |
| `napi_object_expected` | An object was expected | the receiver is `null`/`undefined` (a `TypeError` is then pending), or not an object |
| `napi_string_expected` | A string was expected | |
| `napi_name_expected` | A string or symbol was expected | property keys, symbol descriptions |
| `napi_function_expected` | A function was expected | |
| `napi_number_expected` | A number was expected | getters never coerce |
| `napi_boolean_expected` | A boolean was expected | |
| `napi_array_expected` | An array was expected | |
| `napi_generic_failure` | Unknown failure | out of memory, or a failed engine operation without a JS exception |
| `napi_pending_exception` | An exception is pending | JavaScript threw during the call, or an exception was already pending |
| `napi_cancelled` | The async work item was cancelled | passed to `complete` |
| `napi_escape_called_twice` | napi_escape_handle already called on scope | |
| `napi_handle_scope_mismatch` | Invalid handle scope usage | closing a scope that is not the innermost one |
| `napi_callback_scope_mismatch` | Invalid callback scope usage | |
| `napi_queue_full` | Thread-safe function queue is full | non-blocking call on a full queue |
| `napi_closing` | Thread-safe function handle is closing | |
| `napi_bigint_expected` | A bigint was expected | |
| `napi_date_expected` | A date was expected | |
| `napi_arraybuffer_expected` | An arraybuffer was expected | |
| `napi_detachable_arraybuffer_expected` | A detachable arraybuffer was expected | |
| `napi_would_deadlock` | Main thread would deadlock | never returned (as in Node.js 22) |
| `napi_no_external_buffers_allowed` | External buffers are not allowed | never returned: external buffers are always allowed |
| `napi_cannot_run_js` | Cannot run JavaScript | the env is tearing down (modules built for Node-API ≥ 10) |

### Functions that run JavaScript

Some functions may call into JavaScript (getters, setters, `valueOf`,
proxies, user callbacks). These **refuse to start** when an exception is
already pending (`napi_pending_exception`) or when the env can no longer run
JavaScript (`napi_cannot_run_js`, or `napi_pending_exception` for modules
built for Node-API < 10). They are marked **JS** in the
[function index](#function-index). The other functions only touch C
bookkeeping or create values, and work even with an exception pending: this
is what lets an addon build an error message while an exception is pending.

When JavaScript throws during a call, the function returns
`napi_pending_exception` and **the exception stays pending**, exactly as in
Node.js. Returning from the native callback then propagates it to the
JavaScript caller. Use `napi_is_exception_pending` and
`napi_get_and_clear_last_exception` to handle it in C.

### Basic env

Functions whose first parameter is `node_api_basic_env` (cleanup hooks,
instance data, `napi_delete_reference`, `napi_adjust_external_memory`,
`napi_get_uv_event_loop`, ...) may be called from a *basic finalizer*
(`node_api_basic_finalize`). In quickjs-napi every finalizer, basic or not,
runs **outside the garbage collector** (from the event loop or at teardown),
so calling other functions from a basic finalizer never causes the fatal
error Node.js raises; the distinction is kept for source compatibility.

### Values and threads

- A `napi_value` is valid until the handle scope it was created in is closed.
  Values created in a native callback are released when the callback returns,
  unless they are returned, escaped, or kept in a `napi_ref`.
- Every function except the thread-safe function ones
  (`napi_call_threadsafe_function`, `napi_acquire_threadsafe_function`,
  `napi_release_threadsafe_function`,
  `napi_get_threadsafe_function_context`) must be called on the thread that
  runs the event loop. `execute` callbacks of async work run on the thread
  pool and must not call Node-API functions that take an env.

## Writing an addon

Addons are written exactly as for Node.js. A minimal C addon:

```c
#include <node_api.h>

#define NAPI_CALL(env, call)                                            \
  do {                                                                  \
    if ((call) != napi_ok) {                                            \
      const napi_extended_error_info *info;                             \
      bool pending;                                                     \
      napi_get_last_error_info((env), &info);                           \
      napi_is_exception_pending((env), &pending);                       \
      if (!pending)                                                     \
        napi_throw_error((env), NULL, info->error_message               \
                                          ? info->error_message         \
                                          : "Node-API call failed");    \
      return NULL;                                                      \
    }                                                                   \
  } while (0)

static napi_value Add(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  double a, b;
  NAPI_CALL(env, napi_get_value_double(env, argv[0], &a));
  NAPI_CALL(env, napi_get_value_double(env, argv[1], &b));
  napi_value sum;
  NAPI_CALL(env, napi_create_double(env, a + b, &sum));
  return sum;
}

NAPI_MODULE_INIT(/* napi_env env, napi_value exports */) {
  napi_property_descriptor desc = {"add", NULL, Add, NULL, NULL, NULL,
                                   napi_default, NULL};
  NAPI_CALL(env, napi_define_properties(env, exports, 1, &desc));
  return exports;
}
```

Build it with node-gyp, cmake-js or any toolchain that produces a Node.js
addon, then:

```sh
quickjs-napi -e "console.log(require('./build/Release/addon.node').add(2, 3))"
```

The same binary loads in Node.js. node-addon-api (C++) and napi-rs (Rust)
addons work the same way; the node-addon-api 8.9.2 test suite runs in CI
([testing.md](../testing.md)).

The module API version is chosen by the addon at build time (`NAPI_VERSION`,
exported through `node_api_module_get_api_version_v1`). It is 8 when the
addon does not declare one, and changes a few behaviours: which values
`napi_create_reference` accepts, the status of
[functions that run JavaScript](#functions-that-run-javascript) during
teardown, and the [thread-safe function exception policy](async.md#exceptions-in-call_js_cb).

## Function index

**JS**: the function may run JavaScript and refuses to start with an exception
pending (see [above](#functions-that-run-javascript)).

| Function | | Reference |
|---|---|---|
| `napi_acquire_threadsafe_function` | | [async.md](async.md#napi_acquire_threadsafe_function) |
| `napi_add_async_cleanup_hook` | | [environment.md](environment.md#napi_add_async_cleanup_hook) |
| `napi_add_env_cleanup_hook` | | [environment.md](environment.md#napi_add_env_cleanup_hook) |
| `napi_add_finalizer` | | [lifetime.md](lifetime.md#napi_add_finalizer) |
| `napi_adjust_external_memory` | | [lifetime.md](lifetime.md#napi_adjust_external_memory) |
| `napi_async_destroy` | | [node-specific.md](node-specific.md#napi_async_destroy) |
| `napi_async_init` | | [node-specific.md](node-specific.md#napi_async_init) |
| `napi_call_function` | JS | [functions.md](functions.md#napi_call_function) |
| `napi_call_threadsafe_function` | | [async.md](async.md#napi_call_threadsafe_function) |
| `napi_cancel_async_work` | | [async.md](async.md#napi_cancel_async_work) |
| `napi_check_object_type_tag` | JS | [lifetime.md](lifetime.md#napi_check_object_type_tag) |
| `napi_close_callback_scope` | | [node-specific.md](node-specific.md#napi_close_callback_scope) |
| `napi_close_escapable_handle_scope` | | [scopes.md](scopes.md#napi_close_escapable_handle_scope) |
| `napi_close_handle_scope` | | [scopes.md](scopes.md#napi_close_handle_scope) |
| `napi_coerce_to_bool` | JS | [values.md](values.md#napi_coerce_to_bool) |
| `napi_coerce_to_number` | JS | [values.md](values.md#napi_coerce_to_number) |
| `napi_coerce_to_object` | JS | [values.md](values.md#napi_coerce_to_object) |
| `napi_coerce_to_string` | JS | [values.md](values.md#napi_coerce_to_string) |
| `napi_create_array` | | [values.md](values.md#napi_create_array) |
| `napi_create_array_with_length` | | [values.md](values.md#napi_create_array_with_length) |
| `napi_create_arraybuffer` | JS | [binary-data.md](binary-data.md#napi_create_arraybuffer) |
| `napi_create_async_work` | | [async.md](async.md#napi_create_async_work) |
| `napi_create_bigint_int64` | | [binary-data.md](binary-data.md#napi_create_bigint_int64) |
| `napi_create_bigint_uint64` | | [binary-data.md](binary-data.md#napi_create_bigint_uint64) |
| `napi_create_bigint_words` | JS | [binary-data.md](binary-data.md#napi_create_bigint_words) |
| `napi_create_buffer` | JS | [node-specific.md](node-specific.md#napi_create_buffer) |
| `napi_create_buffer_copy` | JS | [node-specific.md](node-specific.md#napi_create_buffer_copy) |
| `napi_create_dataview` | JS | [binary-data.md](binary-data.md#napi_create_dataview) |
| `napi_create_date` | JS | [binary-data.md](binary-data.md#napi_create_date) |
| `napi_create_double` | | [values.md](values.md#napi_create_double) |
| `napi_create_error` | | [errors.md](errors.md#napi_create_error) |
| `napi_create_external` | JS | [lifetime.md](lifetime.md#napi_create_external) |
| `napi_create_external_arraybuffer` | JS | [binary-data.md](binary-data.md#napi_create_external_arraybuffer) |
| `napi_create_external_buffer` | JS | [node-specific.md](node-specific.md#napi_create_external_buffer) |
| `napi_create_function` | JS | [functions.md](functions.md#napi_create_function) |
| `napi_create_int32` | | [values.md](values.md#napi_create_int32) |
| `napi_create_int64` | | [values.md](values.md#napi_create_int64) |
| `napi_create_object` | | [values.md](values.md#napi_create_object) |
| `napi_create_promise` | JS | [binary-data.md](binary-data.md#napi_create_promise) |
| `napi_create_range_error` | | [errors.md](errors.md#napi_create_range_error) |
| `napi_create_reference` | | [lifetime.md](lifetime.md#napi_create_reference) |
| `napi_create_string_latin1` | | [strings.md](strings.md#napi_create_string_latin1) |
| `napi_create_string_utf16` | | [strings.md](strings.md#napi_create_string_utf16) |
| `napi_create_string_utf8` | | [strings.md](strings.md#napi_create_string_utf8) |
| `napi_create_symbol` | | [strings.md](strings.md#napi_create_symbol) |
| `napi_create_threadsafe_function` | | [async.md](async.md#napi_create_threadsafe_function) |
| `napi_create_type_error` | | [errors.md](errors.md#napi_create_type_error) |
| `napi_create_typedarray` | JS | [binary-data.md](binary-data.md#napi_create_typedarray) |
| `napi_create_uint32` | | [values.md](values.md#napi_create_uint32) |
| `napi_define_class` | JS | [functions.md](functions.md#napi_define_class) |
| `napi_define_properties` | JS | [objects.md](objects.md#napi_define_properties) |
| `napi_delete_async_work` | | [async.md](async.md#napi_delete_async_work) |
| `napi_delete_element` | JS | [objects.md](objects.md#napi_delete_element) |
| `napi_delete_property` | JS | [objects.md](objects.md#napi_delete_property) |
| `napi_delete_reference` | | [lifetime.md](lifetime.md#napi_delete_reference) |
| `napi_detach_arraybuffer` | | [binary-data.md](binary-data.md#napi_detach_arraybuffer) |
| `napi_escape_handle` | | [scopes.md](scopes.md#napi_escape_handle) |
| `napi_fatal_error` | | [environment.md](environment.md#napi_fatal_error) |
| `napi_fatal_exception` | JS | [node-specific.md](node-specific.md#napi_fatal_exception) |
| `napi_get_all_property_names` | JS | [objects.md](objects.md#napi_get_all_property_names) |
| `napi_get_and_clear_last_exception` | | [errors.md](errors.md#napi_get_and_clear_last_exception) |
| `napi_get_array_length` | JS | [values.md](values.md#napi_get_array_length) |
| `napi_get_arraybuffer_info` | | [binary-data.md](binary-data.md#napi_get_arraybuffer_info) |
| `napi_get_boolean` | | [values.md](values.md#napi_get_boolean) |
| `napi_get_buffer_info` | | [node-specific.md](node-specific.md#napi_get_buffer_info) |
| `napi_get_cb_info` | | [functions.md](functions.md#napi_get_cb_info) |
| `napi_get_dataview_info` | | [binary-data.md](binary-data.md#napi_get_dataview_info) |
| `napi_get_date_value` | JS | [binary-data.md](binary-data.md#napi_get_date_value) |
| `napi_get_element` | JS | [objects.md](objects.md#napi_get_element) |
| `napi_get_global` | | [values.md](values.md#napi_get_global) |
| `napi_get_instance_data` | | [lifetime.md](lifetime.md#napi_get_instance_data) |
| `napi_get_last_error_info` | | [environment.md](environment.md#napi_get_last_error_info) |
| `napi_get_named_property` | JS | [objects.md](objects.md#napi_get_named_property) |
| `napi_get_new_target` | | [functions.md](functions.md#napi_get_new_target) |
| `napi_get_node_version` | | [environment.md](environment.md#napi_get_node_version) |
| `napi_get_null` | | [values.md](values.md#napi_get_null) |
| `napi_get_property` | JS | [objects.md](objects.md#napi_get_property) |
| `napi_get_property_names` | JS | [objects.md](objects.md#napi_get_property_names) |
| `napi_get_prototype` | JS | [objects.md](objects.md#napi_get_prototype) |
| `napi_get_reference_value` | | [lifetime.md](lifetime.md#napi_get_reference_value) |
| `napi_get_threadsafe_function_context` | | [async.md](async.md#napi_get_threadsafe_function_context) |
| `napi_get_typedarray_info` | | [binary-data.md](binary-data.md#napi_get_typedarray_info) |
| `napi_get_undefined` | | [values.md](values.md#napi_get_undefined) |
| `napi_get_uv_event_loop` | | [async.md](async.md#napi_get_uv_event_loop) |
| `napi_get_value_bigint_int64` | | [binary-data.md](binary-data.md#napi_get_value_bigint_int64) |
| `napi_get_value_bigint_uint64` | | [binary-data.md](binary-data.md#napi_get_value_bigint_uint64) |
| `napi_get_value_bigint_words` | | [binary-data.md](binary-data.md#napi_get_value_bigint_words) |
| `napi_get_value_bool` | | [values.md](values.md#napi_get_value_bool) |
| `napi_get_value_double` | | [values.md](values.md#napi_get_value_double) |
| `napi_get_value_external` | | [lifetime.md](lifetime.md#napi_get_value_external) |
| `napi_get_value_int32` | | [values.md](values.md#napi_get_value_int32) |
| `napi_get_value_int64` | | [values.md](values.md#napi_get_value_int64) |
| `napi_get_value_string_latin1` | | [strings.md](strings.md#napi_get_value_string_latin1) |
| `napi_get_value_string_utf16` | | [strings.md](strings.md#napi_get_value_string_utf16) |
| `napi_get_value_string_utf8` | | [strings.md](strings.md#napi_get_value_string_utf8) |
| `napi_get_value_uint32` | | [values.md](values.md#napi_get_value_uint32) |
| `napi_get_version` | | [environment.md](environment.md#napi_get_version) |
| `napi_has_element` | JS | [objects.md](objects.md#napi_has_element) |
| `napi_has_named_property` | JS | [objects.md](objects.md#napi_has_named_property) |
| `napi_has_own_property` | JS | [objects.md](objects.md#napi_has_own_property) |
| `napi_has_property` | JS | [objects.md](objects.md#napi_has_property) |
| `napi_instanceof` | JS | [objects.md](objects.md#napi_instanceof) |
| `napi_is_array` | | [values.md](values.md#napi_is_array) |
| `napi_is_arraybuffer` | | [binary-data.md](binary-data.md#napi_is_arraybuffer) |
| `napi_is_buffer` | | [node-specific.md](node-specific.md#napi_is_buffer) |
| `napi_is_dataview` | | [binary-data.md](binary-data.md#napi_is_dataview) |
| `napi_is_date` | | [binary-data.md](binary-data.md#napi_is_date) |
| `napi_is_detached_arraybuffer` | | [binary-data.md](binary-data.md#napi_is_detached_arraybuffer) |
| `napi_is_error` | | [errors.md](errors.md#napi_is_error) |
| `napi_is_exception_pending` | | [errors.md](errors.md#napi_is_exception_pending) |
| `napi_is_promise` | | [binary-data.md](binary-data.md#napi_is_promise) |
| `napi_is_typedarray` | | [binary-data.md](binary-data.md#napi_is_typedarray) |
| `napi_make_callback` | JS | [node-specific.md](node-specific.md#napi_make_callback) |
| `napi_module_register` | | [environment.md](environment.md#napi_module_register) |
| `napi_new_instance` | JS | [functions.md](functions.md#napi_new_instance) |
| `napi_object_freeze` | JS | [objects.md](objects.md#napi_object_freeze) |
| `napi_object_seal` | JS | [objects.md](objects.md#napi_object_seal) |
| `napi_open_callback_scope` | | [node-specific.md](node-specific.md#napi_open_callback_scope) |
| `napi_open_escapable_handle_scope` | | [scopes.md](scopes.md#napi_open_escapable_handle_scope) |
| `napi_open_handle_scope` | | [scopes.md](scopes.md#napi_open_handle_scope) |
| `napi_queue_async_work` | | [async.md](async.md#napi_queue_async_work) |
| `napi_ref_threadsafe_function` | | [async.md](async.md#napi_ref_threadsafe_function) |
| `napi_reference_ref` | | [lifetime.md](lifetime.md#napi_reference_ref) |
| `napi_reference_unref` | | [lifetime.md](lifetime.md#napi_reference_unref) |
| `napi_reject_deferred` | JS | [binary-data.md](binary-data.md#napi_reject_deferred) |
| `napi_release_threadsafe_function` | | [async.md](async.md#napi_release_threadsafe_function) |
| `napi_remove_async_cleanup_hook` | | [environment.md](environment.md#napi_remove_async_cleanup_hook) |
| `napi_remove_env_cleanup_hook` | | [environment.md](environment.md#napi_remove_env_cleanup_hook) |
| `napi_remove_wrap` | JS | [lifetime.md](lifetime.md#napi_remove_wrap) |
| `napi_resolve_deferred` | JS | [binary-data.md](binary-data.md#napi_resolve_deferred) |
| `napi_run_script` | JS | [functions.md](functions.md#napi_run_script) |
| `napi_set_element` | JS | [objects.md](objects.md#napi_set_element) |
| `napi_set_instance_data` | | [lifetime.md](lifetime.md#napi_set_instance_data) |
| `napi_set_named_property` | JS | [objects.md](objects.md#napi_set_named_property) |
| `napi_set_property` | JS | [objects.md](objects.md#napi_set_property) |
| `napi_strict_equals` | JS | [values.md](values.md#napi_strict_equals) |
| `napi_throw` | JS | [errors.md](errors.md#napi_throw) |
| `napi_throw_error` | JS | [errors.md](errors.md#napi_throw_error) |
| `napi_throw_range_error` | JS | [errors.md](errors.md#napi_throw_range_error) |
| `napi_throw_type_error` | JS | [errors.md](errors.md#napi_throw_type_error) |
| `napi_type_tag_object` | JS | [lifetime.md](lifetime.md#napi_type_tag_object) |
| `napi_typeof` | | [values.md](values.md#napi_typeof) |
| `napi_unref_threadsafe_function` | | [async.md](async.md#napi_unref_threadsafe_function) |
| `napi_unwrap` | JS | [lifetime.md](lifetime.md#napi_unwrap) |
| `napi_wrap` | JS | [lifetime.md](lifetime.md#napi_wrap) |
| `node_api_create_buffer_from_arraybuffer` | JS | [node-specific.md](node-specific.md#node_api_create_buffer_from_arraybuffer) |
| `node_api_create_external_string_latin1` | | [strings.md](strings.md#node_api_create_external_string_latin1) |
| `node_api_create_external_string_utf16` | | [strings.md](strings.md#node_api_create_external_string_utf16) |
| `node_api_create_property_key_latin1` | | [strings.md](strings.md#node_api_create_property_key_latin1) |
| `node_api_create_property_key_utf16` | | [strings.md](strings.md#node_api_create_property_key_utf16) |
| `node_api_create_property_key_utf8` | | [strings.md](strings.md#node_api_create_property_key_utf8) |
| `node_api_create_syntax_error` | | [errors.md](errors.md#node_api_create_syntax_error) |
| `node_api_get_module_file_name` | | [environment.md](environment.md#node_api_get_module_file_name) |
| `node_api_symbol_for` | | [strings.md](strings.md#node_api_symbol_for) |
| `node_api_throw_syntax_error` | JS | [errors.md](errors.md#node_api_throw_syntax_error) |
