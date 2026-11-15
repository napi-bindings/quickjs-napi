/*
 * Values: singletons, typeof, objects, strings and named properties.
 *
 * M0 covers the subset needed to load and exercise a minimal addon; the rest
 * of sections 3.1/3.2 of the plan lands in M1.
 */
#include <string.h>

#include "qjsnapi_internal.h"

static napi_status return_handle(napi_env env, JSValue v, napi_value *result) {
  CHECK_JS(env, !JS_IsException(v));
  napi_value h = qn_new_handle(env, v);
  CHECK_HANDLE(env, h);
  *result = h;
  return qn_clear_last_error(env);
}

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

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf8(napi_env env,
                                                           const char *str,
                                                           size_t length,
                                                           napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  /* Same rules as Node.js: str may be NULL only for an empty string. */
  RETURN_STATUS_IF_FALSE(env, str != NULL || length == 0, napi_invalid_arg);
  RETURN_STATUS_IF_FALSE(env, length == NAPI_AUTO_LENGTH || length <= INT32_MAX,
                         napi_invalid_arg);
  if (length == NAPI_AUTO_LENGTH) length = strlen(str);
  return return_handle(env, JS_NewStringLen(env->ctx, str ? str : "", length),
                       result);
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

NAPI_EXTERN napi_status NAPI_CDECL napi_set_named_property(napi_env env,
                                                           napi_value object,
                                                           const char *utf8name,
                                                           napi_value value) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, object);
  CHECK_ARG(env, utf8name);
  CHECK_ARG(env, value);
  JSValue obj = qn_value(object);
  RETURN_STATUS_IF_FALSE(env, JS_IsObject(obj), napi_object_expected);
  int r = JS_SetPropertyStr(env->ctx, obj, utf8name,
                            JS_DupValue(env->ctx, qn_value(value)));
  CHECK_JS(env, r >= 0);
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_named_property(napi_env env,
                                                           napi_value object,
                                                           const char *utf8name,
                                                           napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, object);
  CHECK_ARG(env, utf8name);
  CHECK_ARG(env, result);
  JSValue obj = qn_value(object);
  RETURN_STATUS_IF_FALSE(env, JS_IsObject(obj), napi_object_expected);
  JSValue v = JS_GetPropertyStr(env->ctx, obj, utf8name);
  return return_handle(env, v, result);
}
