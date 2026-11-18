/*
 * quickjs-napi - internal definitions shared by the implementation files.
 *
 * Ownership model (see PIANO-IMPLEMENTAZIONE.md, section 4):
 *   - napi_value is a pointer to a JSValue slot living in the handle-scope
 *     arena of the env. Every slot OWNS one reference (JS_DupValue on entry,
 *     JS_FreeValue when the owning scope is closed).
 *   - One qjsnapi_host per JSContext (JS_SetContextOpaque), one napi_env__
 *     per loaded addon, as in Node.js.
 */
#ifndef QJSNAPI_INTERNAL_H
#define QJSNAPI_INTERNAL_H

#ifndef NAPI_VERSION
#define NAPI_VERSION 10
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uv.h>

#include "quickjs.h"
#include "node_api.h"
#include "qjsnapi.h"

/* Highest Node-API version implemented by this library. */
#define QN_NAPI_VERSION 10
/* Default module API version when an addon does not export
 * node_api_module_get_api_version_v1 (same default as Node.js). */
#define QN_DEFAULT_MODULE_API_VERSION 8

#if defined(__GNUC__) || defined(__clang__)
#define QN_INTERNAL __attribute__((visibility("hidden")))
#else
#define QN_INTERNAL
#endif

/* ------------------------------------------------------------------------ */
/* Intrusive doubly linked list                                             */
/* ------------------------------------------------------------------------ */

typedef struct qn_list {
  struct qn_list *prev, *next;
} qn_list;

static inline void qn_list_init(qn_list *h) { h->prev = h->next = h; }
static inline bool qn_list_empty(const qn_list *h) { return h->next == h; }
static inline void qn_list_add_tail(qn_list *el, qn_list *h) {
  el->prev = h->prev;
  el->next = h;
  h->prev->next = el;
  h->prev = el;
}
static inline void qn_list_del(qn_list *el) {
  el->prev->next = el->next;
  el->next->prev = el->prev;
  el->prev = el->next = NULL;
}
#define QN_CONTAINER_OF(ptr, type, member) \
  ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* ------------------------------------------------------------------------ */
/* Handle-scope arena                                                       */
/* ------------------------------------------------------------------------ */

#define QN_CHUNK_SLOTS 256

typedef struct qn_chunk {
  struct qn_chunk *prev;
  struct qn_chunk *next;
  JSValue slots[QN_CHUNK_SLOTS];
} qn_chunk;

/* Position in the arena: the next free slot is chunk->slots[pos]. */
typedef struct qn_mark {
  qn_chunk *chunk;
  size_t pos;
} qn_mark;

#define QN_SCOPE_MAGIC 0x51534350u /* "QSCP" */

typedef struct qn_scope {
  uint32_t magic;
  bool user; /* opened by napi_open_*handle_scope (counted) */
  bool escapable;
  bool escaped;
  struct qn_scope *parent;
  qn_mark mark;         /* arena position when the scope was opened */
  JSValue *escape_slot; /* slot reserved in the parent (escapable only) */
} qn_scope;

typedef struct qn_arena {
  qn_chunk *first;
  qn_mark top;           /* current allocation position */
  qn_scope *current;     /* innermost open scope */
  qn_scope *free_scopes; /* recycled scope records (linked via parent) */
} qn_arena;

/* ------------------------------------------------------------------------ */
/* Host (one per JSContext) and env (one per loaded addon)                  */
/* ------------------------------------------------------------------------ */

/* One entry of env->cleanup_hooks, run LIFO at teardown. */
typedef enum {
  QN_HOOK_USER,     /* napi_add_env_cleanup_hook: fun(arg) */
  QN_HOOK_ASYNC,    /* napi_add_async_cleanup_hook: arg is the handle */
  QN_HOOK_INTERNAL, /* library (thread-safe functions): fun(arg) */
} qn_hook_kind;

typedef struct qn_cleanup_hook {
  qn_list link;
  qn_hook_kind kind;
  napi_cleanup_hook fun;
  void *arg;
} qn_cleanup_hook;

/* One per initialization: every qjsnapi_load_addon of a path runs the
 * initializer again with a new env, as Node.js does for each dlopen. */
typedef struct qn_module_record {
  qn_list link;
  char *path;    /* path as given to the loader, UTF-8 */
  void *handle;  /* dlopen / LoadLibrary handle, never closed */
  napi_env env;
  /* napi_module registered by the library's static constructor, which only
   * runs on the first dlopen; reused by later loads (Node.js keeps it in its
   * global handle map). */
  napi_module *legacy;
  /* Static addons only (qjsnapi_init_static_addon caches its exports):
   * strong reference, JS_UNDEFINED for shared libraries. */
  JSValue exports;
} qn_module_record;

enum qn_error_kind {
  QN_ERROR,
  QN_TYPE_ERROR,
  QN_RANGE_ERROR,
  QN_SYNTAX_ERROR,
  QN_ERROR_KIND_COUNT
};

struct qjsnapi_host {
  JSRuntime *rt;
  JSContext *ctx;
  JSValue function_proto; /* %Function.prototype%, captured at init */
  JSValue error_ctors[QN_ERROR_KIND_COUNT]; /* captured at init */
  /* Builtins used by APIs without a QuickJS C equivalent, captured before
   * any user script runs (immune to later monkey-patching). */
  JSValue object_ctor;   /* Object: ToObject for primitives */
  JSValue object_freeze; /* Object.freeze */
  JSValue object_seal;   /* Object.seal */
  JSValue symbol_ctor;   /* Symbol */
  JSValue symbol_for;    /* Symbol.for */
  /* Weak references (lifetime.c). QuickJS has no C API for them: the JS
   * builtins are used, captured here before any user script runs. */
  JSValue weakref_ctor;  /* WeakRef */
  JSValue weakref_deref; /* WeakRef.prototype.deref */
  JSValue weakmap_get;   /* WeakMap.prototype.get */
  JSValue weakmap_set;   /* WeakMap.prototype.set */
  JSValue holders;       /* WeakMap: target -> NapiHolder object */
  /* Binary data, dates, BigInt (builtins.c): constructors and accessors
   * captured from the pristine builtins, so that user code patching them
   * cannot change what Node-API reports. */
  JSValue arraybuffer_ctor;   /* ArrayBuffer */
  JSValue dataview_ctor;      /* DataView */
  JSValue dv_buffer;          /* get DataView.prototype.buffer */
  JSValue dv_byte_length;     /* get DataView.prototype.byteLength */
  JSValue dv_byte_offset;     /* get DataView.prototype.byteOffset */
  JSValue ta_buffer;          /* get %TypedArray%.prototype.buffer */
  JSValue ta_byte_length;     /* get %TypedArray%.prototype.byteLength */
  JSValue date_get_time;      /* Date.prototype.getTime */
  JSValue bigint_from_hex;    /* (negative, hex) => BigInt */
  JSValue bigint_to_hex;      /* (bigint) => hex string, "-" when negative */
  /* Buffer (buffer.c, src/buffer.js). */
  JSValue buffer_ctor;        /* Buffer */
  JSValue buffer_make;        /* (arrayBuffer, byteOffset, length) => Buffer */
  qn_list envs;           /* napi_env__.link, in creation order */
  qn_list modules;        /* qn_module_record.link */
  int64_t external_memory; /* napi_adjust_external_memory counter */
  /* Class IDs of builtin objects, found by probing instances at startup
   * (QuickJS does not export its class ID enum). 0 when not available. */
  JSClassID cid_arraybuffer;
  JSClassID cid_shared_arraybuffer;
  JSClassID cid_dataview;
  JSClassID cid_date;
  JSClassID cid_typed[12]; /* indexed by JSTypedArrayEnum */
  qjsnapi_uncaught_fn uncaught_fn;
  void *uncaught_opaque;
  /* Native code entered from JavaScript or from the host and still running
   * (Node-API callbacks, finalizers, module init). 0 means no JavaScript
   * frame is below the current native code. */
  uint32_t native_depth;
  /* Open callback scopes of every env (Node: async_callback_scope_depth). */
  uint32_t callback_scope_depth;
  /* Event loop (loop.c). `loop` is own_loop unless the embedder gave one. */
  uv_loop_t *loop;
  uv_loop_t own_loop;
  bool owns_loop;
  /* Queued finalizers run from the loop: the check handle (unreferenced)
   * runs them, the idle handle is active while some are queued (keeps the
   * loop alive and stops it from blocking; Node: a SetImmediate). */
  uv_check_t finalizer_check;
  uv_idle_t finalizer_idle;
  bool finalizer_handles;
  qjsnapi_checkpoint_fn checkpoint_fn;
  void *checkpoint_opaque;
  /* --force-node-api-uncaught-exceptions-policy */
  bool force_uncaught_policy;
  bool dep0168_emitted;
  /* Work the teardown waits for (uv_run until all are 0). */
  uint32_t active_works;          /* async work queued, not completed */
  uint32_t live_tsfns;            /* thread-safe functions not finalized */
  uint32_t running_async_cleanup; /* async cleanup hooks started, not removed */
  bool destroying;
  /* Targets of references that became weak, held strongly until the next
   * qjsnapi_host_clear_kept_objects (lifetime.c). */
  JSValue *kept;
  size_t kept_len, kept_cap;
};

/* Per-target side record of the lifetime machinery, stored as the opaque of
 * a NapiHolder object kept in host->holders (a WeakMap keyed by the target).
 * When the target dies, the next JS_RunGC drops the WeakMap entry, the
 * holder object is freed and its class finalizer notifies every reference
 * still registered on it: this is the equivalent of V8's weak callback. */
typedef struct qn_holder {
  qn_list refs;  /* napi_ref__.holder_link */
  napi_ref wrap; /* reference created by napi_wrap, or NULL */
  bool has_tag;
  napi_type_tag tag;
} qn_holder;

/* napi_ref, and every other tracked finalizer (instance data). Mirrors
 * v8impl::Reference / ReferenceWithFinalizer / TrackedFinalizer.
 *
 *   count > 0           value holds a strong reference
 *   count == 0, weak    weak holds a WeakRef to the target (objects,
 *                       functions, non-registered symbols)
 *   count == 0, !weak   the value is released for good (`empty`)
 *
 * Registered symbols (Symbol.for) cannot be held weakly by QuickJS; they
 * are never collected anyway, so they stay strong (`keep_strong`), which is
 * what Node.js observably does. */
struct napi_ref__ {
  qn_list link; /* env->reflist, env->finalizing_reflist or env->orphan_refs */
  qn_list holder_link; /* holder->refs while holder != NULL */
  qn_list queue_link;  /* env->pending_finalizers while queued */
  napi_env env;
  qn_holder *holder;
  JSValue value;
  JSValue weak;
  uint32_t count;
  bool can_be_weak;
  bool keep_strong;
  bool empty;         /* the value is gone (Node: persistent reset) */
  bool runtime_owned; /* deleted after Finalize (ReferenceOwnership::kRuntime) */
  bool finalizing;    /* in finalizing_reflist: has a finalizer slot */
  bool tracker;       /* TrackedFinalizer: no JS value at all */
  bool queued;
  napi_finalize finalize_cb;
  void *data; /* finalize data, also the wrapped native object */
  void *hint;
};

struct napi_env__ {
  qjsnapi_host *host;
  JSContext *ctx;
  qn_list link;
  int32_t module_api_version;
  bool can_call_into_js;
  bool in_gc_finalizer;
  napi_extended_error_info last_error;
  /* Message for napi_get_last_error_info when the failing API is a stub;
   * empty string otherwise. */
  char not_impl_message[160];
  qn_arena arena;
  qn_scope *root_scope;
  uint32_t open_handle_scopes; /* user-visible scopes, root excluded */
  char *module_filename;       /* file:// URL, or NULL */
  napi_ref instance_data;      /* tracker in finalizing_reflist, or NULL */
  qn_list cleanup_hooks; /* qn_cleanup_hook.link, executed LIFO */
  /* References. Those with a finalizer slot are finalized first at
   * teardown, since their finalizers may delete the plain ones. */
  qn_list reflist;
  qn_list finalizing_reflist;
  /* Finalizers of collected targets, run at a point where JavaScript may
   * run (qjsnapi_host_run_finalizers), never from inside the GC. */
  qn_list pending_finalizers;
  /* napi_deferred not concluded yet, released at teardown. */
  qn_list deferreds;
  /* napi_async_context not destroyed yet, released at teardown. */
  qn_list async_contexts;
  /* napi_async_work queued and not completed (cancelled at teardown). */
  qn_list works;       /* napi_async_work items not deleted yet */
  qn_list orphan_refs; /* finalized user references, not deleted yet */
  uint32_t open_callback_scopes;
  bool destructing;
};

/* Native data of a function created through Node-API. */
typedef struct qn_function {
  napi_env env;
  napi_callback cb;
  void *data;
} qn_function;

/* napi_callback_info points to this stack-allocated record. */
struct napi_callback_info__ {
  JSValueConst this_arg;
  JSValueConst new_target; /* JS_UNDEFINED unless called as constructor */
  int argc;
  JSValueConst *argv;
  void *data;
};

/* ------------------------------------------------------------------------ */
/* Error state helpers and argument-check macros (mirror Node.js)           */
/* ------------------------------------------------------------------------ */

static inline napi_status qn_set_last_error(napi_env env, napi_status code) {
  env->last_error.error_code = code;
  env->last_error.engine_error_code = 0;
  env->last_error.engine_reserved = NULL;
  env->not_impl_message[0] = '\0';
  return code;
}

static inline napi_status qn_clear_last_error(napi_env env) {
  env->last_error.error_code = napi_ok;
  env->last_error.engine_error_code = 0;
  env->last_error.engine_reserved = NULL;
  env->last_error.error_message = NULL;
  env->not_impl_message[0] = '\0';
  return napi_ok;
}

/* A basic finalizer calling an engine-touching API is a fatal error, as in
 * Node.js: the engine may be in the middle of a GC sweep. */
QN_INTERNAL void qn_check_gc_access(napi_env env);

#define CHECK_ENV(env)                          \
  do {                                          \
    if ((env) == NULL) return napi_invalid_arg; \
  } while (0)

#define CHECK_ENV_NOT_IN_GC(env) \
  do {                           \
    CHECK_ENV(env);              \
    qn_check_gc_access(env);     \
  } while (0)

#define RETURN_STATUS_IF_FALSE(env, condition, status)           \
  do {                                                           \
    if (!(condition)) return qn_set_last_error((env), (status)); \
  } while (0)

#define CHECK_ARG(env, arg) \
  RETURN_STATUS_IF_FALSE((env), ((arg) != NULL), napi_invalid_arg)

/* Every API that may run JavaScript starts with this. */
#define NAPI_PREAMBLE(env)                                           \
  do {                                                               \
    CHECK_ENV_NOT_IN_GC(env);                                        \
    RETURN_STATUS_IF_FALSE((env), !JS_HasException((env)->ctx),      \
                           napi_pending_exception);                  \
    RETURN_STATUS_IF_FALSE(                                          \
        (env), (env)->can_call_into_js,                              \
        ((env)->module_api_version >= 10 ? napi_cannot_run_js        \
                                         : napi_pending_exception)); \
    qn_clear_last_error(env);                                        \
  } while (0)

/* Converts a QuickJS failure into a Node-API status. A JS exception stays
 * pending on the context, exactly like Node.js leaves it pending on V8. */
#define CHECK_JS(env, cond)                                             \
  do {                                                                  \
    if (!(cond)) {                                                      \
      return qn_set_last_error((env), JS_HasException((env)->ctx)       \
                                          ? napi_pending_exception      \
                                          : napi_generic_failure);      \
    }                                                                   \
  } while (0)

/* Like Node's RETURN_STATUS_IF_FALSE_WITH_PREAMBLE: a failure caused by a JS
 * exception reports napi_pending_exception, anything else `status`. */
#define CHECK_JS_STATUS(env, cond, status)                              \
  do {                                                                  \
    if (!(cond)) {                                                      \
      return qn_set_last_error((env), JS_HasException((env)->ctx)       \
                                          ? napi_pending_exception      \
                                          : (status));                  \
    }                                                                   \
  } while (0)

/* A handle allocation failed: QuickJS out of memory. */
#define CHECK_HANDLE(env, h) \
  RETURN_STATUS_IF_FALSE((env), ((h) != NULL), napi_generic_failure)

#define GET_RETURN_STATUS(env)                                     \
  (JS_HasException((env)->ctx)                                     \
       ? qn_set_last_error((env), napi_pending_exception)          \
       : napi_ok)

/* ------------------------------------------------------------------------ */
/* napi_value <-> JSValue                                                   */
/* ------------------------------------------------------------------------ */

static inline JSValue qn_value(napi_value v) { return *(JSValue *)(void *)v; }

/* Stores `v` in a new slot of the innermost scope, taking ownership of `v`.
 * Returns NULL (after freeing `v`) only on out-of-memory. */
QN_INTERNAL napi_value qn_new_handle(napi_env env, JSValue v);

/* Internal scopes used around every native entry point. */
QN_INTERNAL qn_scope *qn_scope_open(napi_env env, bool escapable);
QN_INTERNAL void qn_scope_close(napi_env env, qn_scope *scope);
QN_INTERNAL bool qn_arena_init(napi_env env);
QN_INTERNAL void qn_arena_destroy(napi_env env);

/* ------------------------------------------------------------------------ */
/* Cross-file internal functions                                            */
/* ------------------------------------------------------------------------ */

QN_INTERNAL int qn_function_class_init(qjsnapi_host *host);
QN_INTERNAL JSValue qn_function_new(napi_env env, const char *name,
                                    size_t name_len, napi_callback cb,
                                    void *data);

QN_INTERNAL napi_env qn_env_new(qjsnapi_host *host, int32_t module_api_version,
                                const char *module_filename_url);
QN_INTERNAL void qn_env_teardown(napi_env env);
QN_INTERNAL void qn_env_free(napi_env env);

QN_INTERNAL void qn_modules_free(qjsnapi_host *host);

/* ToObject: new reference, or JS_EXCEPTION with a TypeError pending for
 * null/undefined (V8 semantics of Value::ToObject). */
QN_INTERNAL JSValue qn_to_object(napi_env env, JSValueConst v);

/* Defines one napi_property_descriptor on `obj`. Shared by
 * napi_define_properties and napi_define_class. */
QN_INTERNAL napi_status qn_define_property(napi_env env, JSValueConst obj,
                                           const napi_property_descriptor *p);

/* UTF-8 string from (str, length) with the NAPI_AUTO_LENGTH convention;
 * the caller has validated the arguments. */
QN_INTERNAL JSValue qn_new_string_utf8(JSContext *ctx, const char *str,
                                       size_t length);

/* Validation shared by every string-creating API (CHECK_NEW_STRING_ARGS). */
#define CHECK_NEW_STRING_ARGS(env, str, length, result)                    \
  do {                                                                     \
    CHECK_ENV_NOT_IN_GC(env);                                              \
    if ((length) > 0) CHECK_ARG((env), (str));                             \
    CHECK_ARG((env), (result));                                            \
    RETURN_STATUS_IF_FALSE(                                                \
        (env), ((length) == NAPI_AUTO_LENGTH) || (length) <= INT32_MAX,    \
        napi_invalid_arg);                                                 \
  } while (0)

/* lifetime.c: classes, captured builtins and the holders WeakMap. */
QN_INTERNAL int qn_lifetime_init(qjsnapi_host *host);
/* builtins.c: binary data, BigInt, Date and Promise support. */
QN_INTERNAL int qn_builtins_init(qjsnapi_host *host);
QN_INTERNAL void qn_env_free_deferreds(napi_env env);
QN_INTERNAL int qn_buffer_init(qjsnapi_host *host);
QN_INTERNAL void qn_env_free_async_contexts(napi_env env);
/* Data and byte length of any ArrayBufferView (typed array or DataView);
 * false for anything else. */
QN_INTERNAL bool qn_view_data(napi_env env, JSValueConst value, void **data,
                              size_t *length);
/* Runs the pending promise jobs (or the embedder's checkpoint); exceptions
 * go to the uncaught handler. */
QN_INTERNAL void qn_microtask_checkpoint(qjsnapi_host *host);

/* Native code called from the event loop, with no JavaScript below it
 * (async work completion, thread-safe function calls and finalizers): a
 * handle scope and a callback scope around the call; an exception left
 * pending is reported as uncaught (Node: CallbackIntoModule), and the
 * outermost scope ends with a microtask checkpoint. */
typedef struct qn_callback_frame {
  qn_scope *scope;
  uint32_t open_handle_scopes;
} qn_callback_frame;
QN_INTERNAL void qn_callback_enter(napi_env env, qn_callback_frame *frame);
/* enforce_policy: Node's enforceUncaughtExceptionPolicy. When false and the
 * module targets Node-API < 10, an exception becomes a DEP0168 warning
 * unless the host forces the policy. */
QN_INTERNAL void qn_callback_leave(napi_env env, qn_callback_frame *frame,
                                   bool enforce_policy);
QN_INTERNAL void qn_report_callback_exception(napi_env env, JSValue exc,
                                              bool enforce_policy);

/* Event loop (loop.c). */
QN_INTERNAL int qn_loop_init(qjsnapi_host *host, uv_loop_t *loop);
QN_INTERNAL void qn_loop_schedule_finalizers(qjsnapi_host *host);
/* Teardown: runs the loop until async work, thread-safe functions and async
 * cleanup hooks are done, then (later) closes the loop. */
QN_INTERNAL void qn_loop_drain(qjsnapi_host *host);
QN_INTERNAL void qn_loop_close(qjsnapi_host *host);
QN_INTERNAL void qn_env_cancel_works(napi_env env);
/* qn_env_free: release what the addon never deleted. Node.js leaks these
 * (undeleted async work items, user references); here they are freed with
 * the env, after which no addon code can reach them. */
QN_INTERNAL void qn_env_free_works(napi_env env);
QN_INTERNAL void qn_env_free_orphan_refs(napi_env env);
QN_INTERNAL void qn_env_run_cleanup_hooks(napi_env env);
QN_INTERNAL bool qn_cleanup_hook_add(napi_env env, qn_hook_kind kind,
                                     napi_cleanup_hook fun, void *arg,
                                     qn_cleanup_hook **entry);
QN_INTERNAL void qn_call_finalizer_policy(napi_env env, napi_finalize cb,
                                          void *data, void *hint,
                                          bool enforce_policy);
/* lifetime.c: runs `finalize_cb` (queued, as for napi_add_finalizer) once
 * `obj` is collected. Returns -1 with an exception pending on failure. */
QN_INTERNAL int qn_attach_finalizer(napi_env env, JSValueConst obj,
                                    napi_finalize finalize_cb, void *data,
                                    void *hint);
QN_INTERNAL bool qn_is_external(JSValueConst v);
/* Teardown of the references of an env (Node: DrainFinalizerQueue, then
 * FinalizeAll(finalizing_reflist), then FinalizeAll(reflist)). */
QN_INTERNAL void qn_env_finalize_refs(napi_env env);
/* Runs a Node-API finalizer inside its own handle scope. An exception it
 * leaves pending is reported as uncaught (or dropped during teardown). */
QN_INTERNAL void qn_call_finalizer(napi_env env, napi_finalize cb, void *data,
                                   void *hint);
/* Reports an uncaught exception (ownership of `exc` is taken). */
QN_INTERNAL void qn_report_uncaught(qjsnapi_host *host, JSValue exc);

/* Error used when an API exists in the headers but is not implemented yet
 * (generated stubs). Sets the last error to napi_generic_failure. */
QN_INTERNAL napi_status qn_not_implemented(napi_env env, const char *name);

#endif /* QJSNAPI_INTERNAL_H */
