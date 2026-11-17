/*
 * Child processes for the quickjs-napi executable: the natives behind the
 * child_process module of host/bootstrap.js.
 *
 *   natives.spawnSync(file, args, options) -> { pid, status, signal,
 *       stdout, stderr, error }
 *     Runs the child on a private libuv loop until it exits and its pipes
 *     are drained. options: { env: ["K=V", ...] | undefined (inherit),
 *     cwd, stdio: ["pipe"|"inherit"|"ignore" x3], input: ArrayBuffer,
 *     timeout (ms, 0 = none), killSignal (name) }. stdout / stderr are
 *     ArrayBuffers for "pipe", null otherwise. `error` is the libuv error
 *     name (e.g. "ENOENT") when the child could not be started, or
 *     "ETIMEDOUT" when the timeout killed it; status is then null.
 *
 *   natives.spawn(file, args, options, onEvent) -> { pid, kill(signal) }
 *     Runs the child on the executable's loop. onEvent(type, a, b):
 *       ("stdout" | "stderr", ArrayBuffer)  data from a "pipe" stream;
 *       ("exit", code, signal)              the process ended;
 *       ("close", code, signal)             the process ended and every
 *                                           stream is closed (last event).
 *     A start failure throws an Error whose `code` is the libuv error name.
 *     stdin "pipe" is not supported for asynchronous children (nothing can
 *     write to it): it is ignored, so the child reads end-of-file.
 *
 * Ownership of an asynchronous child: the struct is malloc'ed and linked in
 * g_children from uv_spawn until the close callback of its last handle; it
 * holds a reference to onEvent until then. The JS handle refers to it by id
 * only, so it may outlive the struct. spawn_close_all() closes every
 * handle at teardown without calling JavaScript.
 */
#include "spawn.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
  char *data;
  size_t len, cap;
} byte_buf;

static int buf_append(byte_buf *b, const char *p, size_t n) {
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + n) cap *= 2;
    char *d = (char *)realloc(b->data, cap);
    if (d == NULL) return -1;
    b->data = d;
    b->cap = cap;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  return 0;
}

/* The signals of the platform (uv.h defines SIGHUP, SIGQUIT, SIGKILL on
 * Windows, where libuv emulates them). */
#define SIG(n) {#n, n}
static const struct {
  const char *name;
  int num;
} kSignals[] = {
    SIG(SIGINT), SIG(SIGILL), SIG(SIGABRT), SIG(SIGFPE), SIG(SIGSEGV),
    SIG(SIGTERM),
#ifdef SIGHUP
    SIG(SIGHUP),
#endif
#ifdef SIGQUIT
    SIG(SIGQUIT),
#endif
#ifdef SIGKILL
    SIG(SIGKILL),
#endif
#ifdef SIGTRAP
    SIG(SIGTRAP),
#endif
#ifdef SIGBUS
    SIG(SIGBUS),
#endif
#ifdef SIGUSR1
    SIG(SIGUSR1),
#endif
#ifdef SIGUSR2
    SIG(SIGUSR2),
#endif
#ifdef SIGPIPE
    SIG(SIGPIPE),
#endif
#ifdef SIGALRM
    SIG(SIGALRM),
#endif
};
#undef SIG

static JSValue signal_name(JSContext *ctx, int num) {
  if (num == 0) return JS_NULL;
  for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++)
    if (kSignals[i].num == num) return JS_NewString(ctx, kSignals[i].name);
  return JS_NewInt32(ctx, num);
}

/* A signal given as a name ("SIGTERM") or a number; -1 if unknown. */
static int signal_number(JSContext *ctx, JSValueConst v, int dflt) {
  if (JS_IsUndefined(v) || JS_IsNull(v)) return dflt;
  if (JS_IsNumber(v)) {
    int32_t n;
    if (JS_ToInt32(ctx, &n, v) < 0) return -1;
    return n;
  }
  const char *s = JS_ToCString(ctx, v);
  if (s == NULL) return -1;
  int num = -1;
  for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++)
    if (strcmp(kSignals[i].name, s) == 0) num = kSignals[i].num;
  JS_FreeCString(ctx, s);
  return num;
}

/* Strings collected from JS for uv_process_options_t. */
typedef struct {
  char *file;
  char **args; /* NULL-terminated, args[0] = file */
  char **env;  /* NULL-terminated, or NULL to inherit */
  char *cwd;
  int stdio[3]; /* 0 ignore, 1 inherit, 2 pipe */
} spawn_spec;

static void spec_free(spawn_spec *s) {
  free(s->file);
  if (s->args)
    for (char **p = s->args; *p; p++) free(*p);
  free(s->args);
  if (s->env)
    for (char **p = s->env; *p; p++) free(*p);
  free(s->env);
  free(s->cwd);
}

static char *dup_string(JSContext *ctx, JSValueConst v) {
  const char *s = JS_ToCString(ctx, v);
  if (s == NULL) return NULL;
  char *d = strdup(s);
  JS_FreeCString(ctx, s);
  if (d == NULL) JS_ThrowOutOfMemory(ctx);
  return d;
}

/* A NULL-terminated copy of the array `arr` of strings, with `first`
 * (if not NULL) prepended. */
static char **dup_string_array(JSContext *ctx, JSValueConst arr,
                               const char *first) {
  int64_t n = 0;
  JSValue len = JS_GetPropertyStr(ctx, arr, "length");
  int r = JS_ToInt64(ctx, &n, len);
  JS_FreeValue(ctx, len);
  if (r < 0) return NULL;
  if (n < 0 || n > 65536) {
    JS_ThrowRangeError(ctx, "spawn: too many strings");
    return NULL;
  }
  size_t extra = first ? 1 : 0;
  char **out = (char **)calloc((size_t)n + extra + 1, sizeof(char *));
  if (out == NULL) {
    JS_ThrowOutOfMemory(ctx);
    return NULL;
  }
  if (first && (out[0] = strdup(first)) == NULL) goto oom;
  for (int64_t i = 0; i < n; i++) {
    JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
    if (JS_IsException(v)) goto fail;
    out[extra + i] = dup_string(ctx, v);
    JS_FreeValue(ctx, v);
    if (out[extra + i] == NULL) goto fail;
  }
  return out;
oom:
  JS_ThrowOutOfMemory(ctx);
fail:
  for (char **p = out; *p; p++) free(*p);
  free(out);
  return NULL;
}

static int spec_init(JSContext *ctx, spawn_spec *s, JSValueConst file,
                     JSValueConst args, JSValueConst options) {
  memset(s, 0, sizeof(*s));
  s->stdio[0] = s->stdio[1] = s->stdio[2] = 2;
  if ((s->file = dup_string(ctx, file)) == NULL) return -1;
  if ((s->args = dup_string_array(ctx, args, s->file)) == NULL) return -1;
  JSValue env = JS_GetPropertyStr(ctx, options, "env");
  if (JS_IsException(env)) return -1;
  if (!JS_IsUndefined(env) && !JS_IsNull(env)) {
    s->env = dup_string_array(ctx, env, NULL);
    JS_FreeValue(ctx, env);
    if (s->env == NULL) return -1;
  }
  JSValue cwd = JS_GetPropertyStr(ctx, options, "cwd");
  if (JS_IsException(cwd)) return -1;
  if (!JS_IsUndefined(cwd) && !JS_IsNull(cwd)) {
    s->cwd = dup_string(ctx, cwd);
    JS_FreeValue(ctx, cwd);
    if (s->cwd == NULL) return -1;
  }
  JSValue stdio = JS_GetPropertyStr(ctx, options, "stdio");
  if (JS_IsException(stdio)) return -1;
  for (uint32_t i = 0; i < 3 && JS_IsArray(ctx, stdio) > 0; i++) {
    JSValue m = JS_GetPropertyUint32(ctx, stdio, i);
    const char *ms = JS_IsString(m) ? JS_ToCString(ctx, m) : NULL;
    if (ms) {
      s->stdio[i] = strcmp(ms, "inherit") == 0  ? 1
                    : strcmp(ms, "ignore") == 0 ? 0
                                                : 2;
      JS_FreeCString(ctx, ms);
    }
    JS_FreeValue(ctx, m);
  }
  JS_FreeValue(ctx, stdio);
  return 0;
}

static void spec_options(const spawn_spec *s, uv_process_options_t *o,
                         uv_stdio_container_t io[3], uv_pipe_t pipes[3],
                         bool stdin_pipe) {
  memset(o, 0, sizeof(*o));
  o->file = s->file;
  o->args = s->args;
  o->env = s->env;
  o->cwd = s->cwd;
  o->flags = UV_PROCESS_WINDOWS_HIDE;
  o->stdio = io;
  o->stdio_count = 3;
  for (int i = 0; i < 3; i++) {
    if (s->stdio[i] == 1) {
      io[i].flags = UV_INHERIT_FD;
      io[i].data.fd = i;
    } else if (s->stdio[i] == 2 && (i > 0 || stdin_pipe)) {
      io[i].flags = (uv_stdio_flags)(UV_CREATE_PIPE |
                                     (i == 0 ? UV_READABLE_PIPE
                                             : UV_WRITABLE_PIPE));
      io[i].data.stream = (uv_stream_t *)&pipes[i];
    } else {
      io[i].flags = UV_IGNORE;
    }
  }
}

static JSValue array_buffer(JSContext *ctx, const byte_buf *b) {
  return JS_NewArrayBufferCopy(ctx, (const uint8_t *)(b->data ? b->data : ""),
                               b->len);
}

static void alloc_cb(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
  (void)h;
  buf->base = (char *)malloc(suggested);
  buf->len = buf->base ? suggested : 0;
}

/* ------------------------------------------------------------------------ */
/* spawnSync                                                                */
/* ------------------------------------------------------------------------ */

typedef struct {
  uv_process_t proc;
  uv_pipe_t pipes[3];
  uv_timer_t timer;
  uv_write_t write_req;
  byte_buf out[3]; /* [1] stdout, [2] stderr */
  bool exited, timed_out, oom;
  int64_t status;
  int term_signal;
  int kill_signal;
} sync_child;

static void sync_read_cb(uv_stream_t *s, ssize_t n, const uv_buf_t *buf) {
  sync_child *c = (sync_child *)s->data;
  int idx = s == (uv_stream_t *)&c->pipes[1] ? 1 : 2;
  if (n > 0 && buf_append(&c->out[idx], buf->base, (size_t)n) < 0)
    c->oom = true;
  free(buf->base);
  if (n < 0) uv_close((uv_handle_t *)s, NULL); /* EOF or error */
}

static void sync_exit_cb(uv_process_t *p, int64_t status, int sig) {
  sync_child *c = (sync_child *)p->data;
  c->exited = true;
  c->status = status;
  c->term_signal = sig;
  uv_close((uv_handle_t *)p, NULL);
  if (uv_is_active((uv_handle_t *)&c->timer)) uv_timer_stop(&c->timer);
  if (!uv_is_closing((uv_handle_t *)&c->timer))
    uv_close((uv_handle_t *)&c->timer, NULL);
}

static void sync_timeout_cb(uv_timer_t *t) {
  sync_child *c = (sync_child *)t->data;
  c->timed_out = true;
  uv_process_kill(&c->proc, c->kill_signal);
}

static void sync_write_cb(uv_write_t *req, int status) {
  (void)status;
  uv_close((uv_handle_t *)req->handle, NULL); /* stdin: end of input */
}

static JSValue error_result(JSContext *ctx, int uv_err) {
  JSValue r = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, r, "pid", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, r, "status", JS_NULL);
  JS_SetPropertyStr(ctx, r, "signal", JS_NULL);
  JS_SetPropertyStr(ctx, r, "stdout", JS_NULL);
  JS_SetPropertyStr(ctx, r, "stderr", JS_NULL);
  JS_SetPropertyStr(ctx, r, "error", JS_NewString(ctx, uv_err_name(uv_err)));
  return r;
}

static JSValue native_spawn_sync(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  JSValueConst options = argv[2];
  spawn_spec spec;
  if (spec_init(ctx, &spec, argv[0], argv[1], options) < 0) {
    spec_free(&spec);
    return JS_EXCEPTION;
  }
  /* Optional input for stdin, and timeout. */
  size_t input_len = 0;
  uint8_t *input_data = NULL;
  JSValue input = JS_GetPropertyStr(ctx, options, "input");
  if (!JS_IsUndefined(input) && !JS_IsNull(input))
    input_data = JS_GetArrayBuffer(ctx, &input_len, input);
  int64_t timeout = 0;
  JSValue tv = JS_GetPropertyStr(ctx, options, "timeout");
  if (!JS_IsUndefined(tv)) JS_ToInt64(ctx, &timeout, tv);
  JS_FreeValue(ctx, tv);
  JSValue ks = JS_GetPropertyStr(ctx, options, "killSignal");
  int kill_signal = signal_number(ctx, ks, SIGTERM);
  JS_FreeValue(ctx, ks);
  if (JS_HasException(ctx)) {
    JS_FreeValue(ctx, input);
    spec_free(&spec);
    return JS_EXCEPTION;
  }

  uv_loop_t loop;
  int err = uv_loop_init(&loop);
  if (err < 0) {
    JS_FreeValue(ctx, input);
    spec_free(&spec);
    return error_result(ctx, err);
  }
  sync_child *c = (sync_child *)calloc(1, sizeof(sync_child));
  if (c == NULL) {
    uv_loop_close(&loop);
    JS_FreeValue(ctx, input);
    spec_free(&spec);
    return JS_ThrowOutOfMemory(ctx);
  }
  c->kill_signal = kill_signal < 0 ? SIGTERM : kill_signal;
  bool stdin_pipe = spec.stdio[0] == 2 && input_data != NULL;
  for (int i = 0; i < 3; i++) {
    uv_pipe_init(&loop, &c->pipes[i], 0);
    c->pipes[i].data = c;
  }
  uv_process_options_t o;
  uv_stdio_container_t io[3];
  spec_options(&spec, &o, io, c->pipes, stdin_pipe);
  o.exit_cb = sync_exit_cb;
  c->proc.data = c;
  err = uv_spawn(&loop, &c->proc, &o);

  JSValue result;
  if (err < 0) {
    /* libuv: a process handle that failed to spawn must still be closed. */
    uv_close((uv_handle_t *)&c->proc, NULL);
    for (int i = 0; i < 3; i++) uv_close((uv_handle_t *)&c->pipes[i], NULL);
    uv_run(&loop, UV_RUN_DEFAULT);
    result = error_result(ctx, err);
  } else {
    for (int i = 0; i < 3; i++) {
      bool used = io[i].flags & UV_CREATE_PIPE;
      if (!used) {
        uv_close((uv_handle_t *)&c->pipes[i], NULL);
      } else if (i == 0) {
        uv_buf_t b = uv_buf_init((char *)input_data, (unsigned)input_len);
        if (uv_write(&c->write_req, (uv_stream_t *)&c->pipes[0], &b, 1,
                     sync_write_cb) < 0)
          uv_close((uv_handle_t *)&c->pipes[0], NULL);
      } else {
        uv_read_start((uv_stream_t *)&c->pipes[i], alloc_cb, sync_read_cb);
      }
    }
    uv_timer_init(&loop, &c->timer);
    c->timer.data = c;
    if (timeout > 0) uv_timer_start(&c->timer, sync_timeout_cb, (uint64_t)timeout, 0);
    uv_run(&loop, UV_RUN_DEFAULT);

    result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "pid", JS_NewInt32(ctx, c->proc.pid));
    bool by_signal = c->term_signal != 0;
    JS_SetPropertyStr(ctx, result, "status",
                      by_signal ? JS_NULL : JS_NewInt64(ctx, c->status));
    JS_SetPropertyStr(ctx, result, "signal", signal_name(ctx, c->term_signal));
    for (int i = 1; i < 3; i++)
      JS_SetPropertyStr(ctx, result, i == 1 ? "stdout" : "stderr",
                        spec.stdio[i] == 2 ? array_buffer(ctx, &c->out[i])
                                           : JS_NULL);
    JS_SetPropertyStr(ctx, result, "error",
                      c->timed_out ? JS_NewString(ctx, "ETIMEDOUT")
                      : c->oom     ? JS_NewString(ctx, "ENOMEM")
                                   : JS_NULL);
  }
  uv_loop_close(&loop); /* every handle is closed: cannot be EBUSY */
  for (int i = 0; i < 3; i++) free(c->out[i].data);
  free(c);
  JS_FreeValue(ctx, input);
  spec_free(&spec);
  return result;
}

/* ------------------------------------------------------------------------ */
/* spawn                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct async_child {
  struct async_child *next;
  uint32_t id; /* what the kill() function of the JS handle looks up */
  uv_process_t proc;
  uv_pipe_t pipes[3];
  bool pipe_open[3];
  int open_handles;
  JSContext *ctx;
  JSValue on_event;
  int64_t status;
  int term_signal;
  bool exited, silent; /* silent: teardown, no more JavaScript */
} async_child;

static uv_loop_t *g_spawn_loop;
static spawn_dispatch_fn g_dispatch;
static async_child *g_children;
static uint32_t g_next_child_id;

static void child_event(async_child *c, const char *type, JSValue a,
                        JSValue b) {
  if (c->silent) {
    JS_FreeValue(c->ctx, a);
    JS_FreeValue(c->ctx, b);
    return;
  }
  JSValue args[3] = {JS_NewString(c->ctx, type), a, b};
  g_dispatch(c->ctx, c->on_event, 3, args);
  for (int i = 0; i < 3; i++) JS_FreeValue(c->ctx, args[i]);
}

static void child_unlink(async_child *c) {
  for (async_child **p = &g_children; *p; p = &(*p)->next)
    if (*p == c) {
      *p = c->next;
      return;
    }
}

static JSValue exit_code(async_child *c) {
  return c->term_signal ? JS_NULL : JS_NewInt64(c->ctx, c->status);
}

static void child_handle_closed(uv_handle_t *h) {
  async_child *c = (async_child *)h->data;
  if (--c->open_handles > 0) return;
  child_event(c, "close", exit_code(c), signal_name(c->ctx, c->term_signal));
  child_unlink(c);
  JS_FreeValue(c->ctx, c->on_event);
  free(c);
}

static void child_read_cb(uv_stream_t *s, ssize_t n, const uv_buf_t *buf) {
  async_child *c = (async_child *)s->data;
  int idx = s == (uv_stream_t *)&c->pipes[1] ? 1 : 2;
  if (n > 0) {
    child_event(c, idx == 1 ? "stdout" : "stderr",
                JS_NewArrayBufferCopy(c->ctx, (const uint8_t *)buf->base,
                                      (size_t)n),
                JS_UNDEFINED);
  }
  free(buf->base);
  if (n < 0 && c->pipe_open[idx]) {
    c->pipe_open[idx] = false;
    uv_close((uv_handle_t *)s, child_handle_closed);
  }
}

static void child_exit_cb(uv_process_t *p, int64_t status, int sig) {
  async_child *c = (async_child *)p->data;
  c->exited = true;
  c->status = status;
  c->term_signal = sig;
  child_event(c, "exit", exit_code(c), signal_name(c->ctx, sig));
  uv_close((uv_handle_t *)p, child_handle_closed);
}

/* kill(signal), bound to a child id (func_data[0]): the handle object may
 * outlive the child struct. */
static JSValue native_child_kill(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic,
                                 JSValueConst *func_data) {
  (void)this_val;
  (void)magic;
  uint32_t id = 0;
  JS_ToUint32(ctx, &id, func_data[0]);
  async_child *c = g_children;
  while (c && c->id != id) c = c->next;
  /* Node.js: kill() after the child exited returns false. */
  if (c == NULL || c->exited) return JS_FALSE;
  int sig = signal_number(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, SIGTERM);
  if (sig < 0) return JS_ThrowTypeError(ctx, "Unknown signal");
  return JS_NewBool(ctx, uv_process_kill(&c->proc, sig) == 0);
}

static JSValue native_spawn(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  if (!JS_IsFunction(ctx, argv[3]))
    return JS_ThrowTypeError(ctx, "spawn: onEvent must be a function");
  spawn_spec spec;
  if (spec_init(ctx, &spec, argv[0], argv[1], argv[2]) < 0) {
    spec_free(&spec);
    return JS_EXCEPTION;
  }
  async_child *c = (async_child *)calloc(1, sizeof(async_child));
  if (c == NULL) {
    spec_free(&spec);
    return JS_ThrowOutOfMemory(ctx);
  }
  c->ctx = ctx;
  for (int i = 0; i < 3; i++) {
    uv_pipe_init(g_spawn_loop, &c->pipes[i], 0);
    c->pipes[i].data = c;
  }
  uv_process_options_t o;
  uv_stdio_container_t io[3];
  spec_options(&spec, &o, io, c->pipes, false);
  o.exit_cb = child_exit_cb;
  c->proc.data = c;
  int err = uv_spawn(g_spawn_loop, &c->proc, &o);
  spec_free(&spec);
  if (err < 0) {
    /* The pipes are initialized handles: closed, freed by the callback. */
    c->silent = true;
    c->exited = true; /* nothing to kill */
    c->on_event = JS_UNDEFINED;
    c->open_handles = 4;
    c->next = g_children;
    g_children = c;
    /* libuv: a process handle that failed to spawn must still be closed. */
    uv_close((uv_handle_t *)&c->proc, child_handle_closed);
    for (int i = 0; i < 3; i++)
      uv_close((uv_handle_t *)&c->pipes[i], child_handle_closed);
    JSValue e = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, e, "message",
                      JS_NewString(ctx, uv_strerror(err)));
    JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, uv_err_name(err)));
    return JS_Throw(ctx, e);
  }
  c->on_event = JS_DupValue(ctx, argv[3]);
  c->open_handles = 1; /* the process */
  for (int i = 0; i < 3; i++) {
    if (io[i].flags & UV_CREATE_PIPE) {
      c->pipe_open[i] = true;
      c->open_handles++;
      uv_read_start((uv_stream_t *)&c->pipes[i], alloc_cb, child_read_cb);
    } else {
      c->open_handles++;
      uv_close((uv_handle_t *)&c->pipes[i], child_handle_closed);
    }
  }
  c->id = ++g_next_child_id;
  c->next = g_children;
  g_children = c;

  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) return obj;
  JSValue id = JS_NewUint32(ctx, c->id);
  JS_SetPropertyStr(ctx, obj, "pid", JS_NewInt32(ctx, c->proc.pid));
  JS_SetPropertyStr(ctx, obj, "kill",
                    JS_NewCFunctionData(ctx, native_child_kill, 1, 0, 1, &id));
  if (JS_HasException(ctx)) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

void spawn_close_all(void) {
  for (async_child *c = g_children; c; c = c->next) {
    c->silent = true;
    if (!c->exited && !uv_is_closing((uv_handle_t *)&c->proc))
      uv_close((uv_handle_t *)&c->proc, child_handle_closed);
    for (int i = 1; i < 3; i++)
      if (c->pipe_open[i]) {
        c->pipe_open[i] = false;
        uv_close((uv_handle_t *)&c->pipes[i], child_handle_closed);
      }
  }
}

int spawn_add_natives(JSContext *ctx, JSValueConst natives, uv_loop_t *loop,
                      spawn_dispatch_fn dispatch) {
  g_spawn_loop = loop;
  g_dispatch = dispatch;
  if (JS_SetPropertyStr(ctx, natives, "spawnSync",
                        JS_NewCFunction(ctx, native_spawn_sync, "spawnSync",
                                        3)) < 0 ||
      JS_SetPropertyStr(ctx, natives, "spawn",
                        JS_NewCFunction(ctx, native_spawn, "spawn", 4)) < 0)
    return -1;
  return 0;
}
