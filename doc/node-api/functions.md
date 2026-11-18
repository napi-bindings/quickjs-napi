# Functions, classes and scripts

[Node-API reference](README.md) › Functions

Implemented in `src/function.c`. How a call reaches the addon is described in
[architecture.md](../architecture.md#calling-native-functions).

| Function | Purpose |
|---|---|
| [`napi_create_function`](#napi_create_function) | wrap a C callback in a JavaScript function |
| [`napi_get_cb_info`](#napi_get_cb_info) | arguments, `this` and data of the current call |
| [`napi_get_new_target`](#napi_get_new_target) | `new.target` of the current call |
| [`napi_call_function`](#napi_call_function) | call a JavaScript function |
| [`napi_new_instance`](#napi_new_instance) | `new Constructor(...)` |
| [`napi_define_class`](#napi_define_class) | constructor with prototype and static members |
| [`napi_run_script`](#napi_run_script) | evaluate a string of JavaScript |

## The callback

```c
typedef napi_value (*napi_callback)(napi_env env, napi_callback_info info);
```

A native function receives the env and an opaque `info`, valid only during
the call. It returns the result, or `NULL` for `undefined`. If it leaves an
exception pending (`napi_throw*`, or a failed call into JavaScript), the
returned value is ignored and the exception propagates to the caller.

Every call runs in its own handle scope: values created during the call are
released when it returns, except the returned one. The last error is cleared
before the callback runs.

---

### `napi_create_function`

```c
napi_status napi_create_function(napi_env env, const char *utf8name,
                                 size_t length, napi_callback cb, void *data,
                                 napi_value *result);
```

Creates a JavaScript function that calls `cb`. `data` is handed back by
`napi_get_cb_info`.

- `utf8name` (optional; `NULL` gives `""`) becomes the `name` property;
  `length` is its length in bytes or `NAPI_AUTO_LENGTH`.
- Like a V8 function created by `Function::New`: own properties `length` (0)
  and `name`, both non-writable, non-enumerable and configurable, in that
  order; and an own `prototype` object whose `constructor` points back to the
  function, so the function can also be called with `new`.
- The function holds `data` as a plain pointer. To free it when the function is
  collected, attach a finalizer with
  [`napi_add_finalizer`](lifetime.md#napi_add_finalizer).

### `napi_get_cb_info`

```c
napi_status napi_get_cb_info(napi_env env, napi_callback_info cbinfo,
                             size_t *argc, napi_value *argv,
                             napi_value *this_arg, void **data);
```

Reads the current call. Every output is optional:

- `argc` is in/out: on input the capacity of `argv`, on output the **actual**
  number of arguments, which may be larger than the capacity.
- `argv` receives `min(capacity, actual)` arguments; the remaining slots up to
  the capacity are filled with `undefined`. `argc` is required when `argv` is
  given.
- `this_arg` receives the receiver. In a construct call this is the new object
  created from `new.target.prototype`.
- `data` receives the pointer given to `napi_create_function`,
  `napi_define_class` or the property descriptor.

```c
size_t argc = 3;
napi_value argv[3];
napi_value self;
napi_get_cb_info(env, info, &argc, argv, &self, NULL);
/* argc may now be 0..N; argv[argc..2] are undefined */
```

### `napi_get_new_target`

```c
napi_status napi_get_new_target(napi_env env, napi_callback_info cbinfo,
                                napi_value *result);
```

Stores `new.target`: the constructor `new` was applied to (a subclass when
called through `super()`), or `NULL` when the function was called without
`new`. This is how a constructor detects a plain call.

### `napi_call_function`

```c
napi_status napi_call_function(napi_env env, napi_value recv, napi_value func,
                               size_t argc, const napi_value *argv,
                               napi_value *result);
```

Calls `func` with `this = recv` and `argc` arguments. `recv` is required (use
`napi_get_undefined` or `napi_get_global`). `result` is optional.

- `func` not a function: `napi_invalid_arg`, as in Node.js.
- The function throws: `napi_pending_exception`, exception pending.
- Up to 8 arguments are passed without allocation.

### `napi_new_instance`

```c
napi_status napi_new_instance(napi_env env, napi_value constructor,
                              size_t argc, const napi_value *argv,
                              napi_value *result);
```

`new constructor(...argv)`. `napi_invalid_arg` when `constructor` is not a
function; `napi_pending_exception` when it is not a constructor (an arrow
function, a method) or throws.

### `napi_define_class`

```c
napi_status napi_define_class(napi_env env, const char *utf8name,
                              size_t length, napi_callback constructor,
                              void *data, size_t property_count,
                              const napi_property_descriptor *properties,
                              napi_value *result);
```

Creates a constructor function named `utf8name` (required) that calls
`constructor`, then defines the properties:

- descriptors **without** `napi_static` go on `prototype` (instance methods,
  accessors and values);
- descriptors **with** `napi_static` go on the constructor itself.

Descriptors follow [`napi_define_properties`](objects.md#napi_define_properties).
As with V8's prototype templates, when two instance descriptors have the same
key **the later one wins**, whatever the attributes of the earlier one
(node-addon-api relies on it). Static descriptors are defined one by one, so
redefining a non-configurable static property fails, as in Node.js.

The usual pattern pairs the class with [`napi_wrap`](lifetime.md#napi_wrap):

```c
static napi_value Counter_New(napi_env env, napi_callback_info info) {
  napi_value self, target;
  napi_get_new_target(env, info, &target);
  if (target == NULL) {
    napi_throw_type_error(env, NULL, "Counter must be called with new");
    return NULL;
  }
  napi_get_cb_info(env, info, NULL, NULL, &self, NULL);
  int64_t *count = calloc(1, sizeof *count);
  napi_wrap(env, self, count, free_counter, NULL, NULL);
  return self;
}

static napi_value Counter_Increment(napi_env env, napi_callback_info info) {
  napi_value self, result;
  int64_t *count;
  napi_get_cb_info(env, info, NULL, NULL, &self, NULL);
  if (napi_unwrap(env, self, (void **)&count) != napi_ok) {
    napi_throw_type_error(env, NULL, "not a Counter");
    return NULL;
  }
  napi_create_int64(env, ++*count, &result);
  return result;
}

napi_property_descriptor props[] = {
  {"increment", NULL, Counter_Increment, NULL, NULL, NULL, napi_default_method, NULL},
};
napi_value cls;
napi_define_class(env, "Counter", NAPI_AUTO_LENGTH, Counter_New, NULL, 1,
                  props, &cls);
```

**Difference from Node.js:** V8 attaches a *signature* to prototype methods,
so calling one on an object that was not created by the class throws
`TypeError: Illegal invocation` before the callback runs. In quickjs-napi the
callback runs and must validate `this` itself, as `Counter_Increment` does
above (`napi_unwrap` fails on objects that are not wrapped). See
[compatibility.md](../compatibility.md).

### `napi_run_script`

```c
napi_status napi_run_script(napi_env env, napi_value script,
                            napi_value *result);
```

Compiles and runs `script` (a string) as a classic global script in the
current context, like `v8::Script::Compile` + `Run`, and stores its
completion value. The file name in stack traces is `<anonymous>`.

- `script` not a string: `napi_string_expected`.
- A syntax error or an exception thrown by the script: `napi_generic_failure`,
  **with the exception pending**, as in Node.js.
