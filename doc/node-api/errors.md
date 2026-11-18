# Errors and exceptions

[Node-API reference](README.md) › Errors

Implemented in `src/error.c`. The model (status codes plus a pending
JavaScript exception) is described in
[architecture.md](../architecture.md#errors-and-exceptions).

| Group | Functions |
|---|---|
| Create an error object | [`napi_create_error`](#napi_create_error), [`napi_create_type_error`](#napi_create_type_error), [`napi_create_range_error`](#napi_create_range_error), [`node_api_create_syntax_error`](#node_api_create_syntax_error) |
| Throw | [`napi_throw`](#napi_throw), [`napi_throw_error`](#napi_throw_error), [`napi_throw_type_error`](#napi_throw_type_error), [`napi_throw_range_error`](#napi_throw_range_error), [`node_api_throw_syntax_error`](#node_api_throw_syntax_error) |
| Inspect | [`napi_is_error`](#napi_is_error), [`napi_is_exception_pending`](#napi_is_exception_pending), [`napi_get_and_clear_last_exception`](#napi_get_and_clear_last_exception) |

## Pending exceptions

QuickJS keeps at most **one pending exception** on the runtime, and
quickjs-napi uses it directly:

- a failed call into JavaScript leaves the exception pending and returns
  `napi_pending_exception`;
- `napi_throw*` make an exception pending and return `napi_ok`;
- while an exception is pending, [functions that run JavaScript](README.md#functions-that-run-javascript)
  refuse to start, but value creation and the functions on this page still
  work;
- when the native callback returns, a pending exception propagates to the
  JavaScript caller. With no JavaScript caller (finalizers, async work,
  thread-safe functions) it is reported as uncaught.

A typical error path:

```c
napi_value result;
napi_status s = napi_call_function(env, recv, fn, 0, NULL, &result);
if (s == napi_pending_exception) {
  napi_value exc;
  napi_get_and_clear_last_exception(env, &exc); /* handle it here ... */
  /* ... or simply return NULL and let it propagate */
}
```

## Error objects

The four error constructors (`Error`, `TypeError`, `RangeError`,
`SyntaxError`) are captured when the host is created, so replacing
`globalThis.Error` later does not change what these functions create. Like
Node.js, they create `new Ctor(message)` and, when a code is given, set an
own enumerable `code` property after construction.

---

## Create an error object

These functions only create the object; nothing is thrown. They work while an
exception is pending.

### `napi_create_error`

```c
napi_status napi_create_error(napi_env env, napi_value code, napi_value msg,
                              napi_value *result);
```

Creates `new Error(msg)`. `msg` must be a string; `code` is optional (`NULL`)
or a string, set as `error.code`. Either one not being a string gives
`napi_string_expected`.

### `napi_create_type_error`

```c
napi_status napi_create_type_error(napi_env env, napi_value code,
                                   napi_value msg, napi_value *result);
```

Same with `TypeError`.

### `napi_create_range_error`

```c
napi_status napi_create_range_error(napi_env env, napi_value code,
                                    napi_value msg, napi_value *result);
```

Same with `RangeError`.

### `node_api_create_syntax_error`

```c
napi_status node_api_create_syntax_error(napi_env env, napi_value code,
                                         napi_value msg, napi_value *result);
```

Same with `SyntaxError`.

## Throw

### `napi_throw`

```c
napi_status napi_throw(napi_env env, napi_value error);
```

Throws `error`, which may be any value (not only an Error object). Returns
`napi_ok`; the exception is now pending.

### `napi_throw_error`

```c
napi_status napi_throw_error(napi_env env, const char *code, const char *msg);
```

Creates and throws `new Error(msg)` with an optional `code` (`NULL` for none).
`msg` is a NUL-terminated UTF-8 string and is required.

### `napi_throw_type_error`

```c
napi_status napi_throw_type_error(napi_env env, const char *code,
                                  const char *msg);
```

Same with `TypeError`.

### `napi_throw_range_error`

```c
napi_status napi_throw_range_error(napi_env env, const char *code,
                                   const char *msg);
```

Same with `RangeError`.

### `node_api_throw_syntax_error`

```c
napi_status node_api_throw_syntax_error(napi_env env, const char *code,
                                        const char *msg);
```

Same with `SyntaxError`.

All `napi_throw*` functions refuse to start (`napi_pending_exception`) when an
exception is already pending: an exception never silently replaces another.

## Inspect

### `napi_is_error`

```c
napi_status napi_is_error(napi_env env, napi_value value, bool *result);
```

`true` when `value` is an Error object: an object created by a builtin error
constructor or a subclass of one (QuickJS `JS_IsError`). An ordinary object
whose prototype chain contains `Error.prototype` is not an Error.

### `napi_is_exception_pending`

```c
napi_status napi_is_exception_pending(napi_env env, bool *result);
```

`true` when an exception is pending on the context.

### `napi_get_and_clear_last_exception`

```c
napi_status napi_get_and_clear_last_exception(napi_env env,
                                              napi_value *result);
```

Takes the pending exception: stores it in `result` and clears it, so
JavaScript can run again. When no exception is pending, stores `undefined`.
