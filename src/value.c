/*
 * Values: singletons, numbers, booleans, typeof, coercion, arrays and
 * strict equality. Semantics follow src/js_native_api_v8.cc of Node.js:
 * getters never coerce (napi_*_expected on a type mismatch), coercions may
 * run JS and leave the exception pending.
 */
#include <math.h>
#include <string.h>

#include "qjsnapi_internal.h"

static napi_status return_handle(napi_env env, JSValue v, napi_value *result) {
  CHECK_JS(env, !JS_IsException(v));
  napi_value h = qn_new_handle(env, v);
  CHECK_HANDLE(env, h);
  *result = h;
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* Singletons and primitives                                                */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_get_undefined(napi_env env,
                                                      napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_UNDEFINED, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_null(napi_env env,
                                                 napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_NULL, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_global(napi_env env,
                                                   napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_GetGlobalObject(env->ctx), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value,
                                                    napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_NewBool(env->ctx, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_object(napi_env env,
                                                      napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_NewObject(env->ctx), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_double(napi_env env,
                                                      double value,
                                                      napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_NewFloat64(env->ctx, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_int32(napi_env env,
                                                     int32_t value,
                                                     napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_NewInt32(env->ctx, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_uint32(napi_env env,
                                                      uint32_t value,
                                                      napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_NewUint32(env->ctx, value), result);
}

/* A Number, not a BigInt: values beyond 2^53 lose precision, as in V8. */
NAPI_EXTERN napi_status NAPI_CDECL napi_create_int64(napi_env env,
                                                     int64_t value,
                                                     napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_NewInt64(env->ctx, value), result);
}

/* ------------------------------------------------------------------------ */
/* Getters                                                                  */
/* ------------------------------------------------------------------------ */

/* Numeric value of a Number (int or float64 tag), no coercion. */
static double number_value(JSValueConst v) {
  if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) return JS_VALUE_GET_INT(v);
  return JS_VALUE_GET_FLOAT64(v);
}

/* ECMAScript ToInt32 on a double, without engine calls. */
static int32_t double_to_int32(double d) {
  if (!isfinite(d)) return 0;
  d = trunc(d);
  double m = fmod(d, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return (int32_t)(uint32_t)m;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_double(napi_env env,
                                                         napi_value value,
                                                         double *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue v = qn_value(value);
  RETURN_STATUS_IF_FALSE(env, JS_IsNumber(v), napi_number_expected);
  *result = number_value(v);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int32(napi_env env,
                                                        napi_value value,
                                                        int32_t *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue v = qn_value(value);
  RETURN_STATUS_IF_FALSE(env, JS_IsNumber(v), napi_number_expected);
  *result = JS_VALUE_GET_TAG(v) == JS_TAG_INT ? JS_VALUE_GET_INT(v)
                                              : double_to_int32(number_value(v));
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_uint32(napi_env env,
                                                         napi_value value,
                                                         uint32_t *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue v = qn_value(value);
  RETURN_STATUS_IF_FALSE(env, JS_IsNumber(v), napi_number_expected);
  /* ToUint32 has the same bit pattern as ToInt32. */
  *result = JS_VALUE_GET_TAG(v) == JS_TAG_INT
                ? (uint32_t)JS_VALUE_GET_INT(v)
                : (uint32_t)double_to_int32(number_value(v));
  return qn_clear_last_error(env);
}

/* V8 Value::IntegerValue: truncation, saturating outside the int64 range;
 * Node.js maps non-finite values to 0. */
NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int64(napi_env env,
                                                        napi_value value,
                                                        int64_t *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue v = qn_value(value);
  if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
    *result = JS_VALUE_GET_INT(v);
    return qn_clear_last_error(env);
  }
  RETURN_STATUS_IF_FALSE(env, JS_IsNumber(v), napi_number_expected);
  double d = number_value(v);
  if (!isfinite(d)) {
    *result = 0;
  } else if (d >= 9223372036854775808.0) {
    *result = INT64_MAX;
  } else if (d <= -9223372036854775808.0) {
    *result = INT64_MIN;
  } else {
    *result = (int64_t)d;
  }
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bool(napi_env env,
                                                       napi_value value,
                                                       bool *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue v = qn_value(value);
  RETURN_STATUS_IF_FALSE(env, JS_IsBool(v), napi_boolean_expected);
  *result = JS_VALUE_GET_BOOL(v) != 0;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_typeof(napi_env env, napi_value value,
                                               napi_valuetype *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue v = qn_value(value);
  if (JS_IsNumber(v)) {
    *result = napi_number;
  } else if (JS_IsBigInt(ctx, v)) {
    *result = napi_bigint;
  } else if (JS_IsString(v)) {
    *result = napi_string;
  } else if (JS_IsFunction(ctx, v)) {
    *result = napi_function;
  } else if (JS_IsObject(v)) {
    /* napi_external is reported once externals exist (M2). */
    *result = napi_object;
  } else if (JS_IsBool(v)) {
    *result = napi_boolean;
  } else if (JS_IsUndefined(v)) {
    *result = napi_undefined;
  } else if (JS_IsSymbol(v)) {
    *result = napi_symbol;
  } else if (JS_IsNull(v)) {
    *result = napi_null;
  } else {
    return qn_set_last_error(env, napi_invalid_arg);
  }
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* Coercion (may run JS)                                                    */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env,
                                                       napi_value value,
                                                       napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  int b = JS_ToBool(env->ctx, qn_value(value));
  CHECK_JS(env, b >= 0);
  return return_handle(env, JS_NewBool(env->ctx, b), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_number(napi_env env,
                                                         napi_value value,
                                                         napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue v = qn_value(value);
  if (JS_IsNumber(v)) return return_handle(env, v, result);
  double d;
  /* ToNumber throws (Symbol, BigInt, throwing valueOf): the exception stays
   * pending and the status is napi_number_expected, as CHECK_TO_NUMBER. */
  RETURN_STATUS_IF_FALSE(env, JS_ToFloat64(env->ctx, &d, v) == 0,
                         napi_number_expected);
  return return_handle(env, JS_NewFloat64(env->ctx, d), result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_object(napi_env env,
                                                         napi_value value,
                                                         napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue obj = qn_to_object(env, qn_value(value));
  RETURN_STATUS_IF_FALSE(env, !JS_IsException(obj), napi_object_expected);
  return return_handle(env, obj, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_string(napi_env env,
                                                         napi_value value,
                                                         napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue s = JS_ToString(env->ctx, qn_value(value));
  RETURN_STATUS_IF_FALSE(env, !JS_IsException(s), napi_string_expected);
  return return_handle(env, s, result);
}

/* ------------------------------------------------------------------------ */
/* Arrays and comparison                                                    */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_array(napi_env env,
                                                     napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return return_handle(env, JS_NewArray(env->ctx), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_array_with_length(napi_env env, size_t length, napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue arr = JS_NewArray(ctx);
  CHECK_JS(env, !JS_IsException(arr));
  /* V8 Array::New(int): lengths beyond int range give an empty array. */
  if (length > 0 && length <= INT32_MAX) {
    if (JS_SetPropertyStr(ctx, arr, "length",
                          JS_NewUint32(ctx, (uint32_t)length)) < 0) {
      JS_FreeValue(ctx, arr);
      return qn_set_last_error(env, napi_pending_exception);
    }
  }
  return return_handle(env, arr, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_array(napi_env env,
                                                 napi_value value,
                                                 bool *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  int r = JS_IsArray(env->ctx, qn_value(value));
  if (r < 0) {
    /* Only a revoked Proxy can throw here; V8's IsArray never throws and
     * reports false for any Proxy. */
    JS_FreeValue(env->ctx, JS_GetException(env->ctx));
    r = 0;
  }
  *result = r != 0;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_array_length(napi_env env,
                                                         napi_value value,
                                                         uint32_t *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue v = qn_value(value);
  int is_array = JS_IsArray(ctx, v);
  CHECK_JS_STATUS(env, is_array >= 0, napi_array_expected);
  RETURN_STATUS_IF_FALSE(env, is_array, napi_array_expected);
  JSValue len = JS_GetPropertyStr(ctx, v, "length");
  CHECK_JS(env, !JS_IsException(len));
  uint32_t n = 0;
  int r = JS_ToInt32(ctx, (int32_t *)&n, len);
  JS_FreeValue(ctx, len);
  CHECK_JS(env, r == 0);
  *result = n;
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_strict_equals(napi_env env,
                                                      napi_value lhs,
                                                      napi_value rhs,
                                                      bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, lhs);
  CHECK_ARG(env, rhs);
  CHECK_ARG(env, result);
  *result = JS_StrictEq(env->ctx, qn_value(lhs), qn_value(rhs)) != 0;
  return GET_RETURN_STATUS(env);
}
