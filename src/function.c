/*
 * Functions created through Node-API.
 *
 * JS_NewCFunctionData cannot be used: its trampoline does not expose
 * new.target, which napi_get_new_target and napi_define_class need. A
 * dedicated class with a `call` hook receives JS_CALL_FLAG_CONSTRUCTOR and,
 * for construct calls, new.target as `this_val`.
 *
 * Lifetime: the qn_function record is owned by the JS function object and
 * released by the class finalizer. It holds no JSValue, so no gc_mark hook
 * is needed. Each call runs inside an implicit handle scope that is closed
 * before returning to QuickJS; the result is duplicated out of the scope.
 */
#include <stdio.h>
#include <string.h>

#include "qjsnapi_internal.h"

/* Class IDs are process-wide in QuickJS; registration is per runtime. */
static JSClassID qn_function_class_id;

static void qn_function_finalizer(JSRuntime *rt, JSValue val) {
  qn_function *f = (qn_function *)JS_GetOpaque(val, qn_function_class_id);
  if (f) js_free_rt(rt, f);
}

static JSValue qn_function_call(JSContext *ctx, JSValueConst func_obj,
                                JSValueConst this_val, int argc,
                                JSValueConst *argv, int flags) {
  qn_function *f = (qn_function *)JS_GetOpaque(func_obj, qn_function_class_id);
  if (!f) return JS_ThrowTypeError(ctx, "not a Node-API function");
  napi_env env = f->env;
  if (!env->can_call_into_js) {
    return JS_ThrowInternalError(
        ctx, "Node-API function called while its environment is shutting down");
  }

  const bool construct = (flags & JS_CALL_FLAG_CONSTRUCTOR) != 0;
  JSValue receiver;
  JSValueConst new_target = JS_UNDEFINED;
  if (construct) {
    /* this_val is new.target; build the receiver from new.target.prototype
     * (OrdinaryCreateFromConstructor). */
    new_target = this_val;
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) return JS_EXCEPTION;
    receiver = JS_IsObject(proto) ? JS_NewObjectProto(ctx, proto)
                                  : JS_NewObject(ctx);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(receiver)) return JS_EXCEPTION;
  } else {
    receiver = JS_DupValue(ctx, this_val);
  }

  qn_scope *scope = qn_scope_open(env, false);
  if (!scope) {
    JS_FreeValue(ctx, receiver);
    return JS_ThrowOutOfMemory(ctx);
  }

  struct napi_callback_info__ info;
  info.this_arg = receiver;
  info.new_target = new_target;
  info.argc = argc;
  info.argv = argv;
  info.data = f->data;

  napi_value result = f->cb(env, &info);

  JSValue ret;
  if (JS_HasException(ctx)) {
    ret = JS_EXCEPTION;
  } else if (result != NULL) {
    ret = JS_DupValue(ctx, qn_value(result));
  } else {
    ret = JS_UNDEFINED;
  }
  qn_scope_close(env, scope);

  if (construct && !JS_IsException(ret) && !JS_IsObject(ret)) {
    JS_FreeValue(ctx, ret);
    ret = receiver; /* ownership moves to the caller */
  } else {
    JS_FreeValue(ctx, receiver);
  }
  return ret;
}

static const JSClassDef qn_function_class_def = {
    .class_name = "Function",
    .finalizer = qn_function_finalizer,
    .gc_mark = NULL,
    .call = qn_function_call,
    .exotic = NULL,
};

int qn_function_class_init(qjsnapi_host *host) {
  JS_NewClassID(&qn_function_class_id);
  if (!JS_IsRegisteredClass(host->rt, qn_function_class_id)) {
    if (JS_NewClass(host->rt, qn_function_class_id, &qn_function_class_def) <
        0) {
      JS_ThrowInternalError(host->ctx,
                            "quickjs-napi: cannot register the function class");
      return -1;
    }
  }
  return 0;
}

JSValue qn_function_new(napi_env env, const char *name, size_t name_len,
                        napi_callback cb, void *data) {
  JSContext *ctx = env->ctx;
  JSValue fn = JS_NewObjectProtoClass(ctx, env->host->function_proto,
                                      qn_function_class_id);
  if (JS_IsException(fn)) return fn;

  qn_function *f = (qn_function *)js_malloc(ctx, sizeof(qn_function));
  if (!f) {
    JS_FreeValue(ctx, fn);
    return JS_EXCEPTION;
  }
  f->env = env;
  f->cb = cb;
  f->data = data;
  JS_SetOpaque(fn, f);
  JS_SetConstructorBit(ctx, fn, 1);

  /* Same property order and attributes as V8: length, then name, both
   * non-writable, non-enumerable, configurable. */
  if (JS_DefinePropertyValueStr(ctx, fn, "length", JS_NewInt32(ctx, 0),
                                JS_PROP_CONFIGURABLE) < 0)
    goto fail;
  JSValue name_val = JS_NewStringLen(ctx, name ? name : "", name ? name_len : 0);
  if (JS_IsException(name_val)) goto fail;
  if (JS_DefinePropertyValueStr(ctx, fn, "name", name_val,
                                JS_PROP_CONFIGURABLE) < 0)
    goto fail;

  /* Like V8 functions created by Function::New (constructor behavior
   * kAllow): an own `prototype` object whose `constructor` points back to
   * the function. The fn <-> prototype cycle is reclaimed by the QuickJS
   * cycle collector. */
  JSValue proto = JS_NewObject(ctx);
  if (JS_IsException(proto)) goto fail;
  if (JS_DefinePropertyValueStr(ctx, proto, "constructor",
                                JS_DupValue(ctx, fn),
                                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
    JS_FreeValue(ctx, proto);
    goto fail;
  }
  if (JS_DefinePropertyValueStr(ctx, fn, "prototype", proto,
                                JS_PROP_WRITABLE) < 0)
    goto fail;
  return fn;
fail:
  JS_FreeValue(ctx, fn);
  return JS_EXCEPTION;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_function(napi_env env,
                                                        const char *utf8name,
                                                        size_t length,
                                                        napi_callback cb,
                                                        void *data,
                                                        napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, cb);
  if (utf8name != NULL) {
    /* Same validation as Node.js CHECK_NEW_FROM_UTF8_LEN. */
    RETURN_STATUS_IF_FALSE(env,
                           length == NAPI_AUTO_LENGTH || length <= INT32_MAX,
                           napi_invalid_arg);
    if (length == NAPI_AUTO_LENGTH) length = strlen(utf8name);
  } else {
    length = 0;
  }

  JSValue fn = qn_function_new(env, utf8name, length, cb, data);
  CHECK_JS(env, !JS_IsException(fn));
  napi_value h = qn_new_handle(env, fn);
  CHECK_HANDLE(env, h);
  *result = h;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_cb_info(napi_env env, napi_callback_info cbinfo, size_t *argc,
                 napi_value *argv, napi_value *this_arg, void **data) {
  CHECK_ENV(env);
  CHECK_ARG(env, cbinfo);

  if (argv != NULL) {
    CHECK_ARG(env, argc);
    size_t wanted = *argc;
    size_t actual = (size_t)cbinfo->argc;
    for (size_t i = 0; i < wanted; i++) {
      JSValue v = i < actual ? JS_DupValue(env->ctx, cbinfo->argv[i])
                             : JS_UNDEFINED;
      napi_value h = qn_new_handle(env, v);
      CHECK_HANDLE(env, h);
      argv[i] = h;
    }
  }
  if (argc != NULL) *argc = (size_t)cbinfo->argc;
  if (this_arg != NULL) {
    napi_value h = qn_new_handle(env, JS_DupValue(env->ctx, cbinfo->this_arg));
    CHECK_HANDLE(env, h);
    *this_arg = h;
  }
  if (data != NULL) *data = cbinfo->data;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_new_target(
    napi_env env, napi_callback_info cbinfo, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, cbinfo);
  CHECK_ARG(env, result);
  if (JS_IsUndefined(cbinfo->new_target)) {
    *result = NULL;
  } else {
    napi_value h =
        qn_new_handle(env, JS_DupValue(env->ctx, cbinfo->new_target));
    CHECK_HANDLE(env, h);
    *result = h;
  }
  return qn_clear_last_error(env);
}
