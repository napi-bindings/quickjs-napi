/*
 * Edge cases of the M4 APIs (Buffer, async context, napi_make_callback,
 * callback scopes, module file name). test/js/test_node_api.js checks the
 * results under quickjs-napi and, as the reference, under Node.js.
 *
 * A call that fails reports {status, exc}, where exc is the pending
 * exception (cleared) or undefined.
 */
#include <node_api.h>
#include <stdio.h>
#include <string.h>

#define ARGS(n)                                                      \
  size_t argc = (n);                                                 \
  napi_value argv[(n) > 0 ? (n) : 1];                                \
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL)

static napi_value num(napi_env env, double d) {
  napi_value v;
  napi_create_double(env, d, &v);
  return v;
}

static napi_value boolean(napi_env env, bool b) {
  napi_value v;
  napi_get_boolean(env, b, &v);
  return v;
}

static void set(napi_env env, napi_value o, const char *k, napi_value v) {
  napi_set_named_property(env, o, k, v);
}

static napi_value failure(napi_env env, napi_status s) {
  napi_value o, exc = NULL;
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  if (pending) napi_get_and_clear_last_exception(env, &exc);
  napi_create_object(env, &o);
  set(env, o, "status", num(env, s));
  if (exc) set(env, o, "exc", exc);
  return o;
}

static int64_t to_i64(napi_env env, napi_value v) {
  int64_t x = 0;
  napi_get_value_int64(env, v, &x);
  return x;
}

/* ---- Buffer -------------------------------------------------------------- */

/* bufferInfo(v) -> {status, isBuffer, length, first} */
static napi_value BufferInfo(napi_env env, napi_callback_info info) {
  ARGS(1);
  bool is_buffer = false;
  void *data = (void *)1;
  size_t length = 99;
  napi_is_buffer(env, argv[0], &is_buffer);
  napi_status s = napi_get_buffer_info(env, argv[0], &data, &length);
  napi_value o;
  napi_create_object(env, &o);
  set(env, o, "status", num(env, s));
  set(env, o, "isBuffer", boolean(env, is_buffer));
  if (s == napi_ok) {
    set(env, o, "length", num(env, (double)length));
    set(env, o, "first",
        num(env, data && length ? ((unsigned char *)data)[0] : -1));
  }
  return o;
}

/* Also reports an exception left pending with napi_ok (Node.js does that
 * for an out-of-range offset + length). */
static napi_value FromArrayBuffer(napi_env env, napi_callback_info info) {
  ARGS(3);
  napi_value r = NULL;
  napi_status s = node_api_create_buffer_from_arraybuffer(
      env, argv[0], (size_t)to_i64(env, argv[1]), (size_t)to_i64(env, argv[2]),
      &r);
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  return s == napi_ok && !pending ? r : failure(env, s);
}

/* copy(string): a buffer copied from the string's UTF-8 bytes. */
static napi_value Copy(napi_env env, napi_callback_info info) {
  ARGS(1);
  char text[64];
  size_t n = 0;
  napi_get_value_string_utf8(env, argv[0], text, sizeof text, &n);
  void *data = NULL;
  napi_value r;
  napi_status s = napi_create_buffer_copy(env, n, text, &data, &r);
  if (s != napi_ok) return failure(env, s);
  if (data == NULL || memcmp(data, text, n) != 0) return num(env, -1);
  return r;
}

/* create(n): a new buffer whose first byte is set from C. */
static napi_value Create(napi_env env, napi_callback_info info) {
  ARGS(1);
  void *data = NULL;
  napi_value r;
  size_t n = (size_t)to_i64(env, argv[0]);
  napi_status s = napi_create_buffer(env, n, &data, &r);
  if (s != napi_ok) return failure(env, s);
  if (n > 0) ((unsigned char *)data)[0] = 42;
  return r;
}

static char external_text[] = "external";
static int finalized = 0;

static void count_finalize(napi_env env, void *data, void *hint) {
  (void)env;
  (void)hint;
  if (data == external_text) finalized++;
}

static napi_value External(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value r;
  napi_status s = napi_create_external_buffer(
      env, strlen(external_text), external_text, count_finalize, NULL, &r);
  return s == napi_ok ? r : failure(env, s);
}

static napi_value Finalized(napi_env env, napi_callback_info info) {
  (void)info;
  return num(env, finalized);
}

/* ---- Async context, make_callback, callback scopes ------------------------ */

/* makeCallback(resource, recv, fn, ...args) */
static napi_value MakeCallback(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value argv[8];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  napi_value name;
  napi_create_string_utf8(env, "test", NAPI_AUTO_LENGTH, &name);
  napi_async_context context;
  napi_status s = napi_async_init(env, argv[0], name, &context);
  if (s != napi_ok) return failure(env, s);
  napi_value r;
  s = napi_make_callback(env, context, argv[1], argv[2], argc - 3, argv + 3,
                         &r);
  napi_value out = s == napi_ok ? r : failure(env, s);
  napi_async_destroy(env, context);
  return out;
}

/* asyncInit(resource) -> status or failure */
static napi_value AsyncInit(napi_env env, napi_callback_info info) {
  ARGS(1);
  napi_value name;
  napi_create_string_utf8(env, "test", NAPI_AUTO_LENGTH, &name);
  napi_async_context context;
  napi_status s = napi_async_init(env, argv[0], name, &context);
  if (s != napi_ok) return failure(env, s);
  napi_async_destroy(env, context);
  return num(env, s);
}

/* scopes(fn): runs fn inside two nested callback scopes; then closes one
 * scope too many. Returns [result, status of the extra close]. */
static napi_value Scopes(napi_env env, napi_callback_info info) {
  ARGS(1);
  napi_value name, resource, global, result, out;
  napi_create_string_utf8(env, "test", NAPI_AUTO_LENGTH, &name);
  napi_create_object(env, &resource);
  napi_get_global(env, &global);
  napi_async_context context;
  napi_async_init(env, resource, name, &context);
  napi_callback_scope outer, inner;
  napi_open_callback_scope(env, resource, context, &outer);
  napi_open_callback_scope(env, resource, context, &inner);
  napi_call_function(env, global, argv[0], 0, NULL, &result);
  napi_close_callback_scope(env, inner);
  napi_close_callback_scope(env, outer);
  napi_status extra = napi_close_callback_scope(env, outer);
  napi_async_destroy(env, context);
  napi_create_array(env, &out);
  napi_set_element(env, out, 0, result);
  napi_set_element(env, out, 1, num(env, extra));
  return out;
}

/* leak(): an async context never destroyed; released at teardown. */
static napi_value Leak(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value name;
  napi_create_string_utf8(env, "leak", NAPI_AUTO_LENGTH, &name);
  napi_async_context context;
  napi_async_init(env, NULL, name, &context);
  return NULL;
}

static napi_value Filename(napi_env env, napi_callback_info info) {
  (void)info;
  const char *filename = NULL;
  napi_value r;
  node_api_get_module_file_name(env, &filename);
  napi_create_string_utf8(env, filename, NAPI_AUTO_LENGTH, &r);
  return r;
}

NAPI_MODULE_INIT() {
  napi_property_descriptor d[] = {
#define F(name, fn) {name, NULL, fn, NULL, NULL, NULL, napi_default, NULL}
      F("bufferInfo", BufferInfo),
      F("fromArrayBuffer", FromArrayBuffer),
      F("copy", Copy),
      F("create", Create),
      F("external", External),
      F("finalized", Finalized),
      F("makeCallback", MakeCallback),
      F("asyncInit", AsyncInit),
      F("scopes", Scopes),
      F("leak", Leak),
#undef F
      {"filename", NULL, NULL, Filename, NULL, NULL, napi_default, NULL},
  };
  napi_define_properties(env, exports, sizeof d / sizeof d[0], d);
  return exports;
}
