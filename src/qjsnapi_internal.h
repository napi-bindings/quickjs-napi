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

typedef struct qn_cleanup_hook {
  qn_list link;
  napi_cleanup_hook fun;
  void *arg;
} qn_cleanup_hook;

typedef struct qn_module_record {
  qn_list link;
  char *path;    /* path as given to the loader, UTF-8 */
  void *handle;  /* dlopen / LoadLibrary handle, never closed */
  napi_env env;
  JSValue exports; /* strong reference */
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
  qn_list envs;           /* napi_env__.link, in creation order */
  qn_list modules;        /* qn_module_record.link */
  bool destroying;
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
  void *instance_data;
  napi_finalize instance_data_finalize_cb;
  void *instance_data_finalize_hint;
  qn_list cleanup_hooks; /* qn_cleanup_hook.link, executed LIFO */
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

/* Error used when an API exists in the headers but is not implemented yet
 * (generated stubs). Sets the last error to napi_generic_failure. */
QN_INTERNAL napi_status qn_not_implemented(napi_env env, const char *name);

#endif /* QJSNAPI_INTERNAL_H */
