/*
 * napi_threadsafe_function: a port of Node 22's v8impl::ThreadSafeFunction
 * on libuv primitives.
 *
 * Threads:
 *   any thread  napi_call_threadsafe_function (Push),
 *               napi_acquire_threadsafe_function, napi_release_...
 *   loop thread creation, the uv_async_t callback (Dispatch), ref/unref,
 *               closing and finalization.
 * Shared state (queue, thread_count, is_closing) is guarded by `mutex`;
 * `cond` (only with a bounded queue) wakes producers blocked on a full
 * queue. `dispatch_state` lets a producer skip uv_async_send while a
 * Dispatch is running and will look at the queue again (Node's protocol).
 *
 * Lifecycle:
 *   1. create: uv_async_init (referenced: keeps the loop alive), a strong
 *      reference to the JavaScript function, an internal env cleanup hook,
 *      host->live_tsfns++.
 *   2. Dispatch, on each wake-up: up to 1000 items per run; each item calls
 *      call_js_cb(env, js_func, context, data) inside a callback frame
 *      (handle scope, callback scope, microtask checkpoint; an exception is
 *      uncaught, or a DEP0168 warning for Node-API < 10 without the forced
 *      policy: Node's CallbackIntoModule<false>).
 *   3. Closing starts when the queue is empty and thread_count reaches 0,
 *      on napi_tsfn_abort, or at env teardown (cleanup hook): the async
 *      handle is closed; producers then get napi_closing.
 *   4. In the close callback: thread_finalize_cb (callback frame, policy as
 *      above), then call_js_cb(NULL, NULL, context, data) for every item
 *      left in the queue, then everything is freed and live_tsfns--.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "qjsnapi_internal.h"

enum {
  DISPATCH_IDLE = 0,
  DISPATCH_RUNNING = 1 << 0,
  DISPATCH_PENDING = 1 << 1,
};

#define MAX_ITERATIONS 1000

struct napi_threadsafe_function__ {
  uv_async_t async;
  uv_mutex_t mutex;
  uv_cond_t cond; /* only when max_queue_size > 0 */
  bool has_cond;
  /* FIFO ring buffer of the data pointers. */
  void **items;
  size_t head, count, capacity;
  size_t thread_count;
  bool is_closing;
  atomic_uchar dispatch_state;
  void *context;
  size_t max_queue_size; /* 0: unbounded */
  JSValue func;          /* JS_UNDEFINED when created without one */
  napi_env env;
  void *finalize_data;
  napi_finalize finalize_cb;
  napi_threadsafe_function_call_js call_js_cb;
  bool handles_closing;
  qn_cleanup_hook *hook; /* in env->cleanup_hooks until it runs */
};

/* ---- queue (mutex held) -------------------------------------------------- */

static bool queue_push(napi_threadsafe_function ts, void *data) {
  if (ts->count == ts->capacity) {
    size_t cap = ts->capacity ? ts->capacity * 2 : 16;
    void **items = (void **)malloc(cap * sizeof(void *));
    if (!items) return false;
    for (size_t i = 0; i < ts->count; i++)
      items[i] = ts->items[(ts->head + i) % ts->capacity];
    free(ts->items);
    ts->items = items;
    ts->head = 0;
    ts->capacity = cap;
  }
  ts->items[(ts->head + ts->count) % ts->capacity] = data;
  ts->count++;
  return true;
}

static void *queue_pop(napi_threadsafe_function ts) {
  void *data = ts->items[ts->head];
  ts->head = (ts->head + 1) % ts->capacity;
  ts->count--;
  return data;
}

/* ---- loop side ----------------------------------------------------------- */

/* Node: ThreadSafeFunction::CallJs, the default call_js_cb. */
static void default_call_js(napi_env env, napi_value cb, void *context,
                            void *data) {
  (void)context;
  (void)data;
  if (env == NULL || cb == NULL) return;
  napi_value recv;
  napi_status status = napi_get_undefined(env, &recv);
  if (status != napi_ok) {
    napi_throw_error(env, "ERR_NAPI_TSFN_GET_UNDEFINED",
                     "Failed to retrieve undefined value");
    return;
  }
  status = napi_call_function(env, recv, cb, 0, NULL, NULL);
  if (status != napi_ok && status != napi_pending_exception) {
    napi_throw_error(env, "ERR_NAPI_TSFN_CALL_JS",
                     "Failed to call JS callback");
  }
}

static void tsfn_send(napi_threadsafe_function ts) {
  unsigned char current =
      atomic_fetch_or(&ts->dispatch_state, (unsigned char)DISPATCH_PENDING);
  /* A running Dispatch will make one more iteration. */
  if ((current & DISPATCH_RUNNING) == DISPATCH_RUNNING) return;
  uv_async_send(&ts->async);
}

static void tsfn_free(napi_threadsafe_function ts) {
  napi_env env = ts->env;
  if (ts->hook) {
    qn_list_del(&ts->hook->link);
    free(ts->hook);
  }
  JS_FreeValue(env->ctx, ts->func);
  if (ts->has_cond) uv_cond_destroy(&ts->cond);
  uv_mutex_destroy(&ts->mutex);
  free(ts->items);
  env->host->live_tsfns--;
  free(ts);
}

/* Node: ThreadSafeFunction::Finalize, from the close callback. */
static void on_closed(uv_handle_t *handle) {
  napi_threadsafe_function ts = (napi_threadsafe_function)handle->data;
  napi_env env = ts->env;
  if (ts->finalize_cb) {
    qn_callback_frame frame;
    qn_callback_enter(env, &frame);
    ts->finalize_cb(env, ts->finalize_data, ts->context);
    qn_callback_leave(env, &frame, false);
  }
  /* EmptyQueueAndDelete: the remaining items, without an env. */
  while (ts->count > 0) ts->call_js_cb(NULL, NULL, ts->context, queue_pop(ts));
  tsfn_free(ts);
}

/* Node: CloseHandlesAndMaybeDelete. Called with the mutex held unless
 * set_closing (cleanup hook). */
static void close_handles(napi_threadsafe_function ts, bool set_closing) {
  if (set_closing) {
    uv_mutex_lock(&ts->mutex);
    ts->is_closing = true;
    if (ts->has_cond) uv_cond_signal(&ts->cond);
    uv_mutex_unlock(&ts->mutex);
  }
  if (ts->handles_closing) return;
  ts->handles_closing = true;
  uv_close((uv_handle_t *)&ts->async, on_closed);
}

/* Node: DispatchOne. Returns true when more items are queued. */
static bool dispatch_one(napi_threadsafe_function ts) {
  void *data = NULL;
  bool popped = false;
  bool has_more = false;

  uv_mutex_lock(&ts->mutex);
  if (ts->is_closing) {
    close_handles(ts, false);
  } else {
    size_t size = ts->count;
    if (size > 0) {
      data = queue_pop(ts);
      popped = true;
      if (size == ts->max_queue_size && ts->max_queue_size > 0)
        uv_cond_signal(&ts->cond);
      size--;
    }
    if (size == 0) {
      if (ts->thread_count == 0) {
        ts->is_closing = true;
        if (ts->has_cond) uv_cond_signal(&ts->cond);
        close_handles(ts, false);
      }
    } else {
      has_more = true;
    }
  }
  uv_mutex_unlock(&ts->mutex);

  if (popped) {
    napi_env env = ts->env;
    qn_callback_frame frame;
    qn_callback_enter(env, &frame);
    napi_value js_cb = NULL;
    if (!JS_IsUndefined(ts->func))
      js_cb = qn_new_handle(env, JS_DupValue(env->ctx, ts->func));
    ts->call_js_cb(env, js_cb, ts->context, data);
    qn_callback_leave(env, &frame, false);
  }
  return has_more;
}

/* Node: Dispatch, the uv_async_t callback. */
static void on_async(uv_async_t *handle) {
  napi_threadsafe_function ts = (napi_threadsafe_function)handle->data;
  bool has_more = true;
  unsigned int iterations_left = MAX_ITERATIONS;
  while (has_more && --iterations_left != 0) {
    atomic_store(&ts->dispatch_state, (unsigned char)DISPATCH_RUNNING);
    has_more = dispatch_one(ts);
    /* tsfn_send() was called while the callback was running. */
    if (atomic_exchange(&ts->dispatch_state, (unsigned char)DISPATCH_IDLE) !=
        DISPATCH_RUNNING)
      has_more = true;
  }
  if (has_more) tsfn_send(ts);
}

/* Internal env cleanup hook (Node: ThreadSafeFunction::Cleanup). */
static void on_cleanup(void *arg) {
  napi_threadsafe_function ts = (napi_threadsafe_function)arg;
  ts->hook = NULL; /* the entry was freed by the caller */
  close_handles(ts, true);
}

/* ---- API ----------------------------------------------------------------- */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_threadsafe_function(
    napi_env env, napi_value func, napi_value async_resource,
    napi_value async_resource_name, size_t max_queue_size,
    size_t initial_thread_count, void *thread_finalize_data,
    napi_finalize thread_finalize_cb, void *context,
    napi_threadsafe_function_call_js call_js_cb,
    napi_threadsafe_function *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, async_resource_name);
  RETURN_STATUS_IF_FALSE(env, initial_thread_count > 0, napi_invalid_arg);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  if (func == NULL) {
    CHECK_ARG(env, call_js_cb);
  } else {
    /* Node: CHECK_TO_FUNCTION */
    RETURN_STATUS_IF_FALSE(env, JS_IsFunction(ctx, qn_value(func)),
                           napi_invalid_arg);
  }
  /* Node: CHECK_TO_OBJECT(async_resource), CHECK_TO_STRING(name). */
  if (async_resource != NULL) {
    JSValue r = qn_value(async_resource);
    RETURN_STATUS_IF_FALSE(env, !JS_IsNull(r) && !JS_IsUndefined(r),
                           napi_object_expected);
  }
  RETURN_STATUS_IF_FALSE(env, !JS_IsSymbol(qn_value(async_resource_name)),
                         napi_string_expected);

  napi_threadsafe_function ts = (napi_threadsafe_function)calloc(
      1, sizeof(struct napi_threadsafe_function__));
  RETURN_STATUS_IF_FALSE(env, ts != NULL, napi_generic_failure);
  ts->env = env;
  ts->thread_count = initial_thread_count;
  ts->context = context;
  ts->max_queue_size = max_queue_size;
  ts->finalize_data = thread_finalize_data;
  ts->finalize_cb = thread_finalize_cb;
  ts->call_js_cb = call_js_cb ? call_js_cb : default_call_js;
  ts->func = JS_UNDEFINED;
  atomic_init(&ts->dispatch_state, (unsigned char)DISPATCH_IDLE);
  if (uv_mutex_init(&ts->mutex) != 0) {
    free(ts);
    return qn_set_last_error(env, napi_generic_failure);
  }
  if (max_queue_size > 0) {
    if (uv_cond_init(&ts->cond) != 0) {
      uv_mutex_destroy(&ts->mutex);
      free(ts);
      return qn_set_last_error(env, napi_generic_failure);
    }
    ts->has_cond = true;
  }
  if (!qn_cleanup_hook_add(env, QN_HOOK_INTERNAL, on_cleanup, ts, &ts->hook)) {
    if (ts->has_cond) uv_cond_destroy(&ts->cond);
    uv_mutex_destroy(&ts->mutex);
    free(ts);
    return qn_set_last_error(env, napi_generic_failure);
  }
  if (uv_async_init(env->host->loop, &ts->async, on_async) != 0) {
    qn_list_del(&ts->hook->link);
    free(ts->hook);
    if (ts->has_cond) uv_cond_destroy(&ts->cond);
    uv_mutex_destroy(&ts->mutex);
    free(ts);
    return qn_set_last_error(env, napi_generic_failure);
  }
  ts->async.data = ts;
  if (func != NULL) ts->func = JS_DupValue(ctx, qn_value(func));
  env->host->live_tsfns++;
  *result = ts;
  return qn_clear_last_error(env);
}

/* No env: invalid arguments return napi_invalid_arg (Node.js aborts). */
NAPI_EXTERN napi_status NAPI_CDECL
napi_get_threadsafe_function_context(napi_threadsafe_function func,
                                     void **result) {
  if (func == NULL || result == NULL) return napi_invalid_arg;
  *result = func->context;
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_call_threadsafe_function(
    napi_threadsafe_function func, void *data,
    napi_threadsafe_function_call_mode is_blocking) {
  if (func == NULL) return napi_invalid_arg;
  napi_threadsafe_function ts = func;
  napi_status status;
  uv_mutex_lock(&ts->mutex);
  while (ts->count >= ts->max_queue_size && ts->max_queue_size > 0 &&
         !ts->is_closing) {
    if (is_blocking == napi_tsfn_nonblocking) {
      uv_mutex_unlock(&ts->mutex);
      return napi_queue_full;
    }
    uv_cond_wait(&ts->cond, &ts->mutex);
  }
  if (ts->is_closing) {
    if (ts->thread_count == 0) {
      status = napi_invalid_arg;
    } else {
      ts->thread_count--;
      status = napi_closing;
    }
  } else if (!queue_push(ts, data)) {
    status = napi_generic_failure;
  } else {
    tsfn_send(ts);
    status = napi_ok;
  }
  uv_mutex_unlock(&ts->mutex);
  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_acquire_threadsafe_function(napi_threadsafe_function func) {
  if (func == NULL) return napi_invalid_arg;
  napi_status status = napi_ok;
  uv_mutex_lock(&func->mutex);
  if (func->is_closing)
    status = napi_closing;
  else
    func->thread_count++;
  uv_mutex_unlock(&func->mutex);
  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_release_threadsafe_function(
    napi_threadsafe_function func, napi_threadsafe_function_release_mode mode) {
  if (func == NULL) return napi_invalid_arg;
  napi_threadsafe_function ts = func;
  uv_mutex_lock(&ts->mutex);
  if (ts->thread_count == 0) {
    uv_mutex_unlock(&ts->mutex);
    return napi_invalid_arg;
  }
  ts->thread_count--;
  if (ts->thread_count == 0 || mode == napi_tsfn_abort) {
    if (!ts->is_closing) {
      ts->is_closing = (mode == napi_tsfn_abort);
      if (ts->is_closing && ts->max_queue_size > 0)
        uv_cond_signal(&ts->cond);
      tsfn_send(ts);
    }
  }
  uv_mutex_unlock(&ts->mutex);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_unref_threadsafe_function(
    node_api_basic_env env, napi_threadsafe_function func) {
  (void)env;
  if (func == NULL) return napi_invalid_arg;
  uv_unref((uv_handle_t *)&func->async);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_ref_threadsafe_function(
    node_api_basic_env env, napi_threadsafe_function func) {
  (void)env;
  if (func == NULL) return napi_invalid_arg;
  uv_ref((uv_handle_t *)&func->async);
  return napi_ok;
}
