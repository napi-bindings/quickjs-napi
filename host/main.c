/*
 * quickjs-napi host executable.
 *
 *   quickjs-napi <script.js> [args...]
 *
 * Runs a script with QuickJS (quickjs-libc helpers: console.log, print,
 * scriptArgs) plus:
 *   require(path)  loads a `.node` addon; relative paths are resolved against
 *                  the directory of the main script, like Node.js resolves
 *                  them against the requiring module
 *   gc()           runs the QuickJS cycle collector
 *
 * The executable exports every napi_* symbol (see CMakeLists.txt): addons
 * built for Node.js resolve them from this process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs-libc.h"
#include "quickjs.h"
#include "qjsnapi.h"

static char *g_script_dir;

static int is_path_separator(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

static int is_absolute(const char *p) {
#ifdef _WIN32
  if (p[0] && p[1] == ':') return 1;
#endif
  return is_path_separator(p[0]);
}

static char *dir_of(const char *path) {
  const char *last = NULL;
  for (const char *p = path; *p; p++)
    if (is_path_separator(*p)) last = p;
  if (!last) {
    char *d = (char *)malloc(2);
    if (d) strcpy(d, ".");
    return d;
  }
  size_t n = (size_t)(last - path);
  if (n == 0) n = 1; /* "/script.js" */
  char *d = (char *)malloc(n + 1);
  if (d) {
    memcpy(d, path, n);
    d[n] = '\0';
  }
  return d;
}

static int ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && strcmp(s + a - b, suffix) == 0;
}

static JSValue js_require(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  (void)this_val;
  if (argc < 1 || !JS_IsString(argv[0]))
    return JS_ThrowTypeError(ctx, "require: the path must be a string");
  const char *spec = JS_ToCString(ctx, argv[0]);
  if (!spec) return JS_EXCEPTION;
  if (!ends_with(spec, ".node")) {
    JS_ThrowTypeError(ctx,
                      "require: only Node-API addons (.node) are supported, "
                      "got '%s'",
                      spec);
    JS_FreeCString(ctx, spec);
    return JS_EXCEPTION;
  }
  char *path = NULL;
  if (!is_absolute(spec) && spec[0] == '.' && g_script_dir) {
    size_t n = strlen(g_script_dir) + 1 + strlen(spec) + 1;
    path = (char *)malloc(n);
    if (path) snprintf(path, n, "%s/%s", g_script_dir, spec);
  } else {
    size_t n = strlen(spec) + 1;
    path = (char *)malloc(n);
    if (path) memcpy(path, spec, n);
  }
  JS_FreeCString(ctx, spec);
  if (!path) return JS_ThrowOutOfMemory(ctx);

  JSValue exports;
  int r = qjsnapi_load_addon(qjsnapi_host_from_context(ctx), path, &exports);
  free(path);
  return r < 0 ? JS_EXCEPTION : exports;
}

static JSValue js_gc(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  JS_RunGC(JS_GetRuntime(ctx));
  return JS_UNDEFINED;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s <script.js> [args...]\n"
            "quickjs-napi: Node-API %u for QuickJS\n",
            argv[0], qjsnapi_napi_version());
    return 2;
  }
  const char *script = argv[1];

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

  int exit_code = 0;
  qjsnapi_host *host = qjsnapi_host_create(ctx);
  if (!host) {
    js_std_dump_error(ctx);
    exit_code = 1;
    goto done;
  }

  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "require",
                    JS_NewCFunction(ctx, js_require, "require", 1));
  JS_SetPropertyStr(ctx, global, "gc", JS_NewCFunction(ctx, js_gc, "gc", 0));
  JS_FreeValue(ctx, global);

  g_script_dir = dir_of(script);
  size_t len;
  uint8_t *buf = js_load_file(ctx, &len, script);
  if (!buf) {
    fprintf(stderr, "quickjs-napi: cannot read '%s'\n", script);
    exit_code = 1;
  } else {
    JSValue r =
        JS_Eval(ctx, (const char *)buf, len, script, JS_EVAL_TYPE_GLOBAL);
    js_free(ctx, buf);
    if (JS_IsException(r)) {
      js_std_dump_error(ctx);
      exit_code = 1;
    } else {
      JS_FreeValue(ctx, r);
      js_std_loop(ctx); /* pending jobs and timers */
    }
  }
  qjsnapi_host_destroy(host);

done:
  free(g_script_dir);
  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return exit_code;
}
