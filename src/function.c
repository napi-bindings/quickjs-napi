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
#include <stdlib.h>
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
  NAPI_PREAMBLE(env);
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

/* ------------------------------------------------------------------------ */
/* Calls                                                                    */
/* ------------------------------------------------------------------------ */

/* napi_value[] -> JSValueConst[] (the slots already hold references). Uses
 * `stack` when it is large enough; returns NULL on out-of-memory. */
#define QN_STACK_ARGS 8

static JSValueConst *args_to_js(size_t argc, const napi_value *argv,
                                JSValueConst *stack) {
  JSValueConst *args =
      argc <= QN_STACK_ARGS ? stack
                            : (JSValueConst *)malloc(argc * sizeof(JSValue));
  if (args)
    for (size_t i = 0; i < argc; i++) args[i] = qn_value(argv[i]);
  return args;
}

static void args_free(JSValueConst *args, const JSValueConst *stack) {
  if (args != stack) free(args);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_call_function(napi_env env,
                                                      napi_value recv,
                                                      napi_value func,
                                                      size_t argc,
                                                      const napi_value *argv,
                                                      napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, recv);
  if (argc > 0) CHECK_ARG(env, argv);
  CHECK_ARG(env, func);
  JSContext *ctx = env->ctx;
  JSValue fn = qn_value(func);
  /* CHECK_TO_FUNCTION in Node.js reports napi_invalid_arg. */
  RETURN_STATUS_IF_FALSE(env, JS_IsFunction(ctx, fn), napi_invalid_arg);
  RETURN_STATUS_IF_FALSE(env, argc <= INT32_MAX, napi_invalid_arg);

  JSValueConst stack[QN_STACK_ARGS];
  JSValueConst *args = args_to_js(argc, argv, stack);
  RETURN_STATUS_IF_FALSE(env, args != NULL, napi_generic_failure);
  JSValue r = JS_Call(ctx, fn, qn_value(recv), (int)argc, args);
  args_free(args, stack);
  CHECK_JS_STATUS(env, !JS_IsException(r), napi_generic_failure);
  if (result != NULL) {
    napi_value h = qn_new_handle(env, r);
    CHECK_HANDLE(env, h);
    *result = h;
  } else {
    JS_FreeValue(ctx, r);
  }
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_new_instance(napi_env env,
                                                     napi_value constructor,
                                                     size_t argc,
                                                     const napi_value *argv,
                                                     napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, constructor);
  if (argc > 0) CHECK_ARG(env, argv);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  JSValue ctor = qn_value(constructor);
  RETURN_STATUS_IF_FALSE(env, JS_IsFunction(ctx, ctor), napi_invalid_arg);
  RETURN_STATUS_IF_FALSE(env, argc <= INT32_MAX, napi_invalid_arg);

  JSValueConst stack[QN_STACK_ARGS];
  JSValueConst *args = args_to_js(argc, argv, stack);
  RETURN_STATUS_IF_FALSE(env, args != NULL, napi_generic_failure);
  JSValue r = JS_CallConstructor(ctx, ctor, (int)argc, args);
  args_free(args, stack);
  RETURN_STATUS_IF_FALSE(env, !JS_IsException(r), napi_pending_exception);
  napi_value h = qn_new_handle(env, r);
  CHECK_HANDLE(env, h);
  *result = h;
  return GET_RETURN_STATUS(env);
}

/* ------------------------------------------------------------------------ */
/* Classes                                                                  */
/* ------------------------------------------------------------------------ */

/* Equivalent of a V8 FunctionTemplate class: a constructor function whose
 * `prototype` receives the instance properties, static properties defined
 * on the constructor itself.
 *
 * Known difference: V8 attaches a Signature to prototype methods, so calling
 * one with a receiver not created by the class throws "Illegal invocation".
 * Here such a call reaches the native callback, which must validate `this`
 * (napi_unwrap fails on foreign objects). */
NAPI_EXTERN napi_status NAPI_CDECL
napi_define_class(napi_env env, const char *utf8name, size_t length,
                  napi_callback constructor, void *data,
                  size_t property_count,
                  const napi_property_descriptor *properties,
                  napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, constructor);
  if (property_count > 0) CHECK_ARG(env, properties);
  /* CHECK_NEW_FROM_UTF8_LEN: the class name is mandatory. */
  RETURN_STATUS_IF_FALSE(env, length == NAPI_AUTO_LENGTH || length <= INT32_MAX,
                         napi_invalid_arg);
  CHECK_ARG(env, utf8name);
  if (length == NAPI_AUTO_LENGTH) length = strlen(utf8name);

  JSContext *ctx = env->ctx;
  JSValue ctor = qn_function_new(env, utf8name, length, constructor, data);
  CHECK_JS(env, !JS_IsException(ctor));
  /* The handle owns the constructor from here on. */
  napi_value ctor_h = qn_new_handle(env, ctor);
  CHECK_HANDLE(env, ctor_h);

  JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
  CHECK_JS(env, !JS_IsException(proto));
  size_t static_count = 0;
  for (size_t i = 0; i < property_count; i++) {
    const napi_property_descriptor *p = &properties[i];
    if (p->attributes & napi_static) {
      static_count++;
      continue;
    }
    napi_status st = qn_define_property(env, proto, p);
    if (st != napi_ok) {
      JS_FreeValue(ctx, proto);
      return st;
    }
  }
  JS_FreeValue(ctx, proto);

  for (size_t i = 0; static_count > 0 && i < property_count; i++) {
    const napi_property_descriptor *p = &properties[i];
    if (!(p->attributes & napi_static)) continue;
    napi_status st = qn_define_property(env, ctor, p);
    if (st != napi_ok) return st;
  }

  *result = ctor_h;
  return GET_RETURN_STATUS(env);
}
