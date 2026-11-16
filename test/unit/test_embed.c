/*
 * Embedding test: drives Node-API directly through a statically linked addon
 * (qjsnapi_init_static_addon) and checks behaviour that a JS test cannot
 * observe: teardown order, handle-scope misuse, stubs, error state, and the
 * finalizer queue of the embedding API (qjsnapi_host_run_finalizers).
 *
 * Built against a Debug QuickJS: JS_FreeRuntime asserts that every object
 * was released, so any leaked JSValue aborts this test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node_api.h"
#include "qjsnapi.h"
#include "quickjs.h"

static int failures;
static char teardown_log[256];
static qjsnapi_host *g_host;
static JSRuntime *g_rt;
static int external_finalized;

#define EXPECT(cond)                                               \
  do {                                                             \
    if (!(cond)) {                                                 \
      fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, \
              __LINE__, #cond);                                    \
      failures++;                                                  \
    }                                                              \
  } while (0)

static void log_event(const char *what) {
  strncat(teardown_log, what, sizeof(teardown_log) - strlen(teardown_log) - 1);
}

static void hook_a(void *arg) { (void)arg; log_event("A"); }
static void hook_b(void *arg) { (void)arg; log_event("B"); }
static void hook_removed(void *arg) { (void)arg; log_event("X"); }
static void instance_finalizer(napi_env env, void *data, void *hint) {
  (void)env;
  EXPECT(data == (void *)0x1234);
  EXPECT(hint == (void *)0x5678);
  log_event("I");
}

static void external_finalizer(napi_env env, void *data, void *hint) {
  (void)env;
  (void)hint;
  EXPECT(data == (void *)0xE1);
  external_finalized++;
}

/* Runs at teardown: JavaScript is disabled (module API version 10). */
static void wrap_finalizer(napi_env env, void *data, void *hint) {
  (void)hint;
  EXPECT(data == (void *)0xA1);
  napi_value global, v;
  EXPECT(napi_get_global(env, &global) == napi_ok);
  EXPECT(napi_get_named_property(env, global, "Object", &v) ==
         napi_cannot_run_js);
  log_event("W");
}

/* Queued finalizers: not run by the GC, run by qjsnapi_host_run_finalizers. */
static void check_lifetime(napi_env env, napi_value exports) {
  napi_handle_scope scope;
  EXPECT(napi_open_handle_scope(env, &scope) == napi_ok);
  napi_value ext;
  EXPECT(napi_create_external(env, (void *)0xE1, external_finalizer, NULL,
                              &ext) == napi_ok);
  napi_valuetype t;
  EXPECT(napi_typeof(env, ext, &t) == napi_ok && t == napi_external);
  napi_ref weak;
  EXPECT(napi_create_reference(env, ext, 0, &weak) == napi_ok);
  EXPECT(napi_close_handle_scope(env, scope) == napi_ok);

  /* The external is dead: the weak reference sees it at once ... */
  napi_value v = (napi_value)(void *)0x1;
  EXPECT(napi_get_reference_value(env, weak, &v) == napi_ok && v == NULL);
  EXPECT(napi_delete_reference(env, weak) == napi_ok);
  /* ... but its finalizer only runs from the event loop, after a GC. */
  EXPECT(external_finalized == 0);
  JS_RunGC(g_rt);
  EXPECT(external_finalized == 0);
  EXPECT(qjsnapi_host_has_pending_finalizers(g_host));
  EXPECT(qjsnapi_host_run_finalizers(g_host) == 1);
  EXPECT(external_finalized == 1);
  EXPECT(!qjsnapi_host_has_pending_finalizers(g_host));

  /* A wrapped object still alive at teardown: finalized then, before the
   * (older) instance data. */
  napi_value obj;
  EXPECT(napi_create_object(env, &obj) == napi_ok);
  EXPECT(napi_wrap(env, obj, (void *)0xA1, wrap_finalizer, NULL, NULL) ==
         napi_ok);
  void *native = NULL;
  EXPECT(napi_unwrap(env, obj, &native) == napi_ok && native == (void *)0xA1);
  EXPECT(napi_wrap(env, obj, (void *)0xA2, wrap_finalizer, NULL, NULL) ==
         napi_invalid_arg);
  EXPECT(napi_set_named_property(env, exports, "wrapped", obj) == napi_ok);
}

static void check_scopes(napi_env env) {
  napi_handle_scope outer, inner;
  napi_value v;

  EXPECT(napi_close_handle_scope(env, NULL) == napi_invalid_arg);

  EXPECT(napi_open_handle_scope(env, &outer) == napi_ok);
  EXPECT(napi_open_handle_scope(env, &inner) == napi_ok);
  /* Out-of-order close is a mismatch and leaves both scopes open. */
  EXPECT(napi_close_handle_scope(env, outer) == napi_handle_scope_mismatch);
  EXPECT(napi_close_handle_scope(env, inner) == napi_ok);
  EXPECT(napi_close_handle_scope(env, outer) == napi_ok);
  /* Nothing open any more. */
  EXPECT(napi_close_handle_scope(env, outer) == napi_handle_scope_mismatch);

  /* Escapable scope closed with the wrong API. */
  napi_escapable_handle_scope esc;
  EXPECT(napi_open_escapable_handle_scope(env, &esc) == napi_ok);
  EXPECT(napi_close_handle_scope(env, (napi_handle_scope)esc) ==
         napi_invalid_arg);
  EXPECT(napi_close_escapable_handle_scope(env, esc) == napi_ok);

  /* 10 000 handles in nested scopes: many arena chunks, all released. */
  EXPECT(napi_open_handle_scope(env, &outer) == napi_ok);
  for (int i = 0; i < 5000; i++) EXPECT(napi_create_object(env, &v) == napi_ok);
  EXPECT(napi_open_handle_scope(env, &inner) == napi_ok);
  for (int i = 0; i < 5000; i++) EXPECT(napi_create_object(env, &v) == napi_ok);
  EXPECT(napi_close_handle_scope(env, inner) == napi_ok);
  EXPECT(napi_close_handle_scope(env, outer) == napi_ok);

  /* A scope deliberately left open: the enclosing initializer scope releases
   * it instead of keeping 100 objects alive until teardown. */
  EXPECT(napi_open_handle_scope(env, &outer) == napi_ok);
  for (int i = 0; i < 100; i++) EXPECT(napi_create_object(env, &v) == napi_ok);
}

static void check_errors(napi_env env) {
  const napi_extended_error_info *info;
  napi_value v;

  EXPECT(napi_create_string_utf8(env, NULL, 1, &v) == napi_invalid_arg);
  EXPECT(napi_get_last_error_info(env, &info) == napi_ok);
  EXPECT(info->error_code == napi_invalid_arg);
  EXPECT(strcmp(info->error_message, "Invalid argument") == 0);

  /* A successful call clears the last error. */
  EXPECT(napi_get_undefined(env, &v) == napi_ok);
  EXPECT(napi_get_last_error_info(env, &info) == napi_ok);
  EXPECT(info->error_code == napi_ok);
  EXPECT(info->error_message == NULL);

  /* Stubs report themselves by name (napi_get_uv_event_loop stays a stub
   * until the event loop milestone, M5). */
  struct uv_loop_s *loop = NULL;
  EXPECT(napi_get_uv_event_loop(env, &loop) == napi_generic_failure);
  EXPECT(napi_get_last_error_info(env, &info) == napi_ok);
  EXPECT(info->error_code == napi_generic_failure);
  EXPECT(info->error_message != NULL &&
         strstr(info->error_message, "napi_get_uv_event_loop") != NULL);

  /* NULL env never crashes. */
  EXPECT(napi_get_undefined(NULL, &v) == napi_invalid_arg);
  EXPECT(napi_get_last_error_info(NULL, &info) == napi_invalid_arg);

  uint32_t version = 0;
  EXPECT(napi_get_version(env, &version) == napi_ok && version == 10);
  const napi_node_version *nv;
  EXPECT(napi_get_node_version(env, &nv) == napi_ok && nv->major >= 22);
  const char *file;
  EXPECT(node_api_get_module_file_name(env, &file) == napi_ok);
  EXPECT(file != NULL && strcmp(file, "static:test") == 0);
}

/* More than QN_STACK_ARGS (8) arguments take the heap-allocated path of
 * napi_call_function / napi_new_instance. */
static void check_many_args(napi_env env) {
  napi_value global, math, max, array_ctor, args[12], r;
  EXPECT(napi_get_global(env, &global) == napi_ok);
  EXPECT(napi_get_named_property(env, global, "Math", &math) == napi_ok);
  EXPECT(napi_get_named_property(env, math, "max", &max) == napi_ok);
  for (int i = 0; i < 12; i++) EXPECT(napi_create_int32(env, i + 1, &args[i]) == napi_ok);
  EXPECT(napi_call_function(env, math, max, 12, args, &r) == napi_ok);
  int32_t v = 0;
  EXPECT(napi_get_value_int32(env, r, &v) == napi_ok && v == 12);
  /* The result is optional. */
  EXPECT(napi_call_function(env, math, max, 12, args, NULL) == napi_ok);

  EXPECT(napi_get_named_property(env, global, "Array", &array_ctor) == napi_ok);
  EXPECT(napi_new_instance(env, array_ctor, 10, args, &r) == napi_ok);
  uint32_t len = 0;
  EXPECT(napi_get_array_length(env, r, &len) == napi_ok && len == 10);

  /* Not a function: napi_invalid_arg, no exception (CHECK_TO_FUNCTION). */
  bool pending = true;
  EXPECT(napi_call_function(env, global, math, 0, NULL, &r) == napi_invalid_arg);
  EXPECT(napi_is_exception_pending(env, &pending) == napi_ok && !pending);
}

static napi_value init(napi_env env, napi_value exports) {
  check_scopes(env);
  check_errors(env);
  check_many_args(env);

  EXPECT(napi_set_instance_data(env, (void *)0x1234, instance_finalizer,
                                (void *)0x5678) == napi_ok);
  void *data = NULL;
  EXPECT(napi_get_instance_data(env, &data) == napi_ok &&
         data == (void *)0x1234);

  EXPECT(napi_add_env_cleanup_hook(env, hook_a, NULL) == napi_ok);
  EXPECT(napi_add_env_cleanup_hook(env, hook_removed, NULL) == napi_ok);
  EXPECT(napi_add_env_cleanup_hook(env, hook_b, NULL) == napi_ok);
  EXPECT(napi_remove_env_cleanup_hook(env, hook_removed, NULL) == napi_ok);

  napi_value s;
  EXPECT(napi_create_string_utf8(env, "static", NAPI_AUTO_LENGTH, &s) ==
         napi_ok);
  EXPECT(napi_set_named_property(env, exports, "kind", s) == napi_ok);
  check_lifetime(env, exports);
  return exports;
}

int main(void) {
  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  qjsnapi_host *host = qjsnapi_host_create(ctx);
  g_rt = rt;
  g_host = host;
  EXPECT(host != NULL);
  EXPECT(qjsnapi_host_from_context(ctx) == host);
  EXPECT(qjsnapi_host_create(ctx) == NULL); /* opaque already in use */
  if (JS_HasException(ctx)) JS_FreeValue(ctx, JS_GetException(ctx));

  JSValue exports;
  EXPECT(qjsnapi_init_static_addon(host, "static:test", init, 10, &exports) ==
         0);
  JSValue kind = JS_GetPropertyStr(ctx, exports, "kind");
  const char *k = JS_ToCString(ctx, kind);
  EXPECT(k && strcmp(k, "static") == 0);
  JS_FreeCString(ctx, k);
  JS_FreeValue(ctx, kind);

  /* Cached: same exports object, initializer not run twice. */
  JSValue again;
  EXPECT(qjsnapi_init_static_addon(host, "static:test", init, 10, &again) ==
         0);
  EXPECT(JS_StrictEq(ctx, exports, again));
  JS_FreeValue(ctx, again);
  JS_FreeValue(ctx, exports);

  qjsnapi_host_destroy(host);
  /* Cleanup hooks LIFO, removed hook skipped, then the references with a
   * finalizer, newest first: the wrap, then the instance data. */
  EXPECT(strcmp(teardown_log, "BAWI") == 0);
  EXPECT(JS_GetContextOpaque(ctx) == NULL);

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt); /* asserts on leaked objects in Debug builds */

  if (failures) {
    fprintf(stderr, "test_embed: %d failure(s)\n", failures);
    return 1;
  }
  printf("ok test_embed\n");
  return 0;
}
