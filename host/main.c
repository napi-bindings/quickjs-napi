/*
 * quickjs-napi host executable.
 *
 *   quickjs-napi [options] <script.js> [args...]
 *   quickjs-napi [options] -e <code> [args...]
 *
 * Options (Node.js names), also read from NODE_OPTIONS (where the options
 * only Node.js knows are ignored):
 *   -e, --eval <code>        run the code instead of a script
 *   -r, --require <module>   load the module first (repeatable)
 *   --expose-gc   accepted; gc() is always available
 *   --force-node-api-uncaught-exceptions-policy[=true|false],
 *   --no-force-node-api-uncaught-exceptions-policy
 *                 see qjsnapi_host_set_force_uncaught_exceptions_policy
 *
 * Runs the script as a CommonJS module (host/bootstrap.js): `require` loads
 * .js, .json and Node-API addons (.node), with Node.js resolution rules for
 * relative paths, directories and node_modules. Globals: `process` (subset),
 * `global`, `gc()`, `Buffer`, timers (setTimeout, setInterval, setImmediate,
 * queueMicrotask), plus the quickjs-libc helpers (console.log, print,
 * scriptArgs). Builtin modules: module, timers, timers/promises, buffer,
 * util, url, vm, child_process (host/spawn.c), events (minimal subsets, see
 * host/bootstrap.js).
 *
 * Event loop: the host's libuv loop (qjsnapi_host_get_loop), run with
 * uv_run until nothing referenced is left, then process 'beforeExit' (which
 * may schedule more work) and 'exit'. On the loop:
 *   g_timer  uv_timer_t armed by bootstrap.js for the earliest JS timer
 *            (natives.armTimer); referenced only while a referenced timer
 *            exists. Timers phase: due timers run one at a time.
 *   g_check  uv_check_t (unreferenced): runs the immediates queued before
 *            the check phase began, one at a time.
 *   g_idle   uv_idle_t, active while immediates are queued (referenced
 *            only for referenced ones): the poll phase does not block.
 *   plus the library's handles: queued Node-API finalizers, async work,
 *   thread-safe functions, and whatever addons add.
 * After every callback a checkpoint (host_checkpoint, also installed as
 * the library's checkpoint) runs the promise jobs, then reports unhandled
 * rejections (process 'unhandledRejection', else an uncaught exception:
 * Node.js default `--unhandled-rejections=throw`), until both are empty.
 * An uncaught exception goes to process 'uncaughtException' listeners or
 * prints the error and exits with code 1, as in Node.js.
 *
 * Exit: at the end of the script the host is destroyed (cleanup hooks,
 * finalizers, see qjsnapi_host_destroy). process.exit() and fatal
 * exceptions end the process at once instead, without that teardown, as
 * Node.js does.
 *
 * The executable exports every napi_* and uv_* symbol (see CMakeLists.txt):
 * addons built for Node.js resolve them from this process.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <limits.h>
#include <time.h>
#include <unistd.h>
#endif

#include <uv.h>

#if defined(__SANITIZE_ADDRESS__)
#define QN_LEAK_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QN_LEAK_SANITIZER 1
#endif
#endif
#ifndef QN_LEAK_SANITIZER
#define QN_LEAK_SANITIZER 0
#endif

#include "bootstrap_js.h" /* generated: qn_bootstrap_js, qn_bootstrap_js_len */
#include "qjsnapi.h"
#include "spawn.h"
#include "quickjs-libc.h"
#include "quickjs.h"

#ifndef QJS_VERSION_STR
#define QJS_VERSION_STR "unknown"
#endif

static int g_exit_requested;
/* -e / --eval: the code to run instead of a script, or NULL. */
static const char *g_eval_code;
/* -r / --require: modules loaded before the script (from the command line
 * and NODE_OPTIONS), in order. */
#define MAX_PRELOADS 16
static const char *g_preloads[MAX_PRELOADS];
static int g_preload_count;
static char *g_node_options_copy; /* storage of the NODE_OPTIONS tokens */

static int g_exit_code;
/* Object returned by bootstrap.js: runMain, emitExit, emitBeforeExit,
 * runDueTimer, immediateCount, runImmediate, fatalException,
 * unhandledRejection. */
static JSValue g_api;
static JSContext *g_ctx;
static uv_loop_t *g_loop;
static uv_timer_t g_timer;
static uv_check_t g_check;
static uv_idle_t g_idle;

/* ------------------------------------------------------------------------ */
/* File system helpers (UTF-8 paths on every platform)                      */
/* ------------------------------------------------------------------------ */

#ifdef _WIN32
static wchar_t *to_wide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

static char *from_wide(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s = n > 0 ? (char *)malloc((size_t)n) : NULL;
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}
#endif

/* 0: missing, 1: file, 2: directory */
static int path_kind(const char *path) {
#ifdef _WIN32
  wchar_t *w = to_wide(path);
  if (!w) return 0;
  DWORD attr = GetFileAttributesW(w);
  free(w);
  if (attr == INVALID_FILE_ATTRIBUTES) return 0;
  return (attr & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
#else
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return S_ISDIR(st.st_mode) ? 2 : 1;
#endif
}

static char *real_path(const char *path) {
#ifdef _WIN32
  wchar_t *w = to_wide(path);
  if (!w) return NULL;
  wchar_t full[32768];
  DWORD n = GetFullPathNameW(w, 32768, full, NULL);
  free(w);
  if (n == 0 || n >= 32768) return NULL;
  return from_wide(full);
#else
  return realpath(path, NULL);
#endif
}

static FILE *open_file(const char *path) {
#ifdef _WIN32
  wchar_t *w = to_wide(path);
  FILE *f = w ? _wfopen(w, L"rb") : NULL;
  free(w);
  return f;
#else
  return fopen(path, "rb");
#endif
}

static char *current_dir(void) {
#ifdef _WIN32
  wchar_t buf[32768];
  DWORD n = GetCurrentDirectoryW(32768, buf);
  return (n == 0 || n >= 32768) ? NULL : from_wide(buf);
#else
  char *buf = (char *)malloc(PATH_MAX);
  if (buf && !getcwd(buf, PATH_MAX)) {
    free(buf);
    buf = NULL;
  }
  return buf;
#endif
}

/* ------------------------------------------------------------------------ */
/* Natives exposed to host/bootstrap.js                                     */
/* ------------------------------------------------------------------------ */

static const char *arg_string(JSContext *ctx, int argc, JSValueConst *argv,
                              int i, size_t *len) {
  if (i >= argc || !JS_IsString(argv[i])) {
    JS_ThrowTypeError(ctx, "argument %d must be a string", i);
    return NULL;
  }
  return JS_ToCStringLen(ctx, len, argv[i]);
}

static JSValue throw_errno(JSContext *ctx, const char *op, const char *path,
                           int err) {
  const char *code = err == ENOENT ? "ENOENT" : err == EACCES ? "EACCES" : "EIO";
  JSValue e = JS_NewError(ctx);
  if (JS_IsException(e)) return e;
  char msg[1024];
  snprintf(msg, sizeof(msg), "%s: %s, %s '%s'", code, strerror(err), op, path);
  JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, code));
  JS_SetPropertyStr(ctx, e, "path", JS_NewString(ctx, path));
  return JS_Throw(ctx, e);
}

static JSValue native_stat(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  (void)this_val;
  const char *path = arg_string(ctx, argc, argv, 0, NULL);
  if (!path) return JS_EXCEPTION;
  int kind = path_kind(path);
  JS_FreeCString(ctx, path);
  return JS_NewInt32(ctx, kind);
}

static JSValue native_realpath(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  (void)this_val;
  const char *path = arg_string(ctx, argc, argv, 0, NULL);
  if (!path) return JS_EXCEPTION;
  char *real = real_path(path);
  JS_FreeCString(ctx, path);
  if (!real) return JS_NULL;
  JSValue r = JS_NewString(ctx, real);
  free(real);
  return r;
}

static JSValue native_read_file(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  (void)this_val;
  const char *path = arg_string(ctx, argc, argv, 0, NULL);
  if (!path) return JS_EXCEPTION;
  FILE *f = open_file(path);
  if (!f) {
    JSValue e = throw_errno(ctx, "open", path, errno);
    JS_FreeCString(ctx, path);
    return e;
  }
  char *buf = NULL;
  size_t len = 0, cap = 0;
  for (;;) {
    if (len == cap) {
      cap = cap ? cap * 2 : 65536;
      char *n = (char *)realloc(buf, cap);
      if (!n) {
        free(buf);
        fclose(f);
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
      }
      buf = n;
    }
    size_t r = fread(buf + len, 1, cap - len, f);
    len += r;
    if (r == 0) break;
  }
  int failed = ferror(f);
  fclose(f);
  if (failed) {
    free(buf);
    JSValue e = throw_errno(ctx, "read", path, EIO);
    JS_FreeCString(ctx, path);
    return e;
  }
  JS_FreeCString(ctx, path);
  /* Skip a UTF-8 BOM, as Node.js does for modules. */
  size_t off = (len >= 3 && (unsigned char)buf[0] == 0xEF &&
                (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
                   ? 3
                   : 0;
  JSValue s = JS_NewStringLen(ctx, buf + off, len - off);
  free(buf);
  return s;
}

static JSValue native_compile(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  (void)this_val;
  size_t len;
  const char *src = arg_string(ctx, argc, argv, 0, &len);
  if (!src) return JS_EXCEPTION;
  const char *filename = arg_string(ctx, argc, argv, 1, NULL);
  if (!filename) {
    JS_FreeCString(ctx, src);
    return JS_EXCEPTION;
  }
  JSValue r = JS_Eval(ctx, src, len, filename, JS_EVAL_TYPE_GLOBAL);
  JS_FreeCString(ctx, filename);
  JS_FreeCString(ctx, src);
  return r;
}

static JSValue native_load_addon(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  (void)this_val;
  const char *path = arg_string(ctx, argc, argv, 0, NULL);
  if (!path) return JS_EXCEPTION;
  JSValue exports;
  int r = qjsnapi_load_addon(qjsnapi_host_from_context(ctx), path, &exports);
  JS_FreeCString(ctx, path);
  return r < 0 ? JS_EXCEPTION : exports;
}

static JSValue native_cwd(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  char *dir = current_dir();
  if (!dir) return JS_ThrowInternalError(ctx, "cannot read the current directory");
  JSValue r = JS_NewString(ctx, dir);
  free(dir);
  return r;
}

/* process.exit(): unwinds the JS stack with an uncatchable exception. */
static JSValue native_exit(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  (void)this_val;
  int32_t code = 0;
  if (argc > 0) JS_ToInt32(ctx, &code, argv[0]);
  g_exit_requested = 1;
  g_exit_code = code;
  if (g_loop) uv_stop(g_loop);
  JS_ThrowInternalError(ctx, "process.exit(%d)", (int)code);
  JS_SetUncatchableException(ctx, 1);
  return JS_EXCEPTION;
}

/* global.gc(). Two passes: QuickJS drops the WeakMap entries of dead keys
 * before collecting cycles, so a target that died in a cycle is only seen as
 * dead (and its Node-API finalizer queued) by the second pass. */
static JSValue js_gc(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  /* V8's gc() collects the targets of weak handles at once. */
  qjsnapi_host_clear_kept_objects(qjsnapi_host_from_context(ctx));
  JS_RunGC(JS_GetRuntime(ctx));
  JS_RunGC(JS_GetRuntime(ctx));
  return JS_UNDEFINED;
}

static JSValue native_write_stderr(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  size_t len;
  const char *s = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : NULL;
  if (!s) return JS_EXCEPTION;
  fwrite(s, 1, len, stderr);
  fflush(stderr);
  JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

/* vm.runInNewContext(code): evaluates `code` in a new context (realm) of the
 * same runtime and returns the completion value. The context lives as long
 * as the functions and objects created in it. */
static JSValue native_run_in_new_context(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  (void)this_val;
  size_t len;
  const char *code = arg_string(ctx, argc, argv, 0, &len);
  if (!code) return JS_EXCEPTION;
  JSContext *realm = JS_NewContext(JS_GetRuntime(ctx));
  if (!realm) {
    JS_FreeCString(ctx, code);
    return JS_ThrowOutOfMemory(ctx);
  }
  JSValue r = JS_Eval(realm, code, len, "evalmachine.<anonymous>",
                      JS_EVAL_TYPE_GLOBAL);
  JS_FreeCString(ctx, code);
  if (JS_IsException(r)) r = JS_Throw(ctx, JS_GetException(realm));
  JS_FreeContext(realm);
  return r;
}

static void on_timer(uv_timer_t *handle);
static void on_idle(uv_idle_t *handle) { (void)handle; }

/* natives.armTimer(delayMs, referenced): delayMs < 0 stops the timer. */
static JSValue native_arm_timer(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
  (void)this_val;
  double delay = -1;
  if (argc > 0) JS_ToFloat64(ctx, &delay, argv[0]);
  bool refed = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
  if (delay < 0) {
    uv_timer_stop(&g_timer);
  } else {
    if (delay > 2147483647.0) delay = 2147483647.0;
    uv_timer_start(&g_timer, on_timer, (uint64_t)delay, 0);
  }
  if (refed)
    uv_ref((uv_handle_t *)&g_timer);
  else
    uv_unref((uv_handle_t *)&g_timer);
  return JS_UNDEFINED;
}

/* natives.armImmediate(pending, referenced) */
static JSValue native_arm_immediate(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  (void)this_val;
  bool pending = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
  bool refed = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
  if (pending) {
    uv_idle_start(&g_idle, on_idle);
    if (refed)
      uv_ref((uv_handle_t *)&g_idle);
    else
      uv_unref((uv_handle_t *)&g_idle);
  } else {
    uv_idle_stop(&g_idle);
  }
  return JS_UNDEFINED;
}

static const char *platform_name(void) {
#if defined(_WIN32)
  return "win32";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(__FreeBSD__)
  return "freebsd";
#else
  return "linux";
#endif
}

static int set_fn(JSContext *ctx, JSValueConst obj, const char *name,
                  JSCFunction *fn, int length) {
  return JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, length));
}

static void host_checkpoint(qjsnapi_host *host, JSContext *ctx, void *opaque);
static void handle_uncaught(JSContext *ctx, JSValue exc);

/* An event of a child process (spawn.c): a macrotask of its own. */
static void dispatch_callback(JSContext *ctx, JSValueConst fn, int argc,
                              JSValueConst *argv) {
  if (g_exit_requested) return;
  JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
  if (JS_IsException(r))
    handle_uncaught(ctx, JS_GetException(ctx));
  else
    JS_FreeValue(ctx, r);
  host_checkpoint(NULL, ctx, NULL);
}

#if defined(__APPLE__)
#include <crt_externs.h>
static char **host_environ(void) { return *_NSGetEnviron(); }
#elif defined(_WIN32)
static char **host_environ(void) { return _environ; }
#else
extern char **environ;
static char **host_environ(void) { return environ; }
#endif

static JSValue make_natives(JSContext *ctx, int argc, char **argv) {
  JSValue n = JS_NewObject(ctx);
  if (JS_IsException(n)) return n;
  set_fn(ctx, n, "stat", native_stat, 1);
  set_fn(ctx, n, "realpath", native_realpath, 1);
  set_fn(ctx, n, "readFile", native_read_file, 1);
  set_fn(ctx, n, "compile", native_compile, 2);
  set_fn(ctx, n, "loadAddon", native_load_addon, 1);
  set_fn(ctx, n, "cwd", native_cwd, 0);
  set_fn(ctx, n, "exit", native_exit, 1);
  set_fn(ctx, n, "writeStderr", native_write_stderr, 1);
  set_fn(ctx, n, "runInNewContext", native_run_in_new_context, 1);
  set_fn(ctx, n, "armTimer", native_arm_timer, 2);
  set_fn(ctx, n, "armImmediate", native_arm_immediate, 2);
  if (spawn_add_natives(ctx, n, g_loop, dispatch_callback) < 0) {
    JS_FreeValue(ctx, n);
    return JS_EXCEPTION;
  }
  JS_SetPropertyStr(ctx, n, "Buffer",
                    qjsnapi_host_get_buffer(qjsnapi_host_from_context(ctx)));
  const napi_node_version *nv = qjsnapi_node_version();
  char node_version[32];
  snprintf(node_version, sizeof(node_version), "%u.%u.%u", nv->major,
           nv->minor, nv->patch);
  JS_SetPropertyStr(ctx, n, "nodeVersion", JS_NewString(ctx, node_version));
  JS_SetPropertyStr(ctx, n, "platform", JS_NewString(ctx, platform_name()));
  JS_SetPropertyStr(ctx, n, "quickjsVersion", JS_NewString(ctx, QJS_VERSION_STR));
  JS_SetPropertyStr(ctx, n, "napiVersion",
                    JS_NewUint32(ctx, qjsnapi_napi_version()));

  /* process.argv = [execPath, absolute script path, args...] */
  char *exec = real_path(argv[0]);
  JS_SetPropertyStr(ctx, n, "execPath", JS_NewString(ctx, exec ? exec : argv[0]));
  JSValue args = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, args, 0, JS_NewString(ctx, exec ? exec : argv[0]));
  free(exec);
  /* With -e there is no script: argv[1...] are the arguments. */
  int first_arg = 1;
  if (!g_eval_code) {
    char *script = real_path(argv[1]);
    JS_SetPropertyUint32(ctx, args, 1,
                         JS_NewString(ctx, script ? script : argv[1]));
    free(script);
    first_arg = 2;
  }
  for (int i = first_arg; i < argc; i++)
    JS_SetPropertyUint32(ctx, args, (uint32_t)i, JS_NewString(ctx, argv[i]));
  JS_SetPropertyStr(ctx, n, "argv", args);
  /* process.env: the environment as "NAME=value" strings. */
  JSValue env = JS_NewArray(ctx);
  char **envp = host_environ();
  for (uint32_t i = 0; envp && envp[i]; i++)
    JS_SetPropertyUint32(ctx, env, i, JS_NewString(ctx, envp[i]));
  JS_SetPropertyStr(ctx, n, "environ", env);
  JSValue preloads = JS_NewArray(ctx);
  for (int i = 0; i < g_preload_count; i++)
    JS_SetPropertyUint32(ctx, preloads, (uint32_t)i,
                         JS_NewString(ctx, g_preloads[i]));
  JS_SetPropertyStr(ctx, n, "preloads", preloads);
  if (JS_HasException(ctx)) {
    JS_FreeValue(ctx, n);
    return JS_EXCEPTION;
  }
  return n;
}

/* Reports a pending exception unless it is the process.exit() unwinding. */
static int report_exception(JSContext *ctx) {
  if (g_exit_requested) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return g_exit_code;
  }
  js_std_dump_error(ctx);
  return 1;
}

/* An exception no JavaScript frame can catch: hands it to the bootstrap
 * (process 'uncaughtException', or print and process.exit(1)). Takes
 * ownership of `exc`. */
static void handle_uncaught(JSContext *ctx, JSValue exc) {
  if (g_exit_requested || JS_IsUndefined(g_api)) {
    JS_FreeValue(ctx, exc);
    return;
  }
  JSValue fn = JS_GetPropertyStr(ctx, g_api, "fatalException");
  JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&exc);
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, exc);
  if (JS_IsException(r)) {
    if (!g_exit_requested) {
      /* An 'uncaughtException' listener threw: Node.js exits with 7. */
      js_std_dump_error(ctx);
      g_exit_requested = 1;
      g_exit_code = 7;
    } else {
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
  } else {
    JS_FreeValue(ctx, r);
  }
}

static void on_napi_uncaught(qjsnapi_host *host, JSContext *ctx, JSValue exc,
                             void *opaque) {
  (void)host;
  (void)opaque;
  handle_uncaught(ctx, exc);
}

/* Promises rejected with no handler, in rejection order. A rejection handled
 * later in the same turn (is_handled = true) is removed again. */
typedef struct rejection {
  struct rejection *next;
  JSValue promise;
  JSValue reason;
} rejection;

static rejection *g_rejections;

static void rejection_tracker(JSContext *ctx, JSValueConst promise,
                              JSValueConst reason, JS_BOOL is_handled,
                              void *opaque) {
  (void)opaque;
  if (!is_handled) {
    rejection *r = (rejection *)malloc(sizeof(rejection));
    if (!r) return;
    r->promise = JS_DupValue(ctx, promise);
    r->reason = JS_DupValue(ctx, reason);
    r->next = NULL;
    rejection **tail = &g_rejections;
    while (*tail) tail = &(*tail)->next;
    *tail = r;
    return;
  }
  for (rejection **p = &g_rejections; *p; p = &(*p)->next) {
    if (JS_VALUE_GET_PTR((*p)->promise) == JS_VALUE_GET_PTR(promise)) {
      rejection *r = *p;
      *p = r->next;
      JS_FreeValue(ctx, r->promise);
      JS_FreeValue(ctx, r->reason);
      free(r);
      return;
    }
  }
}

static void free_rejections(JSContext *ctx) {
  while (g_rejections) {
    rejection *r = g_rejections;
    g_rejections = r->next;
    JS_FreeValue(ctx, r->promise);
    JS_FreeValue(ctx, r->reason);
    free(r);
  }
}

/* Returns true if at least one rejection was reported. */
static bool process_rejections(JSContext *ctx) {
  bool any = false;
  while (g_rejections && !g_exit_requested) {
    rejection *r = g_rejections;
    g_rejections = r->next;
    any = true;
    JSValue fn = JS_GetPropertyStr(ctx, g_api, "unhandledRejection");
    JSValueConst args[2] = {r->reason, r->promise};
    JSValue res = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, r->promise);
    JS_FreeValue(ctx, r->reason);
    free(r);
    if (JS_IsException(res))
      handle_uncaught(ctx, JS_GetException(ctx));
    else
      JS_FreeValue(ctx, res);
  }
  return any;
}

/* Promise jobs, then unhandled rejections, until both are empty (Node:
 * processTicksAndRejections). Also the library's checkpoint. */
static void host_checkpoint(qjsnapi_host *host, JSContext *ctx, void *opaque) {
  (void)host;
  (void)opaque;
  JSRuntime *rt = JS_GetRuntime(ctx);
  while (!g_exit_requested) {
    JSContext *job_ctx;
    int r = JS_ExecutePendingJob(rt, &job_ctx);
    if (r < 0) {
      handle_uncaught(job_ctx, JS_GetException(job_ctx));
      continue;
    }
    if (r > 0) continue;
    if (!process_rejections(ctx)) break;
  }
  /* End of the macrotask (see qjsnapi_host_clear_kept_objects). */
  qjsnapi_host_clear_kept_objects(qjsnapi_host_from_context(ctx));
  if (g_exit_requested) uv_stop(g_loop);
}

/* Calls a function of g_api without arguments; an exception is uncaught.
 * Returns its result converted to a boolean (false on exception). */
static bool call_api(const char *name, int32_t *int_result) {
  JSContext *ctx = g_ctx;
  JSValue fn = JS_GetPropertyStr(ctx, g_api, name);
  JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
  JS_FreeValue(ctx, fn);
  if (JS_IsException(r)) {
    handle_uncaught(ctx, JS_GetException(ctx));
    return false;
  }
  bool b = JS_ToBool(ctx, r) > 0;
  if (int_result) JS_ToInt32(ctx, int_result, r);
  JS_FreeValue(ctx, r);
  return b;
}

/* Timers phase: every due timer, one at a time. */
static void on_timer(uv_timer_t *handle) {
  (void)handle;
  while (!g_exit_requested && call_api("runDueTimer", NULL))
    host_checkpoint(NULL, g_ctx, NULL);
  host_checkpoint(NULL, g_ctx, NULL);
}

/* Check phase: the immediates queued before it began. */
static void on_check(uv_check_t *handle) {
  (void)handle;
  int32_t n = 0;
  call_api("immediateCount", &n);
  for (int32_t i = 0; i < n && !g_exit_requested; i++) {
    if (!call_api("runImmediate", NULL)) break;
    host_checkpoint(NULL, g_ctx, NULL);
  }
}

static void run_loop(JSContext *ctx) {
  (void)ctx;
  while (!g_exit_requested) {
    uv_run(g_loop, UV_RUN_DEFAULT);
    if (g_exit_requested) break;
    /* process 'beforeExit': listeners may schedule more work. */
    call_api("emitBeforeExit", NULL);
    host_checkpoint(NULL, g_ctx, NULL);
    if (!uv_loop_alive(g_loop)) break;
  }
}

static int run(JSContext *ctx, qjsnapi_host *host, int argc, char **argv) {
  JSValue boot = JS_Eval(ctx, (const char *)qn_bootstrap_js, qn_bootstrap_js_len,
                         "quickjs-napi:bootstrap", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(boot)) return report_exception(ctx);
  JSValue natives = make_natives(ctx, argc, argv);
  if (JS_IsException(natives)) {
    JS_FreeValue(ctx, boot);
    return report_exception(ctx);
  }
  JSValue api = JS_Call(ctx, boot, JS_UNDEFINED, 1, &natives);
  JS_FreeValue(ctx, natives);
  JS_FreeValue(ctx, boot);
  if (JS_IsException(api)) return report_exception(ctx);
  g_api = api;
  JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), rejection_tracker, NULL);
  qjsnapi_host_set_uncaught_handler(host, on_napi_uncaught, NULL);
  qjsnapi_host_set_checkpoint(host, host_checkpoint, NULL);

  JSValue run_main =
      JS_GetPropertyStr(ctx, api, g_eval_code ? "runEval" : "runMain");
  JSValue script = JS_NewString(ctx, g_eval_code ? g_eval_code : argv[1]);
  JSValue r = JS_Call(ctx, run_main, JS_UNDEFINED, 1, &script);
  JS_FreeValue(ctx, script);
  JS_FreeValue(ctx, run_main);
  if (JS_IsException(r))
    handle_uncaught(ctx, JS_GetException(ctx));
  else
    JS_FreeValue(ctx, r);
  host_checkpoint(host, ctx, NULL);
  run_loop(ctx);

  int code = 0;
  if (!g_exit_requested) {
    /* process 'exit' listeners, then process.exitCode */
    JSValue emit_exit = JS_GetPropertyStr(ctx, api, "emitExit");
    JSValue c = JS_Call(ctx, emit_exit, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, emit_exit);
    if (JS_IsException(c)) {
      code = report_exception(ctx);
    } else {
      int32_t exit_code = 0;
      JS_ToInt32(ctx, &exit_code, c);
      JS_FreeValue(ctx, c);
      code = exit_code;
    }
  } else {
    code = g_exit_code;
  }
  /* No JavaScript runs from here on: finalizers run at host destruction
   * with JavaScript disabled, as during Node.js environment teardown. */
  qjsnapi_host_set_uncaught_handler(host, NULL, NULL);
  qjsnapi_host_set_checkpoint(host, NULL, NULL);
  JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), NULL, NULL);
  free_rejections(ctx);
  g_api = JS_UNDEFINED;
  JS_FreeValue(ctx, api);
  return code;
}

static JSValue js_gc_fn(JSContext *ctx) {
  return JS_NewCFunction(ctx, js_gc, "gc", 0);
}

static void close_host_handles(void) {
  uv_close((uv_handle_t *)&g_timer, NULL);
  uv_close((uv_handle_t *)&g_check, NULL);
  uv_close((uv_handle_t *)&g_idle, NULL);
}

static const char kForcePolicy[] = "--force-node-api-uncaught-exceptions-policy";
static const char kNoForcePolicy[] =
    "--no-force-node-api-uncaught-exceptions-policy";


/* Parses the option argv[*i] (and its value, advancing *i). Returns 1 when
 * -e ends the options, 0 otherwise, -1 after printing an error. In
 * NODE_OPTIONS, shared with Node.js, the options quickjs-napi does not know
 * (--max-old-space-size, --use-openssl-ca, ...) and -e are ignored. */
static int parse_option(int argc, char **argv, int *i, int *force_policy,
                        bool from_env) {
  const char *opt = argv[*i];
  size_t n = strlen(kForcePolicy);
  bool is_eval = strcmp(opt, "-e") == 0 || strcmp(opt, "--eval") == 0;
  bool is_require = strcmp(opt, "-r") == 0 || strcmp(opt, "--require") == 0;
  if ((is_eval && !from_env) || is_require) {
    if (*i + 1 >= argc) {
      fprintf(stderr, "quickjs-napi: %s requires an argument\n", opt);
      return -1;
    }
    const char *value = argv[++*i];
    if (is_eval) {
      g_eval_code = value;
      return 1;
    }
    opt = value;
  } else if (strncmp(opt, "--require=", 10) == 0) {
    opt += 10;
    is_require = true;
  }
  if (is_require) {
    if (g_preload_count == MAX_PRELOADS) {
      fprintf(stderr, "quickjs-napi: too many --require options\n");
      return -1;
    }
    g_preloads[g_preload_count++] = opt;
  } else if (strcmp(opt, "--expose-gc") == 0 ||
             strcmp(opt, "--expose_gc") == 0) {
    /* gc() is always available */
  } else if (strcmp(opt, kNoForcePolicy) == 0) {
    *force_policy = 0;
  } else if (strncmp(opt, kForcePolicy, n) == 0 &&
             (opt[n] == '\0' || strcmp(opt + n, "=true") == 0)) {
    *force_policy = 1;
  } else if (strncmp(opt, kForcePolicy, n) == 0 &&
             strcmp(opt + n, "=false") == 0) {
    *force_policy = 0;
  } else if (!from_env) {
    fprintf(stderr, "quickjs-napi: bad option: %s\n", opt);
    return -1;
  }
  return 0;
}

/* NODE_OPTIONS: options separated by spaces (no quoting), read before the
 * command line. Returns -1 after printing an error. */
static int parse_node_options(int *force_policy) {
  const char *env = getenv("NODE_OPTIONS");
  if (env == NULL || env[0] == '\0') return 0;
  g_node_options_copy = strdup(env);
  if (g_node_options_copy == NULL) return -1;
  char *tokens[64];
  int count = 0;
  for (char *t = strtok(g_node_options_copy, " \t"); t;
       t = strtok(NULL, " \t")) {
    if (count == 64) {
      fprintf(stderr, "quickjs-napi: NODE_OPTIONS has too many options\n");
      return -1;
    }
    tokens[count++] = t;
  }
  for (int i = 0; i < count; i++)
    if (parse_option(count, tokens, &i, force_policy, true) < 0) return -1;
  return 0;
}

/* Options before the script. Returns the index of the script in argv (with
 * -e: of the first argument), or -1 after printing an error. */
static int parse_options(int argc, char **argv, int *force_policy) {
  if (parse_node_options(force_policy) < 0) return -1;
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
    int r = parse_option(argc, argv, &i, force_policy, false);
    if (r < 0) return -1;
    if (r == 1) return i + 1; /* -e: what follows are the arguments */
  }
  return i;
}

int main(int argc, char **argv) {
  int force_policy = 0;
  int script = parse_options(argc, argv, &force_policy);
  if (script < 0) return 9; /* Node.js: invalid command-line option */
  if (script >= argc && !g_eval_code) {
    fprintf(stderr,
            "usage: %s [options] <script.js> [args...]\n"
            "quickjs-napi: Node-API %u for QuickJS %s\n",
            argv[0], qjsnapi_napi_version(), QJS_VERSION_STR);
    return 2;
  }
  /* From here on argv is [executable, script, args...], or with -e
   * [executable, args...]. */
  argv[script - 1] = argv[0];
  argv += script - 1;
  argc -= script - 1;

  g_api = JS_UNDEFINED;
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    fprintf(stderr, "quickjs-napi: cannot allocate the JS runtime\n");
    return 1;
  }
  js_std_init_handlers(rt);
  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    fprintf(stderr, "quickjs-napi: cannot allocate the JS context\n");
    js_std_free_handlers(rt);
    JS_FreeRuntime(rt);
    return 1;
  }
  /* scriptArgs = [script, args...], as in the quickjs executable. */
  js_std_add_helpers(ctx, argc - 1, argv + 1);

  int exit_code;
  qjsnapi_host *host = qjsnapi_host_create(ctx);
  if (!host) {
    js_std_dump_error(ctx);
    exit_code = 1;
  } else {
    g_ctx = ctx;
    g_loop = qjsnapi_host_get_loop(host);
    qjsnapi_host_set_force_uncaught_exceptions_policy(host, force_policy);
    uv_timer_init(g_loop, &g_timer);
    uv_check_init(g_loop, &g_check);
    uv_idle_init(g_loop, &g_idle);
    uv_check_start(&g_check, on_check);
    uv_unref((uv_handle_t *)&g_check);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "gc", js_gc_fn(ctx));
    JS_FreeValue(ctx, global);
    exit_code = run(ctx, host, argc, argv);
    if (g_exit_requested) {
      /* process.exit() and fatal exceptions: like Node.js
       * (DefaultProcessExitHandlerInternal), the process ends without
       * tearing the environment down. No cleanup hook or finalizer runs,
       * work in the thread pool is not waited for (its completion callback
       * would run with JavaScript disabled), only the thread pool is shut
       * down. The 'exit' event has already been emitted. */
      fflush(stdout);
      fflush(stderr);
      uv_library_shutdown();
#if QN_LEAK_SANITIZER
      /* What is left allocated is not freed by design (as in Node.js), so
       * LeakSanitizer, which checks at exit(), is bypassed. */
      _exit(exit_code);
#else
      exit(exit_code);
#endif
    }
    spawn_close_all();
    close_host_handles();
    qjsnapi_host_destroy(host);
    g_loop = NULL;
  }

  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  free(g_node_options_copy);
  return exit_code;
}
