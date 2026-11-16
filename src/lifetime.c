/*
 * Lifetime: references, externals, wraps, finalizers, type tags, instance
 * data.
 *
 * Model (Node.js 22 semantics, js_native_api_v8.cc, on QuickJS):
 *
 *   - napi_ref (struct napi_ref__) mirrors v8impl::Reference. A count > 0
 *     holds a strong JSValue. At count 0 an object, function or local
 *     symbol is held through a JS WeakRef: QuickJS reports a WeakRef target
 *     dead as soon as its refcount drops to zero, so napi_get_reference_value
 *     returns NULL exactly when the value is gone. Other primitives are
 *     released for good at count 0 (Node: persistent reset).
 *
 *   - Weak callback. V8 notifies a weak persistent when its target is
 *     collected; QuickJS has no such C hook. Every weakable target gets a
 *     *holder*: an object of the internal NapiHolder class stored in a
 *     WeakMap keyed by the target (host->holders). QuickJS drops a WeakMap
 *     entry whose key is dead during JS_RunGC; the holder is freed, and its
 *     class finalizer notifies every reference registered on it. The holder
 *     also stores the napi_wrap reference and the type tag of the target, so
 *     wrapping works on any object (frozen, proxies, functions) without
 *     touching its properties.
 *
 *   - Finalizers never run inside the GC. A reference without finalizer is
 *     reset immediately (C bookkeeping only); one with a finalizer is queued
 *     on its env (Node: EnqueueFinalizer) and run by
 *     qjsnapi_host_run_finalizers() from the event loop, or at teardown with
 *     JavaScript disabled.
 *
 *   - Teardown of an env (Node: node_napi_env__::DeleteMe): drain the queue,
 *     finalize every reference with a finalizer slot (instance data
 *     included), newest first, then every plain reference.
 *
 * Ownership: references and their lists are malloc'ed and independent of
 * the JS heap; holders and externals are QuickJS objects whose opaque is
 * released by their class finalizer. No holder ever points back to a freed
 * reference: deleting or finalizing a reference detaches it first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qjsnapi_internal.h"

/* Class IDs are process-wide in QuickJS; registration is per runtime. */
static JSClassID qn_holder_class_id;
static JSClassID qn_external_class_id;

/* Opaque of an external value (v8impl::ExternalWrapper). */
typedef struct qn_external {
  void *data;
  bool has_tag;
  napi_type_tag tag;
} qn_external;

static void ref_target_collected(napi_ref ref);

/* ------------------------------------------------------------------------ */
/* Classes                                                                  */
/* ------------------------------------------------------------------------ */

/* Runs when the WeakMap entry of a dead target is dropped (JS_RunGC), or
 * when the WeakMap itself is freed at host destruction (no reference is
 * registered any more by then). Only C bookkeeping happens here. */
static void holder_finalizer(JSRuntime *rt, JSValue val) {
  qn_holder *h = (qn_holder *)JS_GetOpaque(val, qn_holder_class_id);
  if (!h) return;
  h->wrap = NULL;
  while (!qn_list_empty(&h->refs)) {
    napi_ref ref = QN_CONTAINER_OF(h->refs.next, struct napi_ref__, holder_link);
    qn_list_del(&ref->holder_link);
    ref->holder = NULL;
    ref_target_collected(ref);
  }
  js_free_rt(rt, h);
}

static void external_finalizer(JSRuntime *rt, JSValue val) {
  qn_external *e = (qn_external *)JS_GetOpaque(val, qn_external_class_id);
  if (e) js_free_rt(rt, e);
}

static const JSClassDef holder_class_def = {
    .class_name = "NapiHolder",
    .finalizer = holder_finalizer,
};

/* An external is an ordinary object with a null prototype, like the
 * v8::External seen from JavaScript (typeof 'object'). */
static const JSClassDef external_class_def = {
    .class_name = "Object",
    .finalizer = external_finalizer,
};

static int register_class(JSRuntime *rt, JSClassID *id, const JSClassDef *def) {
  JS_NewClassID(id);
  if (JS_IsRegisteredClass(rt, *id)) return 0;
  return JS_NewClass(rt, *id, def);
}

/* globalThis[ctor] and, when `member` is set, ctor.prototype[member]. */
static int capture(JSContext *ctx, JSValueConst global, const char *ctor,
                   const char *member, JSValue *out) {
  JSValue v = JS_GetPropertyStr(ctx, global, ctor);
  if (!JS_IsException(v) && member) {
    JSValue proto = JS_GetPropertyStr(ctx, v, "prototype");
    JS_FreeValue(ctx, v);
    v = JS_IsException(proto) ? proto : JS_GetPropertyStr(ctx, proto, member);
    if (!JS_IsException(proto)) JS_FreeValue(ctx, proto);
  }
  if (JS_IsException(v)) return -1;
  if (!JS_IsFunction(ctx, v)) {
    JS_FreeValue(ctx, v);
    JS_ThrowInternalError(ctx, "quickjs-napi: %s%s%s is not available", ctor,
                          member ? ".prototype." : "", member ? member : "");
    return -1;
  }
  *out = v;
  return 0;
}

int qn_lifetime_init(qjsnapi_host *host) {
  JSContext *ctx = host->ctx;
  if (register_class(host->rt, &qn_holder_class_id, &holder_class_def) < 0 ||
      register_class(host->rt, &qn_external_class_id, &external_class_def) <
          0) {
    JS_ThrowInternalError(ctx, "quickjs-napi: cannot register the classes");
    return -1;
  }
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue weakmap_ctor = JS_UNDEFINED;
  int r = capture(ctx, global, "WeakRef", NULL, &host->weakref_ctor);
  if (r == 0) r = capture(ctx, global, "WeakRef", "deref", &host->weakref_deref);
  if (r == 0) r = capture(ctx, global, "WeakMap", "get", &host->weakmap_get);
  if (r == 0) r = capture(ctx, global, "WeakMap", "set", &host->weakmap_set);
  if (r == 0) r = capture(ctx, global, "WeakMap", NULL, &weakmap_ctor);
  JS_FreeValue(ctx, global);
  if (r < 0) return -1;
  host->holders = JS_CallConstructor(ctx, weakmap_ctor, 0, NULL);
  JS_FreeValue(ctx, weakmap_ctor);
  if (JS_IsException(host->holders)) {
    host->holders = JS_UNDEFINED;
    return -1;
  }
  return 0;
}

bool qn_is_external(JSValueConst v) {
  return JS_GetOpaque(v, qn_external_class_id) != NULL;
}

/* ------------------------------------------------------------------------ */
/* Holders                                                                  */
/* ------------------------------------------------------------------------ */

/* Looks up (and with `create`, makes) the holder of `target`. Returns -1
 * with an exception pending on failure, in particular a TypeError when the
 * target cannot be a WeakMap key (a registered symbol). */
static int holder_get(napi_env env, JSValueConst target, bool create,
                      qn_holder **out) {
  JSContext *ctx = env->ctx;
  qjsnapi_host *host = env->host;
  *out = NULL;
  JSValue found = JS_Call(ctx, host->weakmap_get, host->holders, 1, &target);
  if (JS_IsException(found)) return -1;
  if (!JS_IsUndefined(found)) {
    *out = (qn_holder *)JS_GetOpaque(found, qn_holder_class_id);
    JS_FreeValue(ctx, found);
    return 0;
  }
  if (!create) return 0;

  qn_holder *h = (qn_holder *)js_mallocz(ctx, sizeof(qn_holder));
  if (!h) return -1;
  qn_list_init(&h->refs);
  JSValue obj = JS_NewObjectProtoClass(ctx, JS_NULL, qn_holder_class_id);
  if (JS_IsException(obj)) {
    js_free(ctx, h);
    return -1;
  }
  JS_SetOpaque(obj, h);
  JSValueConst args[2] = {target, obj};
  JSValue r = JS_Call(ctx, host->weakmap_set, host->holders, 2, args);
  /* On success the WeakMap owns the holder; on failure this frees it. */
  JS_FreeValue(ctx, obj);
  if (JS_IsException(r)) return -1;
  JS_FreeValue(ctx, r);
  *out = h;
  return 0;
}

/* ------------------------------------------------------------------------ */
/* References                                                               */
/* ------------------------------------------------------------------------ */

static void unlink_if_linked(qn_list *el) {
  if (el->next) qn_list_del(el);
}

static void ref_dequeue(napi_ref ref) {
  if (ref->queued) {
    qn_list_del(&ref->queue_link);
    ref->queued = false;
  }
}

static void ref_enqueue(napi_ref ref) {
  if (!ref->queued) {
    qn_list_add_tail(&ref->queue_link, &ref->env->pending_finalizers);
    ref->queued = true;
  }
}

static void ref_detach_holder(napi_ref ref) {
  if (ref->holder) {
    if (ref->holder->wrap == ref) ref->holder->wrap = NULL;
    qn_list_del(&ref->holder_link);
    ref->holder = NULL;
  }
}

/* Node: persistent_.Reset(). */
static void ref_reset(napi_ref ref) {
  JSContext *ctx = ref->env->ctx;
  JSValue value = ref->value, weak = ref->weak;
  ref->value = JS_UNDEFINED;
  ref->weak = JS_UNDEFINED;
  ref->empty = true;
  /* Fields are cleared first: freeing may run finalizers re-entering here. */
  JS_FreeValue(ctx, value);
  JS_FreeValue(ctx, weak);
}

/* Node: Reference::SetWeak(). Returns -1 (exception pending) only when the
 * WeakRef cannot be allocated; the reference then stays strong. */
static int ref_set_weak(napi_ref ref) {
  if (ref->keep_strong) return 0;
  if (ref->can_be_weak) {
    JSContext *ctx = ref->env->ctx;
    JSValue w = JS_CallConstructor(ctx, ref->env->host->weakref_ctor, 1,
                                   (JSValueConst *)&ref->value);
    if (JS_IsException(w)) return -1;
    ref->weak = w;
    JSValue v = ref->value;
    ref->value = JS_UNDEFINED;
    JS_FreeValue(ctx, v);
    return 0;
  }
  ref_reset(ref);
  return 0;
}

/* Current value of a weak reference: a new reference, or JS_UNDEFINED once
 * the target is dead. */
static JSValue ref_deref(napi_ref ref) {
  JSContext *ctx = ref->env->ctx;
  JSValue v = JS_Call(ctx, ref->env->host->weakref_deref, ref->weak, 0, NULL);
  if (JS_IsException(v)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
  }
  return v;
}

/* Creates a reference to `value` (Node: Reference::New and friends). Returns
 * NULL with an exception pending on failure. */
static napi_ref ref_new(napi_env env, JSValueConst value, uint32_t count,
                        bool runtime_owned, bool finalizing,
                        napi_finalize finalize_cb, void *data, void *hint) {
  JSContext *ctx = env->ctx;
  napi_ref ref = (napi_ref)calloc(1, sizeof(struct napi_ref__));
  if (!ref) {
    JS_ThrowOutOfMemory(ctx);
    return NULL;
  }
  ref->env = env;
  ref->value = JS_UNDEFINED;
  ref->weak = JS_UNDEFINED;
  ref->count = count;
  ref->runtime_owned = runtime_owned;
  ref->finalizing = finalizing;
  ref->finalize_cb = finalize_cb;
  ref->data = data;
  ref->hint = hint;

  if (JS_IsObject(value) || JS_IsSymbol(value)) {
    qn_holder *h;
    if (holder_get(env, value, true, &h) == 0) {
      ref->can_be_weak = true;
      ref->holder = h;
      qn_list_add_tail(&ref->holder_link, &h->refs);
    } else if (JS_IsSymbol(value)) {
      /* Registered symbol: not a valid weak target, never collected. */
      JS_FreeValue(ctx, JS_GetException(ctx));
      ref->keep_strong = true;
    } else {
      free(ref);
      return NULL;
    }
  }
  ref->value = JS_DupValue(ctx, value);
  if (count == 0 && ref_set_weak(ref) < 0) {
    ref_detach_holder(ref);
    JS_FreeValue(ctx, ref->value);
    free(ref);
    return NULL;
  }
  qn_list_add_tail(&ref->link,
                   finalizing ? &env->finalizing_reflist : &env->reflist);
  return ref;
}

/* A finalizer slot with no JS value (Node: TrackedFinalizer). */
static napi_ref tracker_new(napi_env env, napi_finalize finalize_cb,
                            void *data, void *hint) {
  napi_ref ref = (napi_ref)calloc(1, sizeof(struct napi_ref__));
  if (!ref) return NULL;
  ref->env = env;
  ref->value = JS_UNDEFINED;
  ref->weak = JS_UNDEFINED;
  ref->empty = true;
  ref->tracker = true;
  ref->runtime_owned = true;
  ref->finalizing = true;
  ref->finalize_cb = finalize_cb;
  ref->data = data;
  ref->hint = hint;
  qn_list_add_tail(&ref->link, &env->finalizing_reflist);
  return ref;
}

/* Node: ~Reference / ~ReferenceWithFinalizer. No finalizer runs. */
static void ref_delete(napi_ref ref) {
  napi_env env = ref->env;
  if (!ref->tracker) ref_reset(ref);
  unlink_if_linked(&ref->link);
  ref_dequeue(ref);
  ref_detach_holder(ref);
  if (env->instance_data == ref) env->instance_data = NULL;
  free(ref);
}

/* Node: Reference::Finalize(). The target is gone (or the env is being torn
 * down): reset the value, unlink, run the user finalizer, and delete the
 * reference if the runtime owns it. A user-owned reference may be deleted by
 * its own finalizer, so it is not touched after the callback. */
static void ref_finalize(napi_ref ref) {
  napi_env env = ref->env;
  if (!ref->tracker) ref_reset(ref);
  bool delete_me = ref->runtime_owned;
  unlink_if_linked(&ref->link);
  ref_dequeue(ref);
  ref_detach_holder(ref);
  napi_finalize cb = ref->finalize_cb;
  void *data = ref->data, *hint = ref->hint;
  ref->finalize_cb = NULL; /* Finalizer::ResetFinalizer */
  if (cb) qn_call_finalizer(env, cb, data, hint);
  if (delete_me) {
    if (env->instance_data == ref) env->instance_data = NULL;
    free(ref);
  }
}

/* Node: Reference::WeakCallback + InvokeFinalizerFromGC. Called from the
 * holder finalizer, i.e. from inside the GC: no user code may run here. */
static void ref_target_collected(napi_ref ref) {
  ref_reset(ref);
  if (ref->finalizing)
    ref_enqueue(ref);
  else
    ref_finalize(ref); /* no callback: C bookkeeping only */
}

/* Node: Reference::Ref(). */
static uint32_t ref_ref(napi_ref ref) {
  if (ref->empty) return 0;
  if (++ref->count == 1 && ref->can_be_weak && !JS_IsUndefined(ref->weak)) {
    JSValue v = ref_deref(ref);
    if (JS_IsUndefined(v)) {
      /* Dead target whose holder has not been collected yet: V8 would have
       * an empty persistent too once the weak callback ran. */
      ref->count = 0;
      ref_reset(ref);
      return 0;
    }
    JSValue weak = ref->weak;
    ref->weak = JS_UNDEFINED;
    ref->value = v;
    JS_FreeValue(ref->env->ctx, weak);
  }
  return ref->count;
}

/* Node: Reference::Unref(). The caller checked count > 0. */
static uint32_t ref_unref(napi_ref ref) {
  if (ref->empty || ref->count == 0) return 0;
  if (--ref->count == 0 && ref_set_weak(ref) < 0) {
    /* Out of memory for the WeakRef: keep the value strong. */
    JS_FreeValue(ref->env->ctx, JS_GetException(ref->env->ctx));
  }
  return ref->count;
}

/* Value of a reference: true and a new reference in *out, or false when the
 * value is gone. */
static bool ref_get(napi_ref ref, JSValue *out) {
  if (ref->empty || ref->tracker) return false;
  if (!JS_IsUndefined(ref->weak)) {
    JSValue v = ref_deref(ref);
    if (JS_IsUndefined(v)) return false;
    *out = v;
    return true;
  }
  *out = JS_DupValue(ref->env->ctx, ref->value);
  return true;
}

/* ------------------------------------------------------------------------ */
/* Finalizer execution                                                      */
/* ------------------------------------------------------------------------ */

void qn_report_uncaught(qjsnapi_host *host, JSValue exc) {
  JSContext *ctx = host->ctx;
  if (host->uncaught_fn && !host->destroying) {
    host->uncaught_fn(host, ctx, exc, host->uncaught_opaque);
    return;
  }
  const char *s = JS_ToCString(ctx, exc);
  if (!s) JS_FreeValue(ctx, JS_GetException(ctx));
  fprintf(stderr, "quickjs-napi: uncaught exception in a Node-API callback: %s\n",
          s ? s : "(unprintable)");
  if (s) JS_FreeCString(ctx, s);
  JS_FreeValue(ctx, exc);
}

/* Node: node_napi_env__::CallFinalizer<true> (CallbackIntoModule). */
void qn_call_finalizer(napi_env env, napi_finalize cb, void *data,
                       void *hint) {
  if (!cb) return;
  JSContext *ctx = env->ctx;
  qn_scope *scope = qn_scope_open(env, false);
  uint32_t open_before = env->open_handle_scopes;
  qn_clear_last_error(env);
  cb(env, data, hint);
  if (env->open_handle_scopes != open_before) {
    napi_fatal_error("qn_call_finalizer", NAPI_AUTO_LENGTH,
                     "a finalizer left a handle scope open", NAPI_AUTO_LENGTH);
  }
  if (scope) qn_scope_close(env, scope);
  if (JS_HasException(ctx)) {
    JSValue exc = JS_GetException(ctx);
    if (!env->can_call_into_js)
      JS_FreeValue(ctx, exc); /* Node: terminatedOrTerminating() */
    else
      qn_report_uncaught(env->host, exc);
  }
}

static size_t env_drain(napi_env env) {
  size_t n = 0;
  while (!qn_list_empty(&env->pending_finalizers)) {
    napi_ref ref = QN_CONTAINER_OF(env->pending_finalizers.next,
                                   struct napi_ref__, queue_link);
    ref_dequeue(ref);
    ref_finalize(ref);
    n++;
  }
  return n;
}

static void finalize_all(qn_list *list) {
  while (!qn_list_empty(list)) {
    /* Newest first, like Node's RefTracker::FinalizeAll. */
    ref_finalize(QN_CONTAINER_OF(list->prev, struct napi_ref__, link));
  }
}

void qn_env_finalize_refs(napi_env env) {
  env->destructing = true;
  env_drain(env);
  finalize_all(&env->finalizing_reflist);
  finalize_all(&env->reflist);
  env_drain(env); /* targets that died during the passes above */
}

size_t qjsnapi_host_run_finalizers(qjsnapi_host *host) {
  if (!host || host->destroying) return 0;
  size_t n = 0;
  bool again = true;
  while (again) {
    again = false;
    for (qn_list *el = host->envs.next; el != &host->envs; el = el->next) {
      napi_env env = QN_CONTAINER_OF(el, struct napi_env__, link);
      if (!qn_list_empty(&env->pending_finalizers)) {
        n += env_drain(env);
        again = true; /* a finalizer may have queued work on another env */
        break;
      }
    }
  }
  return n;
}

int qjsnapi_host_has_pending_finalizers(qjsnapi_host *host) {
  if (!host) return 0;
  for (qn_list *el = host->envs.next; el != &host->envs; el = el->next) {
    napi_env env = QN_CONTAINER_OF(el, struct napi_env__, link);
    if (!qn_list_empty(&env->pending_finalizers)) return 1;
  }
  return 0;
}

void qjsnapi_host_set_uncaught_handler(qjsnapi_host *host,
                                       qjsnapi_uncaught_fn fn, void *opaque) {
  if (!host) return;
  host->uncaught_fn = fn;
  host->uncaught_opaque = opaque;
}

/* ------------------------------------------------------------------------ */
/* Public API: references                                                   */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_reference(
    napi_env env, napi_value value, uint32_t initial_refcount,
    napi_ref *result) {
  /* No NAPI_PREAMBLE: as in Node.js, this cannot run JavaScript. */
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  JSValue v = qn_value(value);
  if (env->module_api_version < 10) {
    RETURN_STATUS_IF_FALSE(env, JS_IsObject(v) || JS_IsSymbol(v),
                           napi_invalid_arg);
  }
  napi_ref ref = ref_new(env, v, initial_refcount, false, false, NULL, NULL,
                         NULL);
  CHECK_JS(env, ref != NULL);
  *result = ref;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_reference(node_api_basic_env env,
                                                         napi_ref ref) {
  CHECK_ENV(env);
  CHECK_ARG(env, ref);
  ref_delete(ref);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reference_ref(napi_env env,
                                                      napi_ref ref,
                                                      uint32_t *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, ref);
  uint32_t count = ref_ref(ref);
  if (result) *result = count;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reference_unref(napi_env env,
                                                        napi_ref ref,
                                                        uint32_t *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, ref);
  if (ref->count == 0) return qn_set_last_error(env, napi_generic_failure);
  uint32_t count = ref_unref(ref);
  if (result) *result = count;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_reference_value(napi_env env,
                                                            napi_ref ref,
                                                            napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, ref);
  CHECK_ARG(env, result);
  JSValue v;
  if (ref_get(ref, &v)) {
    *result = qn_new_handle(env, v);
    CHECK_HANDLE(env, *result);
  } else {
    *result = NULL;
  }
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* Public API: wrap                                                         */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_wrap(napi_env env, napi_value js_object,
                                             void *native_object,
                                             node_api_basic_finalize finalize_cb,
                                             void *finalize_hint,
                                             napi_ref *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, js_object);
  JSValue obj = qn_value(js_object);
  RETURN_STATUS_IF_FALSE(env, JS_IsObject(obj), napi_invalid_arg);
  qn_holder *h;
  CHECK_JS(env, holder_get(env, obj, true, &h) == 0);
  /* Wrapping twice is an error. */
  RETURN_STATUS_IF_FALSE(env, h->wrap == NULL, napi_invalid_arg);

  napi_ref ref;
  if (result) {
    /* The returned reference must be deleted only from the finalizer, so a
     * finalizer is required (Node: ReferenceOwnership::kUserland). */
    CHECK_ARG(env, finalize_cb);
    ref = ref_new(env, obj, 0, false, true, finalize_cb, native_object,
                  finalize_hint);
  } else if (finalize_cb) {
    ref = ref_new(env, obj, 0, true, true, finalize_cb, native_object,
                  finalize_hint);
  } else {
    ref = ref_new(env, obj, 0, true, false, NULL, native_object, NULL);
  }
  CHECK_JS(env, ref != NULL);
  h->wrap = ref;
  if (result) *result = ref;
  return GET_RETURN_STATUS(env);
}

static napi_status unwrap(napi_env env, napi_value js_object, void **result,
                          bool remove) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, js_object);
  if (!remove) CHECK_ARG(env, result);
  JSValue obj = qn_value(js_object);
  RETURN_STATUS_IF_FALSE(env, JS_IsObject(obj), napi_invalid_arg);
  qn_holder *h;
  CHECK_JS(env, holder_get(env, obj, false, &h) == 0);
  RETURN_STATUS_IF_FALSE(env, h != NULL && h->wrap != NULL, napi_invalid_arg);
  napi_ref ref = h->wrap;
  if (result) *result = ref->data;
  if (remove) {
    h->wrap = NULL;
    if (ref->runtime_owned) {
      ref_delete(ref);
    } else {
      /* The user deletes the reference; its finalizer must not run. */
      ref->finalize_cb = NULL;
      ref->data = NULL;
      ref->hint = NULL;
    }
  }
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_unwrap(napi_env env,
                                               napi_value js_object,
                                               void **result) {
  return unwrap(env, js_object, result, false);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_remove_wrap(napi_env env,
                                                    napi_value js_object,
                                                    void **result) {
  return unwrap(env, js_object, result, true);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_add_finalizer(napi_env env, napi_value js_object, void *finalize_data,
                   node_api_basic_finalize finalize_cb, void *finalize_hint,
                   napi_ref *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, js_object);
  CHECK_ARG(env, finalize_cb);
  JSValue obj = qn_value(js_object);
  RETURN_STATUS_IF_FALSE(env, JS_IsObject(obj), napi_invalid_arg);
  /* Without `result` the runtime owns (and deletes) the reference. */
  napi_ref ref = ref_new(env, obj, 0, result == NULL, true, finalize_cb,
                         finalize_data, finalize_hint);
  CHECK_JS(env, ref != NULL);
  if (result) *result = ref;
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* Public API: externals                                                    */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_external(napi_env env, void *data,
                     node_api_basic_finalize finalize_cb, void *finalize_hint,
                     napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  JSContext *ctx = env->ctx;
  qn_external *e = (qn_external *)js_mallocz(ctx, sizeof(qn_external));
  CHECK_JS(env, e != NULL);
  e->data = data;
  JSValue obj = JS_NewObjectProtoClass(ctx, JS_NULL, qn_external_class_id);
  if (JS_IsException(obj)) {
    js_free(ctx, e);
    CHECK_JS(env, false);
  }
  JS_SetOpaque(obj, e);
  /* Like a v8::External: no own properties can ever be added. */
  if (JS_PreventExtensions(ctx, obj) < 0) {
    JS_FreeValue(ctx, obj);
    CHECK_JS(env, false);
  }
  if (finalize_cb) {
    /* Self-deleting reference that runs the finalizer once the external is
     * collected (Node: ReferenceWithFinalizer, kRuntime). */
    napi_ref ref = ref_new(env, obj, 0, true, true, finalize_cb, data,
                           finalize_hint);
    if (!ref) {
      JS_FreeValue(ctx, obj);
      CHECK_JS(env, false);
    }
  }
  *result = qn_new_handle(env, obj);
  CHECK_HANDLE(env, *result);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_external(napi_env env,
                                                           napi_value value,
                                                           void **result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);
  qn_external *e =
      (qn_external *)JS_GetOpaque(qn_value(value), qn_external_class_id);
  RETURN_STATUS_IF_FALSE(env, e != NULL, napi_invalid_arg);
  *result = e->data;
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* Public API: type tags                                                    */
/* ------------------------------------------------------------------------ */

static bool tag_equal(const napi_type_tag *a, const napi_type_tag *b) {
  return a->lower == b->lower && a->upper == b->upper;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_type_tag_object(
    napi_env env, napi_value value, const napi_type_tag *type_tag) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  JSContext *ctx = env->ctx;
  JSValue v = qn_value(value);
  qn_external *e = (qn_external *)JS_GetOpaque(v, qn_external_class_id);
  if (e) {
    CHECK_ARG(env, type_tag);
    RETURN_STATUS_IF_FALSE(env, !e->has_tag, napi_invalid_arg);
    e->tag = *type_tag;
    e->has_tag = true;
    return GET_RETURN_STATUS(env);
  }
  /* Node: CHECK_TO_OBJECT_WITH_PREAMBLE, then CHECK_ARG(type_tag). */
  JSValue obj = qn_to_object(env, v);
  RETURN_STATUS_IF_FALSE(env, !JS_IsException(obj), napi_object_expected);
  if (!type_tag) {
    JS_FreeValue(ctx, obj);
    return qn_set_last_error(env, napi_invalid_arg);
  }
  qn_holder *h;
  int r = holder_get(env, obj, true, &h);
  JS_FreeValue(ctx, obj);
  CHECK_JS(env, r == 0);
  RETURN_STATUS_IF_FALSE(env, !h->has_tag, napi_invalid_arg);
  h->tag = *type_tag;
  h->has_tag = true;
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_check_object_type_tag(
    napi_env env, napi_value value, const napi_type_tag *type_tag,
    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  JSContext *ctx = env->ctx;
  JSValue v = qn_value(value);
  qn_external *e = (qn_external *)JS_GetOpaque(v, qn_external_class_id);
  if (e) {
    CHECK_ARG(env, type_tag);
    CHECK_ARG(env, result);
    *result = e->has_tag && tag_equal(&e->tag, type_tag);
    return GET_RETURN_STATUS(env);
  }
  JSValue obj = qn_to_object(env, v);
  RETURN_STATUS_IF_FALSE(env, !JS_IsException(obj), napi_object_expected);
  if (!type_tag || !result) {
    JS_FreeValue(ctx, obj);
    return qn_set_last_error(env, napi_invalid_arg);
  }
  qn_holder *h;
  int r = holder_get(env, obj, false, &h);
  JS_FreeValue(ctx, obj);
  CHECK_JS(env, r == 0);
  *result = h != NULL && h->has_tag && tag_equal(&h->tag, type_tag);
  return GET_RETURN_STATUS(env);
}

/* ------------------------------------------------------------------------ */
/* Public API: instance data, external memory                               */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL
napi_set_instance_data(node_api_basic_env env, void *data,
                       napi_finalize finalize_cb, void *finalize_hint) {
  CHECK_ENV(env);
  /* As in Node.js, previous data is dropped without being finalized. */
  if (env->instance_data) ref_delete(env->instance_data);
  env->instance_data = tracker_new(env, finalize_cb, data, finalize_hint);
  RETURN_STATUS_IF_FALSE(env, env->instance_data != NULL,
                         napi_generic_failure);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_instance_data(node_api_basic_env env, void **data) {
  CHECK_ENV(env);
  CHECK_ARG(env, data);
  *data = env->instance_data ? env->instance_data->data : NULL;
  return qn_clear_last_error(env);
}

/* QuickJS has no notion of external memory pressure: the counter is kept
 * per host, like V8 keeps it per isolate, and never goes below zero. */
NAPI_EXTERN napi_status NAPI_CDECL napi_adjust_external_memory(
    node_api_basic_env env, int64_t change_in_bytes, int64_t *adjusted_value) {
  CHECK_ENV(env);
  CHECK_ARG(env, adjusted_value);
  qjsnapi_host *host = env->host;
  int64_t v = host->external_memory;
  if (change_in_bytes > 0 && v > INT64_MAX - change_in_bytes)
    v = INT64_MAX;
  else
    v += change_in_bytes;
  if (v < 0) v = 0;
  host->external_memory = v;
  *adjusted_value = v;
  return qn_clear_last_error(env);
}
