/*
 * ArrayBuffer, TypedArray, DataView, BigInt, Date and Promise.
 *
 * QuickJS exposes most of what is needed (JS_NewArrayBuffer,
 * JS_GetArrayBuffer, JS_NewTypedArray, JS_GetTypedArrayBuffer, JS_NewDate,
 * JS_NewBigInt64, JS_NewPromiseCapability, JS_PromiseState, JS_GetClassID),
 * but not:
 *   - its class ID enum: the IDs are found by probing one instance of each
 *     builtin at startup (host->cid_*);
 *   - DataView queries, nor Date's time value: the
 *     builtin accessors are captured at startup and called on the object;
 *   - BigInt limbs: napi_*_bigint_words go through a hexadecimal string,
 *     O(n) in the number of words, with two internal JS helpers.
 * Everything captured here comes from an internal script evaluated before
 * any user code, so later monkey-patching of the globals has no effect.
 */
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qjsnapi_internal.h"

/* napi_deferred: the resolving functions of a promise capability. */
struct napi_deferred__ {
  qn_list link; /* env->deferreds */
  JSValue resolve;
  JSValue reject;
};

/* Largest BigInt QuickJS can hold (JS_BIGINT_MAX_SIZE: 2^20 bits), in
 * 64-bit words. V8 allows 2^30 bits. */
#define QN_BIGINT_MAX_WORDS ((size_t)(1024 * 1024 / 64))

/* napi_typedarray_type -> JSTypedArrayEnum, element size and name. */
static const struct {
  JSTypedArrayEnum js;
  uint8_t size;
  const char *name;
} typed_arrays[] = {
    [napi_int8_array] = {JS_TYPED_ARRAY_INT8, 1, "Int8Array"},
    [napi_uint8_array] = {JS_TYPED_ARRAY_UINT8, 1, "Uint8Array"},
    [napi_uint8_clamped_array] = {JS_TYPED_ARRAY_UINT8C, 1,
                                  "Uint8ClampedArray"},
    [napi_int16_array] = {JS_TYPED_ARRAY_INT16, 2, "Int16Array"},
    [napi_uint16_array] = {JS_TYPED_ARRAY_UINT16, 2, "Uint16Array"},
    [napi_int32_array] = {JS_TYPED_ARRAY_INT32, 4, "Int32Array"},
    [napi_uint32_array] = {JS_TYPED_ARRAY_UINT32, 4, "Uint32Array"},
    [napi_float32_array] = {JS_TYPED_ARRAY_FLOAT32, 4, "Float32Array"},
    [napi_float64_array] = {JS_TYPED_ARRAY_FLOAT64, 8, "Float64Array"},
    [napi_bigint64_array] = {JS_TYPED_ARRAY_BIG_INT64, 8, "BigInt64Array"},
    [napi_biguint64_array] = {JS_TYPED_ARRAY_BIG_UINT64, 8, "BigUint64Array"},
    [napi_float16_array] = {JS_TYPED_ARRAY_FLOAT16, 2, "Float16Array"},
};
#define QN_TYPED_ARRAY_COUNT (sizeof(typed_arrays) / sizeof(typed_arrays[0]))

_Static_assert(QN_TYPED_ARRAY_COUNT == 12, "one entry per napi_typedarray_type");

/* ------------------------------------------------------------------------ */
/* Startup                                                                  */
/* ------------------------------------------------------------------------ */

static const char builtins_js[] =
    "(function () {\n"
    "  'use strict';\n"
    "  const get = (o, k) => Object.getOwnPropertyDescriptor(o, k).get;\n"
    "  const TA = Object.getPrototypeOf(Int8Array.prototype);\n"
    "  const B = BigInt, bigintToString = B.prototype.toString;\n"
    "  return [\n"
    "    ArrayBuffer, DataView,\n"
    "    get(DataView.prototype, 'buffer'),\n"
    "    get(DataView.prototype, 'byteLength'),\n"
    "    get(DataView.prototype, 'byteOffset'),\n"
    "    get(TA, 'buffer'),\n"
    "    get(TA, 'byteLength'),\n"
    "    Date.prototype.getTime,\n"
    "    (negative, hex) => { const v = B('0x' + hex); return negative ? -v "
    ": v; },\n"
    "    (v) => Reflect.apply(bigintToString, v, [16]),\n"
    "  ];\n"
    "})()";

static JSClassID class_of(JSContext *ctx, JSValue v) {
  JSClassID id = JS_IsException(v) ? 0 : JS_GetClassID(v);
  if (JS_IsException(v))
    JS_FreeValue(ctx, JS_GetException(ctx));
  else
    JS_FreeValue(ctx, v);
  return id;
}

int qn_builtins_init(qjsnapi_host *host) {
  JSContext *ctx = host->ctx;
  JSValue arr = JS_Eval(ctx, builtins_js, sizeof(builtins_js) - 1,
                        "quickjs-napi:builtins", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(arr)) return -1;
  JSValue *slots[] = {
      &host->arraybuffer_ctor, &host->dataview_ctor,
      &host->dv_buffer,        &host->dv_byte_length, &host->dv_byte_offset,
      &host->ta_buffer,        &host->ta_byte_length,  &host->date_get_time,  &host->bigint_from_hex,
      &host->bigint_to_hex,
  };
  for (uint32_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
    JSValue v = JS_GetPropertyUint32(ctx, arr, i);
    if (JS_IsException(v)) {
      JS_FreeValue(ctx, arr);
      return -1;
    }
    *slots[i] = v;
  }
  JS_FreeValue(ctx, arr);

  /* Class IDs, from one instance of each kind. */
  JSValue zero = JS_NewInt32(ctx, 0);
  JSValue ab = JS_CallConstructor(ctx, host->arraybuffer_ctor, 1, &zero);
  if (JS_IsException(ab)) return -1;
  host->cid_arraybuffer = JS_GetClassID(ab);
  host->cid_dataview =
      class_of(ctx, JS_CallConstructor(ctx, host->dataview_ctor, 1, &ab));
  JS_FreeValue(ctx, ab);
  host->cid_date = class_of(ctx, JS_NewDate(ctx, 0));
  for (int t = 0; t < 12; t++) {
    host->cid_typed[t] =
        class_of(ctx, JS_NewTypedArray(ctx, 1, &zero, (JSTypedArrayEnum)t));
  }
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue sab_ctor = JS_GetPropertyStr(ctx, global, "SharedArrayBuffer");
  JS_FreeValue(ctx, global);
  if (JS_IsFunction(ctx, sab_ctor)) {
    host->cid_shared_arraybuffer =
        class_of(ctx, JS_CallConstructor(ctx, sab_ctor, 1, &zero));
  }
  JS_FreeValue(ctx, sab_ctor);
  if (!host->cid_arraybuffer || !host->cid_dataview || !host->cid_date) {
    JS_ThrowInternalError(ctx, "quickjs-napi: builtin class probing failed");
    return -1;
  }
  return 0;
}

void qn_env_free_deferreds(napi_env env) {
  while (!qn_list_empty(&env->deferreds)) {
    napi_deferred d =
        QN_CONTAINER_OF(env->deferreds.next, struct napi_deferred__, link);
    qn_list_del(&d->link);
    JS_FreeValue(env->ctx, d->resolve);
    JS_FreeValue(env->ctx, d->reject);
    free(d);
  }
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static bool has_class(JSValueConst v, JSClassID id) {
  return id != 0 && JS_IsObject(v) && JS_GetClassID((JSValue)v) == id;
}

static bool is_arraybuffer(qjsnapi_host *host, JSValueConst v) {
  return has_class(v, host->cid_arraybuffer);
}

static bool is_any_arraybuffer(qjsnapi_host *host, JSValueConst v) {
  return has_class(v, host->cid_arraybuffer) ||
         has_class(v, host->cid_shared_arraybuffer);
}

/* napi_typedarray_type of a typed array, or -1. */
static int typed_array_type(qjsnapi_host *host, JSValueConst v) {
  if (!JS_IsObject(v)) return -1;
  JSClassID id = JS_GetClassID((JSValue)v);
  for (int t = 0; t < (int)QN_TYPED_ARRAY_COUNT; t++) {
    if (host->cid_typed[typed_arrays[t].js] == id) return t;
  }
  return -1;
}

/* Calls a captured accessor on `obj`. An exception (a detached buffer) is
 * cleared and reported as JS_UNDEFINED. */
static JSValue call_getter(JSContext *ctx, JSValueConst getter,
                           JSValueConst obj) {
  JSValue v = JS_Call(ctx, getter, obj, 0, NULL);
  if (JS_IsException(v)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
  }
  return v;
}

static size_t getter_size(JSContext *ctx, JSValueConst getter,
                          JSValueConst obj) {
  JSValue v = call_getter(ctx, getter, obj);
  int64_t n = 0;
  if (JS_IsNumber(v)) JS_ToInt64(ctx, &n, v);
  JS_FreeValue(ctx, v);
  return n > 0 ? (size_t)n : 0;
}

/* Data of an (Shared)ArrayBuffer: NULL when detached or empty, like V8's
 * Data(). */
static uint8_t *buffer_data(JSContext *ctx, JSValueConst ab, size_t *len) {
  size_t n = 0;
  uint8_t *p = JS_GetArrayBuffer(ctx, &n, ab);
  if (!p && JS_HasException(ctx)) JS_FreeValue(ctx, JS_GetException(ctx));
  if (!p) n = 0;
  if (len) *len = n;
  return n ? p : NULL;
}

/* JS_GetArrayBuffer throws only on a detached buffer. */
static bool is_detached(JSContext *ctx, JSValueConst ab) {
  size_t n;
  if (JS_GetArrayBuffer(ctx, &n, ab) || !JS_HasException(ctx)) return false;
  JS_FreeValue(ctx, JS_GetException(ctx));
  return true;
}

/* ------------------------------------------------------------------------ */
/* ArrayBuffer                                                              */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_arraybuffer(napi_env env,
                                                           size_t byte_length,
                                                           void **data,
                                                           napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  /* The builtin constructor: zero-filled, with QuickJS's own length checks
   * (RangeError beyond the maximum). */
  JSValue len = JS_NewFloat64(ctx, (double)byte_length);
  JSValue ab = JS_CallConstructor(ctx, env->host->arraybuffer_ctor, 1, &len);
  CHECK_JS(env, !JS_IsException(ab));
  if (data) *data = buffer_data(ctx, ab, NULL);
  *result = qn_new_handle(env, ab);
  CHECK_HANDLE(env, *result);
  return GET_RETURN_STATUS(env);
}

/* The data is used in place, never freed by QuickJS; the finalizer runs
 * (queued, from the event loop) once the ArrayBuffer is collected, as
 * Node.js does through an external Buffer. */
NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_arraybuffer(
    napi_env env, void *external_data, size_t byte_length,
    node_api_basic_finalize finalize_cb, void *finalize_hint,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue ab = JS_NewArrayBuffer(ctx, (uint8_t *)external_data, byte_length,
                                 NULL, NULL, false);
  CHECK_JS(env, !JS_IsException(ab));
  /* V8 has no backing store for NULL data: the buffer reads as detached
   * (test_typedarray NullArrayBuffer). */
  if (external_data == NULL) JS_DetachArrayBuffer(ctx, ab);
  if (finalize_cb &&
      qn_attach_finalizer(env, ab, finalize_cb, external_data,
                          finalize_hint) < 0) {
    JS_FreeValue(ctx, ab);
    CHECK_JS(env, false);
  }
  *result = qn_new_handle(env, ab);
  CHECK_HANDLE(env, *result);
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env,
                                                       napi_value value,
                                                       bool *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  *result = is_arraybuffer(env->host, qn_value(value));
  return qn_clear_last_error(env);
}

/* Node.js 22.23 also accepts a SharedArrayBuffer here. */
NAPI_EXTERN napi_status NAPI_CDECL napi_get_arraybuffer_info(
    napi_env env, napi_value arraybuffer, void **data, size_t *byte_length) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, arraybuffer);
  JSValue ab = qn_value(arraybuffer);
  RETURN_STATUS_IF_FALSE(env, is_any_arraybuffer(env->host, ab),
                         napi_invalid_arg);
  size_t len;
  uint8_t *p = buffer_data(env->ctx, ab, &len);
  if (data) *data = p;
  if (byte_length) *byte_length = len;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env,
                                                           napi_value arraybuffer) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, arraybuffer);
  JSValue ab = qn_value(arraybuffer);
  RETURN_STATUS_IF_FALSE(env, is_arraybuffer(env->host, ab),
                         napi_arraybuffer_expected);
  /* Every non-shared QuickJS ArrayBuffer is detachable. */
  JS_DetachArrayBuffer(env->ctx, ab);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_detached_arraybuffer(
    napi_env env, napi_value arraybuffer, bool *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, arraybuffer);
  CHECK_ARG(env, result);
  JSValue ab = qn_value(arraybuffer);
  *result = is_arraybuffer(env->host, ab) &&
            is_detached(env->ctx, ab);
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* TypedArray                                                               */
/* ------------------------------------------------------------------------ */

/* Node: THROW_RANGE_ERROR_IF_FALSE (the exception stays pending, the
 * status is napi_generic_failure). */
static napi_status throw_range(napi_env env, const char *code,
                               const char *msg) {
  napi_throw_range_error(env, code, msg);
  return qn_set_last_error(env, napi_generic_failure);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_typedarray(napi_env env,
                                                      napi_value value,
                                                      bool *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  *result = typed_array_type(env->host, qn_value(value)) >= 0;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_typedarray(
    napi_env env, napi_typedarray_type type, size_t length,
    napi_value arraybuffer, size_t byte_offset, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, arraybuffer);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue ab = qn_value(arraybuffer);
  RETURN_STATUS_IF_FALSE(env, is_any_arraybuffer(env->host, ab),
                         napi_invalid_arg);
  RETURN_STATUS_IF_FALSE(env, (unsigned)type < QN_TYPED_ARRAY_COUNT,
                         napi_invalid_arg);
  size_t size = typed_arrays[type].size;
  if (size > 1 && byte_offset % size != 0) {
    char msg[96];
    snprintf(msg, sizeof(msg), "start offset of %s should be a multiple of %u",
             typed_arrays[type].name, (unsigned)size);
    return throw_range(env, "ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT", msg);
  }
  size_t buffer_length;
  buffer_data(ctx, ab, &buffer_length);
  if (length > SIZE_MAX / size || length * size > SIZE_MAX - byte_offset ||
      length * size + byte_offset > buffer_length) {
    return throw_range(env, "ERR_NAPI_INVALID_TYPEDARRAY_LENGTH",
                       "Invalid typed array length");
  }
  JSValueConst args[3] = {ab, JS_NewFloat64(ctx, (double)byte_offset),
                          JS_NewFloat64(ctx, (double)length)};
  JSValue ta = JS_NewTypedArray(ctx, 3, args, typed_arrays[type].js);
  CHECK_JS(env, !JS_IsException(ta));
  *result = qn_new_handle(env, ta);
  CHECK_HANDLE(env, *result);
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_typedarray_info(
    napi_env env, napi_value typedarray, napi_typedarray_type *type,
    size_t *length, void **data, napi_value *arraybuffer,
    size_t *byte_offset) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, typedarray);
  JSContext *ctx = env->ctx;
  JSValue ta = qn_value(typedarray);
  int t = typed_array_type(env->host, ta);
  RETURN_STATUS_IF_FALSE(env, t >= 0, napi_invalid_arg);
  if (type) *type = (napi_typedarray_type)t;

  /* The byteLength accessor is right for length-tracking arrays (QuickJS
   * does not keep their stored length up to date) and is 0 when the
   * array is out of bounds or detached, like V8's Length(). */
  size_t elem = typed_arrays[t].size;
  size_t byte_len = getter_size(ctx, env->host->ta_byte_length, ta);
  size_t offset = 0;
  JSValue ab = JS_GetTypedArrayBuffer(ctx, ta, &offset, NULL, NULL);
  if (JS_IsException(ab)) {
    /* Out of bounds or detached: the stored offset is not reachable (V8
     * still reports it for an out-of-bounds, non-detached array). */
    JS_FreeValue(ctx, JS_GetException(ctx));
    offset = 0;
    ab = call_getter(ctx, env->host->ta_buffer, ta);
  }
  if (length) *length = byte_len / elem;
  if (byte_offset) *byte_offset = offset;
  if (data) {
    uint8_t *p = buffer_data(ctx, ab, NULL);
    *data = p ? p + offset : NULL;
  }
  if (arraybuffer) {
    *arraybuffer = qn_new_handle(env, ab);
    CHECK_HANDLE(env, *arraybuffer);
  } else {
    JS_FreeValue(ctx, ab);
  }
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* DataView                                                                 */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_dataview(napi_env env,
                                                        size_t byte_length,
                                                        napi_value arraybuffer,
                                                        size_t byte_offset,
                                                        napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, arraybuffer);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue ab = qn_value(arraybuffer);
  RETURN_STATUS_IF_FALSE(env, is_arraybuffer(env->host, ab), napi_invalid_arg);
  size_t buffer_length;
  buffer_data(ctx, ab, &buffer_length);
  if (byte_length > SIZE_MAX - byte_offset ||
      byte_length + byte_offset > buffer_length) {
    napi_throw_range_error(env, "ERR_NAPI_INVALID_DATAVIEW_ARGS",
                           "byte_offset + byte_length should be less than or "
                           "equal to the size in bytes of the array passed in");
    return qn_set_last_error(env, napi_pending_exception);
  }
  JSValueConst args[3] = {ab, JS_NewFloat64(ctx, (double)byte_offset),
                          JS_NewFloat64(ctx, (double)byte_length)};
  JSValue dv = JS_CallConstructor(ctx, env->host->dataview_ctor, 3, args);
  CHECK_JS(env, !JS_IsException(dv));
  *result = qn_new_handle(env, dv);
  CHECK_HANDLE(env, *result);
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_dataview(napi_env env,
                                                    napi_value value,
                                                    bool *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  *result = has_class(qn_value(value), env->host->cid_dataview);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_dataview_info(
    napi_env env, napi_value dataview, size_t *byte_length, void **data,
    napi_value *arraybuffer, size_t *byte_offset) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, dataview);
  JSContext *ctx = env->ctx;
  qjsnapi_host *host = env->host;
  JSValue dv = qn_value(dataview);
  RETURN_STATUS_IF_FALSE(env, has_class(dv, host->cid_dataview),
                         napi_invalid_arg);
  /* The accessors throw on a detached buffer: V8 reports 0 then. */
  size_t offset = getter_size(ctx, host->dv_byte_offset, dv);
  if (byte_length) *byte_length = getter_size(ctx, host->dv_byte_length, dv);
  if (byte_offset) *byte_offset = offset;
  if (data || arraybuffer) {
    JSValue ab = call_getter(ctx, host->dv_buffer, dv);
    if (data) {
      uint8_t *p = buffer_data(ctx, ab, NULL);
      *data = p ? p + offset : NULL;
    }
    if (arraybuffer) {
      *arraybuffer = qn_new_handle(env, ab);
      CHECK_HANDLE(env, *arraybuffer);
    } else {
      JS_FreeValue(ctx, ab);
    }
  }
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* BigInt                                                                   */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_int64(napi_env env,
                                                            int64_t value,
                                                            napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  *result = qn_new_handle(env, JS_NewBigInt64(env->ctx, value));
  CHECK_HANDLE(env, *result);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_uint64(napi_env env,
                                                             uint64_t value,
                                                             napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  *result = qn_new_handle(env, JS_NewBigUint64(env->ctx, value));
  CHECK_HANDLE(env, *result);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_words(
    napi_env env, int sign_bit, size_t word_count, const uint64_t *words,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, words);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  RETURN_STATUS_IF_FALSE(env, word_count <= INT_MAX, napi_invalid_arg);
  /* Checked before reading `words`, like V8 (the engine error is
   * forwarded as a pending exception). */
  if (word_count > QN_BIGINT_MAX_WORDS) {
    JS_ThrowRangeError(ctx, "Maximum BigInt size exceeded");
    return qn_set_last_error(env, napi_pending_exception);
  }
  size_t n = word_count;
  while (n > 0 && words[n - 1] == 0) n--;
  JSValue v;
  if (n == 0) {
    v = JS_NewBigInt64(ctx, 0); /* also -0n */
  } else {
    char *hex = (char *)js_malloc(ctx, n * 16 + 1);
    CHECK_JS(env, hex != NULL);
    for (size_t i = 0; i < n; i++) {
      snprintf(hex + i * 16, 17, "%016" PRIx64, words[n - 1 - i]);
    }
    JSValue args[2] = {JS_NewBool(ctx, sign_bit != 0),
                       JS_NewStringLen(ctx, hex, n * 16)};
    js_free(ctx, hex);
    if (JS_IsException(args[1])) CHECK_JS(env, false);
    v = JS_Call(ctx, env->host->bigint_from_hex, JS_UNDEFINED, 2,
                (JSValueConst *)args);
    JS_FreeValue(ctx, args[1]);
  }
  RETURN_STATUS_IF_FALSE(env, !JS_IsException(v), napi_generic_failure);
  *result = qn_new_handle(env, v);
  CHECK_HANDLE(env, *result);
  return GET_RETURN_STATUS(env);
}

/* Value modulo 2^64 (two's complement) of a BigInt; `lossless` is set by
 * comparing the value rebuilt from the 64 bits with the original. */
static napi_status bigint64(napi_env env, napi_value value, uint64_t *result,
                            bool *lossless, bool is_signed) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  CHECK_ARG(env, lossless);
  JSContext *ctx = env->ctx;
  JSValue v = qn_value(value);
  RETURN_STATUS_IF_FALSE(env, JS_IsBigInt(ctx, v), napi_bigint_expected);
  int64_t bits = 0;
  if (JS_ToBigInt64(ctx, &bits, v) < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return qn_set_last_error(env, napi_generic_failure);
  }
  JSValue back = is_signed ? JS_NewBigInt64(ctx, bits)
                           : JS_NewBigUint64(ctx, (uint64_t)bits);
  *lossless = JS_StrictEq(ctx, back, v);
  JS_FreeValue(ctx, back);
  *result = (uint64_t)bits;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_int64(
    napi_env env, napi_value value, int64_t *result, bool *lossless) {
  return bigint64(env, value, (uint64_t *)result, lossless, true);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_uint64(
    napi_env env, napi_value value, uint64_t *result, bool *lossless) {
  return bigint64(env, value, result, lossless, false);
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

/* V8 BigInt::ToWordsArray: *word_count is the capacity on input and the
 * number of words of the value on output; only the low words that fit are
 * written. 0n has no words and sign 0. */
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_words(
    napi_env env, napi_value value, int *sign_bit, size_t *word_count,
    uint64_t *words) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, word_count);
  JSContext *ctx = env->ctx;
  JSValue v = qn_value(value);
  RETURN_STATUS_IF_FALSE(env, JS_IsBigInt(ctx, v), napi_bigint_expected);
  bool count_only = sign_bit == NULL && words == NULL;
  if (!count_only) {
    CHECK_ARG(env, sign_bit);
    CHECK_ARG(env, words);
  }
  JSValue hex_v = JS_Call(ctx, env->host->bigint_to_hex, JS_UNDEFINED, 1, &v);
  RETURN_STATUS_IF_FALSE(env, !JS_IsException(hex_v), napi_generic_failure);
  size_t len;
  const char *hex = JS_ToCStringLen(ctx, &len, hex_v);
  JS_FreeValue(ctx, hex_v);
  RETURN_STATUS_IF_FALSE(env, hex != NULL, napi_generic_failure);

  bool negative = len > 0 && hex[0] == '-';
  const char *digits = hex + (negative ? 1 : 0);
  size_t ndigits = len - (negative ? 1 : 0);
  size_t needed = (ndigits == 1 && digits[0] == '0') ? 0 : (ndigits + 15) / 16;
  if (!count_only) {
    size_t avail = *word_count < needed ? *word_count : needed;
    for (size_t w = 0; w < avail; w++) {
      /* word w covers digits [ndigits - 16(w+1), ndigits - 16w) */
      size_t end = ndigits - 16 * w;
      size_t start = end > 16 ? end - 16 : 0;
      uint64_t x = 0;
      for (size_t i = start; i < end; i++)
        x = (x << 4) | (uint64_t)hex_digit(digits[i]);
      words[w] = x;
    }
    *sign_bit = needed > 0 && negative;
  }
  *word_count = needed;
  JS_FreeCString(ctx, hex);
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* Date                                                                     */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_date(napi_env env, double time,
                                                    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  JSValue d = JS_NewDate(env->ctx, time); /* applies TimeClip, like V8 */
  CHECK_JS(env, !JS_IsException(d));
  *result = qn_new_handle(env, d);
  CHECK_HANDLE(env, *result);
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value,
                                                bool *is_date) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, is_date);
  *is_date = has_class(qn_value(value), env->host->cid_date);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_date_value(napi_env env,
                                                       napi_value value,
                                                       double *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue d = qn_value(value);
  RETURN_STATUS_IF_FALSE(env, has_class(d, env->host->cid_date),
                         napi_date_expected);
  JSValue t = JS_Call(ctx, env->host->date_get_time, d, 0, NULL);
  CHECK_JS(env, !JS_IsException(t));
  JS_ToFloat64(ctx, result, t);
  JS_FreeValue(ctx, t);
  return GET_RETURN_STATUS(env);
}

/* ------------------------------------------------------------------------ */
/* Promise                                                                  */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_promise(napi_env env,
                                                       napi_deferred *deferred,
                                                       napi_value *promise) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, deferred);
  CHECK_ARG(env, promise);
  JSContext *ctx = env->ctx;
  napi_deferred d = (napi_deferred)calloc(1, sizeof(struct napi_deferred__));
  RETURN_STATUS_IF_FALSE(env, d != NULL, napi_generic_failure);
  JSValue funcs[2];
  JSValue p = JS_NewPromiseCapability(ctx, funcs);
  if (JS_IsException(p)) {
    free(d);
    RETURN_STATUS_IF_FALSE(env, false, napi_generic_failure);
  }
  d->resolve = funcs[0];
  d->reject = funcs[1];
  qn_list_add_tail(&d->link, &env->deferreds);
  *promise = qn_new_handle(env, p);
  if (!*promise) {
    qn_list_del(&d->link);
    JS_FreeValue(ctx, d->resolve);
    JS_FreeValue(ctx, d->reject);
    free(d);
    CHECK_HANDLE(env, *promise);
  }
  *deferred = d;
  return GET_RETURN_STATUS(env);
}

/* Node: v8impl::ConcludeDeferred. The deferred is released in any case. */
static napi_status conclude(napi_env env, napi_deferred deferred,
                            napi_value result, bool resolve) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue arg = qn_value(result);
  JSValue r = JS_Call(ctx, resolve ? deferred->resolve : deferred->reject,
                      JS_UNDEFINED, 1, &arg);
  qn_list_del(&deferred->link);
  JS_FreeValue(ctx, deferred->resolve);
  JS_FreeValue(ctx, deferred->reject);
  free(deferred);
  RETURN_STATUS_IF_FALSE(env, !JS_IsException(r), napi_generic_failure);
  JS_FreeValue(ctx, r);
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_resolve_deferred(napi_env env,
                                                         napi_deferred deferred,
                                                         napi_value resolution) {
  CHECK_ENV(env);
  CHECK_ARG(env, deferred);
  return conclude(env, deferred, resolution, true);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reject_deferred(napi_env env,
                                                        napi_deferred deferred,
                                                        napi_value rejection) {
  CHECK_ENV(env);
  CHECK_ARG(env, deferred);
  return conclude(env, deferred, rejection, false);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_promise(napi_env env,
                                                   napi_value value,
                                                   bool *is_promise) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, is_promise);
  *is_promise = (int)JS_PromiseState(env->ctx, qn_value(value)) >= 0;
  return qn_clear_last_error(env);
}
