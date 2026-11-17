/*
 * The synchronous part of node_api.h that has no async_hooks to talk to:
 * napi_async_init, napi_async_destroy, napi_make_callback,
 * napi_open_callback_scope, napi_close_callback_scope, napi_fatal_exception.
 *
 * quickjs-napi has no async_hooks, so an async context only records that it
 * exists (no init/destroy hooks, no executionAsyncResource). What remains of
 * Node's InternalCallbackScope is the microtask checkpoint: when the
 * outermost callback scope closes and no JavaScript frame is below the
 * current native code (host->native_depth == 0), the pending promise jobs
 * run, as Node.js runs its tick and microtask queues there. Inside a
 * Node-API callback called from JavaScript (the common case), nothing runs
 * early, exactly like Node.js, where the caller's own scope is still open.
 */
#include <stdio.h>
#include <stdlib.h>

#include "qjsnapi_internal.h"

struct napi_async_context__ {
  qn_list link; /* env->async_contexts */
  napi_env env;
};

struct napi_callback_scope__ {
  napi_env env;
};

void qn_env_free_async_contexts(napi_env env) {
  while (!qn_list_empty(&env->async_contexts)) {
    napi_async_context c = QN_CONTAINER_OF(
        env->async_contexts.next, struct napi_async_context__, link);
    qn_list_del(&c->link);
    free(c);
  }
}

void qn_microtask_checkpoint(qjsnapi_host *host) {
  if (host->checkpoint_fn) {
    host->checkpoint_fn(host, host->ctx, host->checkpoint_opaque);
    return;
  }
  for (;;) {
    JSContext *job_ctx;
    int r = JS_ExecutePendingJob(host->rt, &job_ctx);
    if (r == 0) break;
    if (r < 0) qn_report_uncaught(host, JS_GetException(job_ctx));
  }
}

/* V8's ToObject fails, with a pending TypeError, only for undefined and
 * null (Node: CHECK_TO_OBJECT inside NAPI_PREAMBLE's TryCatch). */
static bool to_object_ok(napi_env env, JSValueConst v) {
  if (!JS_IsNull(v) && !JS_IsUndefined(v)) return true;
  JS_ThrowTypeError(env->ctx, "Cannot convert undefined or null to object");
  return false;
}

static void callback_scope_enter(napi_env env) {
  env->host->callback_scope_depth++;
}

/* Node: ProcessEmitDeprecationWarning, once per code. */
static void emit_dep0168(qjsnapi_host *host) {
  if (host->dep0168_emitted) return;
  host->dep0168_emitted = true;
  fprintf(stderr,
          "(quickjs-napi:%d) [DEP0168] DeprecationWarning: Uncaught Node-API "
          "callback exception detected, please run with option "
          "--force-node-api-uncaught-exceptions-policy=true to handle those "
          "exceptions properly.\n",
          (int)uv_os_getpid());
}

/* Node: the exception handler of node_napi_env__::CallbackIntoModule. */
void qn_report_callback_exception(napi_env env, JSValue exc,
                                  bool enforce_policy) {
  qjsnapi_host *host = env->host;
  if (!env->can_call_into_js) { /* terminatedOrTerminating() */
    JS_FreeValue(env->ctx, exc);
    return;
  }
  if (!enforce_policy && env->module_api_version < 10 &&
      !host->force_uncaught_policy) {
    JS_FreeValue(env->ctx, exc);
    emit_dep0168(host);
    return;
  }
  qn_report_uncaught(host, exc);
}

/* Node: InternalCallbackScope::Close. */
static void callback_scope_leave(napi_env env) {
  qjsnapi_host *host = env->host;
  host->callback_scope_depth--;
  if (host->callback_scope_depth == 0 && host->native_depth == 0 &&
      env->can_call_into_js && !JS_HasException(env->ctx)) {
    qn_microtask_checkpoint(host);
  }
}

void qn_callback_enter(napi_env env, qn_callback_frame *frame) {
  frame->scope = qn_scope_open(env, false);
  frame->open_handle_scopes = env->open_handle_scopes;
  callback_scope_enter(env);
  env->host->native_depth++;
  qn_clear_last_error(env); /* Node: CallIntoModule */
}

void qn_callback_leave(napi_env env, qn_callback_frame *frame,
                       bool enforce_policy) {
  env->host->native_depth--;
  if (env->open_handle_scopes != frame->open_handle_scopes) {
    napi_fatal_error("qn_callback_leave", NAPI_AUTO_LENGTH,
                     "a callback left a handle scope open", NAPI_AUTO_LENGTH);
  }
  if (JS_HasException(env->ctx))
    qn_report_callback_exception(env, JS_GetException(env->ctx),
                                 enforce_policy);
  if (frame->scope) qn_scope_close(env, frame->scope);
  callback_scope_leave(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_async_init(
    napi_env env, napi_value async_resource, napi_value async_resource_name,
    napi_async_context *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, async_resource_name);
  CHECK_ARG(env, result);
  /* Node: CHECK_TO_OBJECT on the resource, outside of any TryCatch: the
   * status is napi_object_expected and no exception reaches the caller. */
  if (async_resource != NULL) {
    JSValue r = qn_value(async_resource);
    RETURN_STATUS_IF_FALSE(env, !JS_IsNull(r) && !JS_IsUndefined(r),
                           napi_object_expected);
  }
  napi_async_context c =
      (napi_async_context)calloc(1, sizeof(struct napi_async_context__));
  RETURN_STATUS_IF_FALSE(env, c != NULL, napi_generic_failure);
  c->env = env;
  qn_list_add_tail(&c->link, &env->async_contexts);
  *result = c;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_async_destroy(napi_env env, napi_async_context async_context) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, async_context);
  qn_list_del(&async_context->link);
  free(async_context);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_make_callback(
    napi_env env, napi_async_context async_context, napi_value recv,
    napi_value func, size_t argc, const napi_value *argv,
    napi_value *result) {
  (void)async_context; /* may be NULL, as in Node.js */
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, recv);
  if (argc > 0) CHECK_ARG(env, argv);
  CHECK_ARG(env, func);
  JSContext *ctx = env->ctx;
  JSValue r = qn_value(recv);
  if (!to_object_ok(env, r)) return qn_set_last_error(env, napi_object_expected);
  JSValue f = qn_value(func);
  RETURN_STATUS_IF_FALSE(env, JS_IsFunction(ctx, f), napi_function_expected);
  RETURN_STATUS_IF_FALSE(env, argc <= 65535, napi_invalid_arg);

  /* Node: CHECK_TO_OBJECT(recv), so a primitive receiver is boxed. */
  JSValue this_obj = JS_IsObject(r)
                         ? JS_DupValue(ctx, r)
                         : JS_Call(ctx, env->host->object_ctor, JS_UNDEFINED,
                                   1, &r);
  CHECK_JS(env, !JS_IsException(this_obj));

  JSValue stack_args[8];
  JSValue *args = argc <= 8 ? stack_args
                            : (JSValue *)malloc(argc * sizeof(JSValue));
  if (!args) {
    JS_FreeValue(ctx, this_obj);
    return qn_set_last_error(env, napi_generic_failure);
  }
  for (size_t i = 0; i < argc; i++) args[i] = qn_value(argv[i]);

  callback_scope_enter(env);
  JSValue ret = JS_Call(ctx, f, this_obj, (int)argc, (JSValueConst *)args);
  callback_scope_leave(env);

  if (args != stack_args) free(args);
  JS_FreeValue(ctx, this_obj);
  CHECK_JS(env, !JS_IsException(ret));
  if (result) {
    *result = qn_new_handle(env, ret);
    CHECK_HANDLE(env, *result);
  } else {
    JS_FreeValue(ctx, ret);
  }
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_open_callback_scope(napi_env env, napi_value resource_object,
                         napi_async_context context,
                         napi_callback_scope *result) {
  (void)resource_object; /* ignored, as in Node.js */
  CHECK_ENV(env);
  CHECK_ARG(env, context);
  CHECK_ARG(env, result);
  napi_callback_scope s =
      (napi_callback_scope)malloc(sizeof(struct napi_callback_scope__));
  RETURN_STATUS_IF_FALSE(env, s != NULL, napi_generic_failure);
  s->env = env;
  env->open_callback_scopes++;
  callback_scope_enter(env);
  *result = s;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_close_callback_scope(napi_env env, napi_callback_scope scope) {
  CHECK_ENV(env);
  CHECK_ARG(env, scope);
  RETURN_STATUS_IF_FALSE(env, env->open_callback_scopes > 0,
                         napi_callback_scope_mismatch);
  free(scope);
  env->open_callback_scopes--;
  callback_scope_leave(env);
  return qn_clear_last_error(env);
}

/* Node: TriggerUncaughtException, synchronously: process
 * 'uncaughtException' listeners run before this returns. */
NAPI_EXTERN napi_status NAPI_CDECL napi_fatal_exception(napi_env env,
                                                        napi_value err) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, err);
  qn_report_uncaught(env->host, JS_DupValue(env->ctx, qn_value(err)));
  return qn_clear_last_error(env);
}
