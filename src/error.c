/*
 * Errors and exceptions.
 *
 * QuickJS keeps the pending exception on the runtime (JS_HasException /
 * JS_GetException). Node-API mirrors it: APIs that may run JS refuse to start
 * while an exception is pending (NAPI_PREAMBLE), and the function trampoline
 * returns JS_EXCEPTION whenever native code leaves one pending.
 */
#include <string.h>

#include "qjsnapi_internal.h"

/* new <Ctor>(message) with an optional `code` property, as Node.js does. */
static JSValue qn_make_error(napi_env env, enum qn_error_kind kind,
                             JSValueConst code, JSValueConst msg) {
  JSContext *ctx = env->ctx;
  JSValueConst args[1] = {msg};
  JSValue err = JS_CallConstructor(ctx, env->host->error_ctors[kind], 1, args);
  if (JS_IsException(err)) return err;
  if (!JS_IsUndefined(code)) {
    if (JS_SetPropertyStr(ctx, err, "code", JS_DupValue(ctx, code)) < 0) {
      JS_FreeValue(ctx, err);
      return JS_EXCEPTION;
    }
  }
  return err;
}

static napi_status create_error(napi_env env, enum qn_error_kind kind,
                                napi_value code, napi_value msg,
                                napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, msg);
  CHECK_ARG(env, result);
  JSValue msg_v = qn_value(msg);
  RETURN_STATUS_IF_FALSE(env, JS_IsString(msg_v), napi_string_expected);
  JSValue code_v = JS_UNDEFINED;
  if (code != NULL) {
    code_v = qn_value(code);
    RETURN_STATUS_IF_FALSE(env, JS_IsString(code_v), napi_string_expected);
  }
  JSValue err = qn_make_error(env, kind, code_v, msg_v);
  CHECK_JS(env, !JS_IsException(err));
  napi_value h = qn_new_handle(env, err);
  CHECK_HANDLE(env, h);
  *result = h;
  return qn_clear_last_error(env);
}

static napi_status throw_error(napi_env env, enum qn_error_kind kind,
                               const char *code, const char *msg) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, msg);
  JSContext *ctx = env->ctx;
  JSValue msg_v = JS_NewString(ctx, msg);
  CHECK_JS(env, !JS_IsException(msg_v));
  JSValue code_v = JS_UNDEFINED;
  if (code != NULL) {
    code_v = JS_NewString(ctx, code);
    if (JS_IsException(code_v)) {
      JS_FreeValue(ctx, msg_v);
      return qn_set_last_error(env, napi_pending_exception);
    }
  }
  JSValue err = qn_make_error(env, kind, code_v, msg_v);
  JS_FreeValue(ctx, msg_v);
  JS_FreeValue(ctx, code_v);
  CHECK_JS(env, !JS_IsException(err));
  JS_Throw(ctx, err); /* takes ownership */
  /* Like Node.js: the call succeeded, the exception is now pending. */
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_error(napi_env env,
                                                     napi_value code,
                                                     napi_value msg,
                                                     napi_value *result) {
  return create_error(env, QN_ERROR, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_type_error(napi_env env,
                                                          napi_value code,
                                                          napi_value msg,
                                                          napi_value *result) {
  return create_error(env, QN_TYPE_ERROR, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_range_error(napi_env env,
                                                           napi_value code,
                                                           napi_value msg,
                                                           napi_value *result) {
  return create_error(env, QN_RANGE_ERROR, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_syntax_error(
    napi_env env, napi_value code, napi_value msg, napi_value *result) {
  return create_error(env, QN_SYNTAX_ERROR, code, msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, error);
  JS_Throw(env->ctx, JS_DupValue(env->ctx, qn_value(error)));
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_error(napi_env env,
                                                    const char *code,
                                                    const char *msg) {
  return throw_error(env, QN_ERROR, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_type_error(napi_env env,
                                                         const char *code,
                                                         const char *msg) {
  return throw_error(env, QN_TYPE_ERROR, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_throw_range_error(napi_env env,
                                                          const char *code,
                                                          const char *msg) {
  return throw_error(env, QN_RANGE_ERROR, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_throw_syntax_error(napi_env env,
                                                               const char *code,
                                                               const char *msg) {
  return throw_error(env, QN_SYNTAX_ERROR, code, msg);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_error(napi_env env,
                                                 napi_value value,
                                                 bool *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  *result = JS_IsError(env->ctx, qn_value(value)) != 0;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_exception_pending(napi_env env,
                                                             bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = JS_HasException(env->ctx) != 0;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_and_clear_last_exception(napi_env env, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  JSValue exc =
      JS_HasException(env->ctx) ? JS_GetException(env->ctx) : JS_UNDEFINED;
  napi_value h = qn_new_handle(env, exc);
  CHECK_HANDLE(env, h);
  *result = h;
  return qn_clear_last_error(env);
}
