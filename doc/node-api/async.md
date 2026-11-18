# Async work, thread-safe functions and the event loop

[Node-API reference](README.md) › Asynchronous APIs

Implemented in `src/async.c`, `src/tsfn.c` and `src/loop.c`, on libuv 1.51.
How these objects sit on the loop, and how teardown waits for them, is
described in [architecture.md](../architecture.md#the-event-loop).

| Group | Functions |
|---|---|
| Async work | [`napi_create_async_work`](#napi_create_async_work), [`napi_queue_async_work`](#napi_queue_async_work), [`napi_cancel_async_work`](#napi_cancel_async_work), [`napi_delete_async_work`](#napi_delete_async_work) |
| Thread-safe functions | [`napi_create_threadsafe_function`](#napi_create_threadsafe_function), [`napi_call_threadsafe_function`](#napi_call_threadsafe_function), [`napi_acquire_threadsafe_function`](#napi_acquire_threadsafe_function), [`napi_release_threadsafe_function`](#napi_release_threadsafe_function), [`napi_get_threadsafe_function_context`](#napi_get_threadsafe_function_context), [`napi_ref_threadsafe_function`](#napi_ref_threadsafe_function), [`napi_unref_threadsafe_function`](#napi_unref_threadsafe_function) |
| Event loop | [`napi_get_uv_event_loop`](#napi_get_uv_event_loop) |

## Threads

| Code | Runs on |
|---|---|
| `execute` of async work | a libuv thread-pool thread (`UV_THREADPOOL_SIZE` threads, 4 by default). Must not call Node-API functions that take an env. |
| `complete` of async work | the loop thread |
| `napi_call_threadsafe_function`, `napi_acquire_threadsafe_function`, `napi_release_threadsafe_function`, `napi_get_threadsafe_function_context` | any thread |
| `call_js_cb`, `thread_finalize_cb` of a thread-safe function | the loop thread |
| everything else | the loop thread |

Every callback the loop thread runs (`complete`, `call_js_cb`,
`thread_finalize_cb`) gets a handle scope and a callback scope; on return,
a pending exception is reported as uncaught and the checkpoint runs
(microtasks, `process.nextTick` in the executable).

---

## Async work

Runs a C function on the thread pool, then a completion on the loop thread.

```c
typedef struct { napi_async_work work; napi_deferred deferred; double input, output; } job;

static void Execute(napi_env env, void *data) {     /* thread pool */
  job *j = data;
  j->output = expensive(j->input);                  /* no Node-API here */
}

static void Complete(napi_env env, napi_status status, void *data) { /* loop */
  job *j = data;
  napi_value v;
  if (status == napi_ok) {
    napi_create_double(env, j->output, &v);
    napi_resolve_deferred(env, j->deferred, v);
  } else {                                           /* napi_cancelled */
    napi_get_undefined(env, &v);
    napi_reject_deferred(env, j->deferred, v);
  }
  napi_delete_async_work(env, j->work);
  free(j);
}

static napi_value Compute(napi_env env, napi_callback_info info) {
  job *j = calloc(1, sizeof *j);
  napi_value name, promise;
  /* ... read the input into j->input ... */
  napi_create_string_utf8(env, "compute", NAPI_AUTO_LENGTH, &name);
  napi_create_promise(env, &j->deferred, &promise);
  napi_create_async_work(env, NULL, name, Execute, Complete, j, &j->work);
  napi_queue_async_work(env, j->work);
  return promise;
}
```

### `napi_create_async_work`

```c
napi_status napi_create_async_work(napi_env env, napi_value async_resource,
                                   napi_value async_resource_name,
                                   napi_async_execute_callback execute,
                                   napi_async_complete_callback complete,
                                   void *data, napi_async_work *result);
```

Creates a work item; nothing runs until it is queued. `execute` is required,
`complete` is optional. `async_resource` (optional; `null`/`undefined` give
`napi_object_expected`) and `async_resource_name` (required; a symbol gives
`napi_string_expected`) are only validated: there is no `async_hooks`.

The item belongs to the addon until `napi_delete_async_work`. Items never
deleted are freed at teardown.

### `napi_queue_async_work`

```c
napi_status napi_queue_async_work(node_api_basic_env env,
                                  napi_async_work work);
```

Queues the item (`uv_queue_work`): `execute(env, data)` runs on a pool thread,
then `complete(env, status, data)` on the loop thread with `napi_ok`. The
queued item keeps the loop alive.

- Queuing an item that is already queued, or queuing while the host is being
  destroyed, returns `napi_generic_failure`.
- The item can be queued again once `complete` has started, including from
  `complete` itself.

### `napi_cancel_async_work`

```c
napi_status napi_cancel_async_work(node_api_basic_env env,
                                   napi_async_work work);
```

Cancels a queued item whose `execute` has not started (`uv_cancel`).
`complete` then still runs, with status `napi_cancelled`. When `execute` is
already running or done, cancellation fails with `napi_generic_failure`.

### `napi_delete_async_work`

```c
napi_status napi_delete_async_work(napi_env env, napi_async_work work);
```

Frees the item. Usually called from `complete`. Deleting an item that is
still queued is undefined behaviour in Node.js; here the item is only marked,
and freed after its `complete` has run.

At teardown, queued items are cancelled, in-flight ones are waited for, and
their `complete` runs with JavaScript disabled, so the addon can still free
its data.

## Thread-safe functions

A thread-safe function lets any thread schedule a call to JavaScript on the
loop thread. It is a port of Node 22's implementation: a queue protected by a
mutex, a `uv_async_t` to wake the loop thread, and up to 1000 calls dispatched
per wake-up. Its life cycle is shown in
[architecture.md](../architecture.md#thread-safe-functions).

```c
static void CallJs(napi_env env, napi_value js_cb, void *context, void *data) {
  int *value = data;
  if (env != NULL) {             /* NULL when the function is being finalized */
    napi_value undefined, arg;
    napi_get_undefined(env, &undefined);
    napi_create_int32(env, *value, &arg);
    napi_call_function(env, undefined, js_cb, 1, &arg, NULL);
  }
  free(value);
}

/* loop thread */
napi_create_threadsafe_function(env, js_callback, NULL, name, 0 /* unbounded */,
                                1 /* this thread */, NULL, NULL, NULL, CallJs,
                                &tsfn);
/* worker thread */
int *v = malloc(sizeof *v); *v = 42;
napi_call_threadsafe_function(tsfn, v, napi_tsfn_blocking);
napi_release_threadsafe_function(tsfn, napi_tsfn_release);
```

### `napi_create_threadsafe_function`

```c
napi_status napi_create_threadsafe_function(
    napi_env env, napi_value func, napi_value async_resource,
    napi_value async_resource_name, size_t max_queue_size,
    size_t initial_thread_count, void *thread_finalize_data,
    napi_finalize thread_finalize_cb, void *context,
    napi_threadsafe_function_call_js call_js_cb,
    napi_threadsafe_function *result);
```

| Parameter | Meaning |
|---|---|
| `func` | JavaScript function passed to `call_js_cb` (kept with a strong reference); optional when `call_js_cb` is given, otherwise it must be a function (`napi_invalid_arg`) |
| `async_resource`, `async_resource_name` | validated only (no `async_hooks`); the name is required |
| `max_queue_size` | 0 for an unbounded queue; otherwise producers block (or get `napi_queue_full`) when it is full |
| `initial_thread_count` | number of threads using the function, at least 1 (`napi_invalid_arg` otherwise) |
| `thread_finalize_data`, `thread_finalize_cb` | `thread_finalize_cb(env, thread_finalize_data, context)` runs on the loop thread when the function is finalized |
| `context` | any pointer, passed to `call_js_cb` and `thread_finalize_cb` |
| `call_js_cb` | `call_js_cb(env, func, context, data)` for each queued item; without it, `func` is called with no arguments and `this = undefined` |

The function starts **referenced**: it keeps the loop alive until it is
finalized or unreferenced. It registers an internal cleanup hook, so it is
closed at teardown.

### `napi_call_threadsafe_function`

```c
napi_status napi_call_threadsafe_function(
    napi_threadsafe_function func, void *data,
    napi_threadsafe_function_call_mode is_blocking);
```

Queues `data` for a call of `call_js_cb` on the loop thread. Callable from any
thread.

| Result | When |
|---|---|
| `napi_ok` | queued |
| `napi_queue_full` | bounded queue full and `napi_tsfn_nonblocking` |
| (blocks) | bounded queue full and `napi_tsfn_blocking`: waits for room. Do not do this on the loop thread: it would deadlock (`napi_would_deadlock` is not returned, as in Node.js 22) |
| `napi_closing` | the function is closing (aborted, or finalized at teardown); this call also releases the caller's thread count, so the caller must not call `napi_release_threadsafe_function` afterwards |
| `napi_invalid_arg` | `func` is `NULL`, or it is closing and no thread holds it any more |

### `napi_acquire_threadsafe_function`

```c
napi_status napi_acquire_threadsafe_function(napi_threadsafe_function func);
```

Increments the thread count, before handing the function to another thread.
`napi_closing` if it is already closing.

### `napi_release_threadsafe_function`

```c
napi_status napi_release_threadsafe_function(
    napi_threadsafe_function func,
    napi_threadsafe_function_release_mode mode);
```

Decrements the thread count. When it reaches 0 the function closes once its
queue is drained. With `napi_tsfn_abort` it closes **now**: blocked producers
wake up, further calls get `napi_closing`, and items still queued are handed
to `call_js_cb` with `env == NULL` (and `func == NULL`) so that their data can
be freed. `napi_invalid_arg` when the count is already 0.

### `napi_get_threadsafe_function_context`

```c
napi_status napi_get_threadsafe_function_context(napi_threadsafe_function func,
                                                 void **result);
```

Stores the `context` given at creation. Callable from any thread.
`napi_invalid_arg` for `NULL` arguments (Node.js aborts).

### `napi_ref_threadsafe_function`

```c
napi_status napi_ref_threadsafe_function(node_api_basic_env env,
                                         napi_threadsafe_function func);
```

Makes the function keep the loop alive again (`uv_ref` on its async handle).
Loop thread only.

### `napi_unref_threadsafe_function`

```c
napi_status napi_unref_threadsafe_function(node_api_basic_env env,
                                           napi_threadsafe_function func);
```

Lets the loop exit while the function is still open (`uv_unref`): useful for a
background producer that should not keep the process running. Loop thread
only.

### Exceptions in `call_js_cb`

An exception left pending by `call_js_cb` (or `thread_finalize_cb`) has no
JavaScript caller. For modules built for **Node-API ≥ 10** it is reported as
uncaught (`process.on('uncaughtException')`). For older modules it is
dropped with a one-time `DEP0168` deprecation warning on stderr, as in
Node.js, unless the host forces the policy
(`--force-node-api-uncaught-exceptions-policy`, or
[`qjsnapi_host_set_force_uncaught_exceptions_policy`](../embedding.md#qjsnapi_host_set_force_uncaught_exceptions_policy)).

Exceptions from async work `complete` callbacks and from finalizers are always
reported as uncaught.

## Event loop

### `napi_get_uv_event_loop`

```c
napi_status napi_get_uv_event_loop(node_api_basic_env env,
                                   struct uv_loop_s **loop);
```

Stores the libuv loop of the host: the one the embedder runs and the
executable runs until nothing referenced is left. Addons may add their own
handles to it (timers, sockets, `uv_async_t`); they must close them before
teardown ends, typically from an
[async cleanup hook](environment.md#napi_add_async_cleanup_hook). The
executable exports every `uv_*` function, so addons built with node-gyp's
`uv.h` link against it like against `node`.
