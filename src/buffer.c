/*
 * Node.js Buffer: napi_create_buffer, napi_create_buffer_copy,
 * napi_create_external_buffer, node_api_create_buffer_from_arraybuffer,
 * napi_is_buffer, napi_get_buffer_info.
 *
 * Buffers are FastBuffer instances (a Uint8Array subclass whose prototype
 * is Buffer.prototype), as in Node.js. The class is the minimal Buffer of
 * src/buffer.js, embedded in the library and evaluated when the host is
 * created; qjsnapi_host_get_buffer() gives it to the embedder, who decides
 * where it is visible (the quickjs-napi executable: globalThis.Buffer and
 * require('buffer')).
 *
 * Like Node.js 22 (node::Buffer::HasInstance), napi_is_buffer and
 * napi_get_buffer_info accept any ArrayBufferView.
 */
#include <stdlib.h>
#include <string.h>

#include "buffer_js.h" /* generated: qn_buffer_js, qn_buffer_js_len */
#include "qjsnapi_internal.h"

int qn_buffer_init(qjsnapi_host *host) {
  JSContext *ctx = host->ctx;
  JSValue fn = JS_Eval(ctx, (const char *)qn_buffer_js, qn_buffer_js_len,
                       "quickjs-napi:buffer", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fn)) return -1;
  JSValue arr = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
  JS_FreeValue(ctx, fn);
  if (JS_IsException(arr)) return -1;
  host->buffer_ctor = JS_GetPropertyUint32(ctx, arr, 0);
  host->buffer_make = JS_GetPropertyUint32(ctx, arr, 1);
  JS_FreeValue(ctx, arr);
  if (!JS_IsFunction(ctx, host->buffer_ctor) ||
      !JS_IsFunction(ctx, host->buffer_make)) {
    if (!JS_HasException(ctx))
      JS_ThrowInternalError(ctx, "quickjs-napi: Buffer initialization failed");
    return -1;
  }
  return 0;
}

JSValue qjsnapi_host_get_buffer(qjsnapi_host *host) {
  return JS_DupValue(host->ctx, host->buffer_ctor);
}

/* new FastBuffer(ab, byte_offset, length); consumes `ab`. */
static JSValue make_buffer(napi_env env, JSValue ab, size_t byte_offset,
                           size_t length) {
  JSContext *ctx = env->ctx;
  JSValue args[3] = {ab, JS_NewFloat64(ctx, (double)byte_offset),
                     JS_NewFloat64(ctx, (double)length)};
  JSValue buf = JS_Call(ctx, env->host->buffer_make, JS_UNDEFINED, 3,
                        (JSValueConst *)args);
  JS_FreeValue(ctx, ab);
  return buf;
}

/* A zero-filled buffer of `length` bytes (Node: node::Buffer::New). */
static napi_status new_buffer(napi_env env, size_t length, void **data,
                              napi_value *result) {
  JSContext *ctx = env->ctx;
  JSValue len = JS_NewFloat64(ctx, (double)length);
  JSValue ab = JS_CallConstructor(ctx, env->host->arraybuffer_ctor, 1, &len);
  CHECK_JS(env, !JS_IsException(ab));
  size_t n = 0;
  uint8_t *p = JS_GetArrayBuffer(ctx, &n, ab);
  JSValue buf = make_buffer(env, ab, 0, length);
  CHECK_JS(env, !JS_IsException(buf));
  if (data) *data = length ? p : NULL;
  *result = qn_new_handle(env, buf);
  CHECK_HANDLE(env, *result);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer(napi_env env,
                                                      size_t length,
                                                      void **data,
                                                      napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  napi_status s = new_buffer(env, length, data, result);
  if (s != napi_ok) return s;
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer_copy(
    napi_env env, size_t length, const void *data, void **result_data,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  void *p = NULL;
  napi_status s = new_buffer(env, length, &p, result);
  if (s != napi_ok) return s;
  if (length && data) memcpy(p, data, length);
  if (result_data) *result_data = p;
  return GET_RETURN_STATUS(env);
}

/* The memory is used in place; the finalizer is queued once the backing
 * ArrayBuffer is collected, as Node.js defers it (BufferFinalizer). */
NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_buffer(
    napi_env env, size_t length, void *data,
    node_api_basic_finalize finalize_cb, void *finalize_hint,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue ab =
      JS_NewArrayBuffer(ctx, (uint8_t *)data, length, NULL, NULL, false);
  CHECK_JS(env, !JS_IsException(ab));
  if (finalize_cb &&
      qn_attach_finalizer(env, ab, (napi_finalize)finalize_cb, data,
                          finalize_hint) < 0) {
    JS_FreeValue(ctx, ab);
    CHECK_JS(env, false);
  }
  JSValue buf = make_buffer(env, ab, 0, length);
  CHECK_JS(env, !JS_IsException(buf));
  *result = qn_new_handle(env, buf);
  CHECK_HANDLE(env, *result);
  return GET_RETURN_STATUS(env);
}

/* Node.js returns napi_ok with the RangeError pending when the range does
 * not fit; here the status is napi_pending_exception (*result is not set in
 * either case). */
NAPI_EXTERN napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
    napi_env env, napi_value arraybuffer, size_t byte_offset,
    size_t byte_length, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, arraybuffer);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue ab = qn_value(arraybuffer);
  RETURN_STATUS_IF_FALSE(
      env,
      JS_IsObject(ab) && JS_GetClassID(ab) == env->host->cid_arraybuffer,
      napi_invalid_arg);
  size_t ab_length = 0;
  if (!JS_GetArrayBuffer(ctx, &ab_length, ab) && JS_HasException(ctx)) {
    JS_FreeValue(ctx, JS_GetException(ctx)); /* detached: length 0 */
    ab_length = 0;
  }
  if (byte_offset > ab_length || byte_length > ab_length - byte_offset) {
    napi_throw_range_error(env, "ERR_OUT_OF_RANGE",
                           "The byte offset + length is out of range");
    return qn_set_last_error(env, napi_pending_exception);
  }
  JSValue buf = make_buffer(env, JS_DupValue(ctx, ab), byte_offset, byte_length);
  CHECK_JS(env, !JS_IsException(buf));
  *result = qn_new_handle(env, buf);
  CHECK_HANDLE(env, *result);
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_buffer(napi_env env,
                                                  napi_value value,
                                                  bool *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  *result = qn_view_data(env, qn_value(value), NULL, NULL);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_buffer_info(napi_env env,
                                                        napi_value value,
                                                        void **data,
                                                        size_t *length) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  void *p = NULL;
  size_t n = 0;
  RETURN_STATUS_IF_FALSE(env, qn_view_data(env, qn_value(value), &p, &n),
                         napi_invalid_arg);
  if (data) *data = p;
  if (length) *length = n;
  return qn_clear_last_error(env);
}
