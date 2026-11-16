/*
 * quickjs-napi host executable.
 *
 *   quickjs-napi <script.js> [args...]
 *
 * Runs the script as a CommonJS module (host/bootstrap.js): `require` loads
 * .js, .json and Node-API addons (.node), with Node.js resolution rules for
 * relative paths, directories and node_modules. Globals: `process` (subset),
 * `global`, `gc()`, timers (setTimeout, setInterval, setImmediate,
 * queueMicrotask), plus the quickjs-libc helpers (console.log, print,
 * scriptArgs).
 *
 * Event loop (run_loop), one iteration:
 *   1. microtasks (promise jobs)
 *   2. unhandled promise rejections -> process 'unhandledRejection', else
 *      uncaught exception (Node.js default `--unhandled-rejections=throw`)
 *   3. Node-API finalizers queued by the GC (Node: a SetImmediate callback)
 *   4. one due timer, else one immediate (bootstrap.js runTimers)
 *   5. sleep until the next timer when nothing else is pending
 * An uncaught exception goes to process 'uncaughtException' listeners or
 * prints the error and exits with code 1, as in Node.js.
 *
 * The executable exports every napi_* symbol (see CMakeLists.txt): addons
 * built for Node.js resolve them from this process.
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

#include "bootstrap_js.h" /* generated: qn_bootstrap_js, qn_bootstrap_js_len */
#include "qjsnapi.h"
#include "quickjs-libc.h"
#include "quickjs.h"

#ifndef QJS_VERSION_STR
#define QJS_VERSION_STR "unknown"
#endif

static int g_exit_requested;
static int g_exit_code;
/* Object returned by bootstrap.js: runMain, emitExit, runTimers,
 * fatalException, unhandledRejection. */
static JSValue g_api;

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
  char *script = real_path(argv[1]);
  JS_SetPropertyUint32(ctx, args, 1, JS_NewString(ctx, script ? script : argv[1]));
  free(script);
  for (int i = 2; i < argc; i++)
    JS_SetPropertyUint32(ctx, args, (uint32_t)i, JS_NewString(ctx, argv[i]));
  JS_SetPropertyStr(ctx, n, "argv", args);
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

static void sleep_ms(double ms) {
  if (ms > 1000.0 * 60) ms = 1000.0 * 60;
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000.0);
  ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
  }
#endif
}

static void run_loop(JSContext *ctx, qjsnapi_host *host) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  JSValue run_timers = JS_GetPropertyStr(ctx, g_api, "runTimers");
  while (!g_exit_requested) {
    /* 1. microtasks */
    for (;;) {
      JSContext *job_ctx;
      int r = JS_ExecutePendingJob(rt, &job_ctx);
      if (r == 0) break;
      if (r < 0) handle_uncaught(job_ctx, JS_GetException(job_ctx));
      if (g_exit_requested) break;
    }
    if (g_exit_requested) break;
    /* 2. unhandled rejections */
    if (process_rejections(ctx)) continue;
    /* 3. Node-API finalizers */
    if (qjsnapi_host_run_finalizers(host) > 0) continue;
    /* 4. timers and immediates */
    JSValue r = JS_Call(ctx, run_timers, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) {
      handle_uncaught(ctx, JS_GetException(ctx));
      continue;
    }
    double wait = -1;
    JS_ToFloat64(ctx, &wait, r);
    JS_FreeValue(ctx, r);
    if (wait == 0 || JS_IsJobPending(rt) ||
        qjsnapi_host_has_pending_finalizers(host))
      continue;
    if (wait < 0) break; /* nothing left to do */
    /* 5. wait for the next timer */
    sleep_ms(wait);
  }
  JS_FreeValue(ctx, run_timers);
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

  JSValue run_main = JS_GetPropertyStr(ctx, api, "runMain");
  JSValue script = JS_NewString(ctx, argv[1]);
  JSValue r = JS_Call(ctx, run_main, JS_UNDEFINED, 1, &script);
  JS_FreeValue(ctx, script);
  JS_FreeValue(ctx, run_main);
  if (JS_IsException(r))
    handle_uncaught(ctx, JS_GetException(ctx));
  else
    JS_FreeValue(ctx, r);
  run_loop(ctx, host);

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
  JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), NULL, NULL);
  free_rejections(ctx);
  g_api = JS_UNDEFINED;
  JS_FreeValue(ctx, api);
  return code;
}

static JSValue js_gc_fn(JSContext *ctx) {
  return JS_NewCFunction(ctx, js_gc, "gc", 0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s <script.js> [args...]\n"
            "quickjs-napi: Node-API %u for QuickJS %s\n",
            argv[0], qjsnapi_napi_version(), QJS_VERSION_STR);
    return 2;
  }

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
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "gc", js_gc_fn(ctx));
    JS_FreeValue(ctx, global);
    exit_code = run(ctx, host, argc, argv);
    qjsnapi_host_destroy(host);
  }

  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return exit_code;
}
