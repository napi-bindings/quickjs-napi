# Environment, versions and cleanup hooks

[Node-API reference](README.md) › Environment

Implemented in `src/env.c` and `src/loader.c`.

| Function | Purpose |
|---|---|
| [`napi_get_last_error_info`](#napi_get_last_error_info) | details of the last failed call |
| [`napi_get_version`](#napi_get_version) | highest supported Node-API version |
| [`napi_get_node_version`](#napi_get_node_version) | the Node.js version that is emulated |
| [`node_api_get_module_file_name`](#node_api_get_module_file_name) | URL of the addon file |
| [`napi_add_env_cleanup_hook`](#napi_add_env_cleanup_hook) | run a function at teardown |
| [`napi_remove_env_cleanup_hook`](#napi_remove_env_cleanup_hook) | unregister it |
| [`napi_add_async_cleanup_hook`](#napi_add_async_cleanup_hook) | asynchronous teardown step |
| [`napi_remove_async_cleanup_hook`](#napi_remove_async_cleanup_hook) | unregister it, or signal it is done |
| [`napi_fatal_error`](#napi_fatal_error) | abort the process |
| [`napi_module_register`](#napi_module_register) | legacy module registration |

---

### `napi_get_last_error_info`

```c
napi_status napi_get_last_error_info(node_api_basic_env env,
                                     const napi_extended_error_info **result);
```

Returns a pointer to the env's last-error record: `error_code` is the status of
the last Node-API call that failed (or `napi_ok`), `error_message` a static
English message (see the [status table](README.md#status-codes-and-the-last-error)).
`engine_error_code` and `engine_reserved` are always `0` / `NULL`.

- The record belongs to the env and is overwritten by the next call: copy what
  you need before calling another function.
- This function never changes the last error itself, so it can be called
  repeatedly. It always returns `napi_ok` (or `napi_invalid_arg` for a `NULL`
  argument).
- Each native callback starts with a clear last error, as in Node.js.

### `napi_get_version`

```c
napi_status napi_get_version(node_api_basic_env env, uint32_t *result);
```

Stores `10`, the highest Node-API version this build supports (the same value
as `qjsnapi_napi_version()`). Experimental APIs (`NAPI_EXPERIMENTAL`) are not
provided.

### `napi_get_node_version`

```c
napi_status napi_get_node_version(node_api_basic_env env,
                                  const napi_node_version **version);
```

Stores a pointer to a static record `{ major: 22, minor: 23, patch: 4,
release: "quickjs-napi" }`: the Node.js release whose Node-API behaviour is
reproduced. Addons that check for a minimum Node.js version therefore see
22.23.4. The executable's `process.version` reports the same.

### `node_api_get_module_file_name`

```c
napi_status node_api_get_module_file_name(node_api_basic_env env,
                                          const char **result);
```

Stores the absolute `file://` URL of the addon this env belongs to, encoded
like Node's `url.pathToFileURL` (percent-encoding, Windows drive letters as
`file:///C:/...`). The string lives as long as the env. For a static addon
(`qjsnapi_init_static_addon`) it is the name given by the embedder.

### `napi_add_env_cleanup_hook`

```c
napi_status napi_add_env_cleanup_hook(node_api_basic_env env,
                                      napi_cleanup_hook fun, void *arg);
```

Registers `fun(arg)` to run when the env is torn down
(`qjsnapi_host_destroy`, or the normal end of the executable). Hooks run in
**reverse order of registration** (LIFO), interleaved with the async cleanup
hooks and the library's own hooks (thread-safe functions), with JavaScript
already disabled.

- Registering the same `(fun, arg)` pair twice is a **fatal error**, as in
  Node.js (where it is a `CHECK` failure).
- Hooks do **not** run when the executable ends with `process.exit()` or a
  fatal exception, as in Node.js.

### `napi_remove_env_cleanup_hook`

```c
napi_status napi_remove_env_cleanup_hook(node_api_basic_env env,
                                         napi_cleanup_hook fun, void *arg);
```

Unregisters the hook registered with the same `(fun, arg)`. Removing a pair
that is not registered does nothing and returns `napi_ok`.

### `napi_add_async_cleanup_hook`

```c
napi_status napi_add_async_cleanup_hook(
    node_api_basic_env env, napi_async_cleanup_hook hook, void *arg,
    napi_async_cleanup_hook_handle *remove_handle);
```

Registers a cleanup step that may take time, such as closing a libuv handle.
At teardown `hook(handle, arg)` is called in LIFO order with the other hooks;
the teardown then **keeps running the event loop** until the addon calls
`napi_remove_async_cleanup_hook(handle)`, typically from a `uv_close`
callback.

`remove_handle` may be `NULL` when the hook is never removed early; the hook
then receives the handle as its first argument.

```c
static void on_close(uv_handle_t *h) {
  napi_remove_async_cleanup_hook(((my_state *)h->data)->cleanup_handle);
}
static void cleanup(napi_async_cleanup_hook_handle handle, void *arg) {
  my_state *s = arg;
  s->cleanup_handle = handle;
  uv_close((uv_handle_t *)&s->timer, on_close);
}
/* in init: */
napi_add_async_cleanup_hook(env, cleanup, state, NULL);
```

### `napi_remove_async_cleanup_hook`

```c
napi_status napi_remove_async_cleanup_hook(
    napi_async_cleanup_hook_handle remove_handle);
```

Has two meanings, as in Node.js:

- **before teardown**, it unregisters the hook, which will not run;
- **after the hook started**, it signals that the asynchronous cleanup is
  complete, so the teardown can stop waiting for it.

The handle is freed in both cases and must not be used again. Returns
`napi_invalid_arg` for `NULL`.

### `napi_fatal_error`

```c
NAPI_NO_RETURN void napi_fatal_error(const char *location, size_t location_len,
                                     const char *message, size_t message_len);
```

Prints `FATAL ERROR: <location> <message>` on stderr and ends the process
immediately: `abort()` on Linux and macOS, `_exit(134)` on Windows (like
Node.js, which avoids the Windows Error Reporting dialog). No cleanup runs.
Both lengths accept `NAPI_AUTO_LENGTH`; `location` and `message` may be
`NULL`.

### `napi_module_register`

```c
void napi_module_register(napi_module *mod);
```

Legacy registration used by addons built with the old `NAPI_MODULE` macro
from a static constructor. When called **while `qjsnapi_load_addon` is opening
a library**, the module is attached to that load and its `nm_register_func`
becomes the initializer (only when the library does not export
`napi_register_module_v1`, which takes precedence). The record is remembered,
so loading the same file again reuses it even though the constructor does not
run a second time.

Called outside of a load (for instance by an addon linked into the
executable), it prints a warning and does nothing: use
[`qjsnapi_init_static_addon`](../embedding.md#qjsnapi_init_static_addon)
instead. A legacy module with no `nm_register_func` fails to load with
`Module has no declared entry point.`
