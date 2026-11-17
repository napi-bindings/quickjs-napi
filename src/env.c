/*
 * Host / env lifecycle, last-error bookkeeping, instance data, cleanup
 * hooks, version information and fatal errors.
 *
 * Teardown order (same as Node.js):
 *   for every env, newest first:
 *     1. can_call_into_js = false
 *     2. env cleanup hooks, LIFO
 *     3. queued finalizers, then every reference with a finalizer slot
 *        (instance data included), then every plain reference, newest
 *        first (lifetime.c, qn_env_finalize_refs)
 *   module cache released (exports objects)
 *   every env arena released (all napi_value slots)
 *   captured builtins released, host detached from the context
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qjsnapi_internal.h"

#ifndef QJSNAPI_NODE_VERSION_MAJOR
#define QJSNAPI_NODE_VERSION_MAJOR 22
#define QJSNAPI_NODE_VERSION_MINOR 14
#define QJSNAPI_NODE_VERSION_PATCH 0
#endif

/* Indexed by napi_status, same wording as Node.js. */
static const char *const qn_error_messages[] = {
    NULL,
    "Invalid argument",
    "An object was expected",
    "A string was expected",
    "A string or symbol was expected",
    "A function was expected",
    "A number was expected",
    "A boolean was expected",
    "An array was expected",
    "Unknown failure",
    "An exception is pending",
    "The async work item was cancelled",
    "napi_escape_handle already called on scope",
    "Invalid handle scope usage",
    "Invalid callback scope usage",
    "Thread-safe function queue is full",
    "Thread-safe function handle is closing",
    "A bigint was expected",
    "A date was expected",
    "An arraybuffer was expected",
    "A detachable arraybuffer was expected",
    "Main thread would deadlock",
    "External buffers are not allowed",
    "Cannot run JavaScript",
};

_Static_assert(sizeof(qn_error_messages) / sizeof(qn_error_messages[0]) ==
                   napi_cannot_run_js + 1,
               "qn_error_messages must cover every napi_status");

/* ------------------------------------------------------------------------ */
/* Internal helpers                                                         */
/* ------------------------------------------------------------------------ */

void qn_check_gc_access(napi_env env) {
  if (env->in_gc_finalizer) {
    napi_fatal_error(
        "qn_check_gc_access", NAPI_AUTO_LENGTH,
        "Finalizer is calling a function that may affect GC state.\n"
        "The finalizers are run directly from GC and must not affect GC "
        "state.\nUse `node_api_post_finalizer` from inside of the finalizer "
        "to work around this issue.",
        NAPI_AUTO_LENGTH);
  }
}

napi_status qn_not_implemented(napi_env env, const char *name) {
  if (getenv("QJSNAPI_TRACE_STUBS"))
    fprintf(stderr, "quickjs-napi: %s is not implemented yet\n", name);
  if (!env) return napi_generic_failure;
  qn_set_last_error(env, napi_generic_failure);
  snprintf(env->not_impl_message, sizeof(env->not_impl_message),
           "%s is not implemented yet in quickjs-napi", name);
  return napi_generic_failure;
}

static char *qn_strdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *d = (char *)malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

napi_env qn_env_new(qjsnapi_host *host, int32_t module_api_version,
                    const char *module_filename_url) {
  napi_env env = (napi_env)calloc(1, sizeof(struct napi_env__));
  if (!env) return NULL;
  env->host = host;
  env->ctx = host->ctx;
  env->module_api_version = module_api_version > 0
                                ? module_api_version
                                : QN_DEFAULT_MODULE_API_VERSION;
  env->can_call_into_js = true;
  qn_list_init(&env->cleanup_hooks);
  qn_list_init(&env->reflist);
  qn_list_init(&env->finalizing_reflist);
  qn_list_init(&env->pending_finalizers);
  qn_list_init(&env->deferreds);
  if (module_filename_url) {
    env->module_filename = qn_strdup(module_filename_url);
    if (!env->module_filename) goto fail;
  }
  if (!qn_arena_init(env)) goto fail;
  /* Root scope: values created outside any callback (which should not
   * happen with a well-behaved host) live until the env is destroyed. */
  env->root_scope = qn_scope_open(env, false);
  if (!env->root_scope) goto fail;
  qn_list_add_tail(&env->link, &host->envs);
  return env;
fail:
  qn_arena_destroy(env);
  free(env->module_filename);
  free(env);
  return NULL;
}

void qn_env_teardown(napi_env env) {
  env->can_call_into_js = false;
  /* Cleanup hooks, last registered first. */
  while (!qn_list_empty(&env->cleanup_hooks)) {
    qn_cleanup_hook *h =
        QN_CONTAINER_OF(env->cleanup_hooks.prev, qn_cleanup_hook, link);
    qn_list_del(&h->link);
    napi_cleanup_hook fun = h->fun;
    void *arg = h->arg;
    free(h);
    fun(arg);
  }
  qn_env_finalize_refs(env);
  qn_env_free_deferreds(env);
}

void qn_env_free(napi_env env) {
  qn_list_del(&env->link);
  qn_arena_destroy(env);
  free(env->module_filename);
  free(env);
}

/* ------------------------------------------------------------------------ */
/* Host                                                                     */
/* ------------------------------------------------------------------------ */

static JSValue qn_noop(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
  (void)ctx;
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_UNDEFINED;
}

/* Every JSValue the host keeps, so that creation failure and destruction
 * release exactly the same set. */
static JSValue *host_values(qjsnapi_host *host, size_t *count) {
  /* Fields are contiguous in qjsnapi_host, from function_proto to
   * holders (checked below). */
  *count = (size_t)(&host->bigint_to_hex - &host->function_proto) + 1;
  return &host->function_proto;
}

_Static_assert(offsetof(qjsnapi_host, bigint_to_hex) -
                       offsetof(qjsnapi_host, function_proto) ==
                   (1 + QN_ERROR_KIND_COUNT + 19) * sizeof(JSValue),
               "host JSValue fields must be contiguous");

static void host_free_values(qjsnapi_host *host) {
  size_t n;
  JSValue *v = host_values(host, &n);
  for (size_t i = 0; i < n; i++) {
    JS_FreeValue(host->ctx, v[i]);
    v[i] = JS_UNDEFINED;
  }
}

/* globalThis[name] (or globalThis[name][member]) must be a function. */
static int capture_builtin(JSContext *ctx, JSValueConst global,
                           const char *name, const char *member,
                           JSValue *out) {
  JSValue v = JS_GetPropertyStr(ctx, global, name);
  if (!JS_IsException(v) && member) {
    JSValue m = JS_GetPropertyStr(ctx, v, member);
    JS_FreeValue(ctx, v);
    v = m;
  }
  if (JS_IsException(v)) return -1;
  if (!JS_IsFunction(ctx, v)) {
    JS_FreeValue(ctx, v);
    JS_ThrowInternalError(ctx, "quickjs-napi: %s%s%s is not available", name,
                          member ? "." : "", member ? member : "");
    return -1;
  }
  *out = v;
  return 0;
}

qjsnapi_host *qjsnapi_host_create(JSContext *ctx) {
  if (JS_GetContextOpaque(ctx) != NULL) {
    JS_ThrowInternalError(ctx,
                          "quickjs-napi: the context opaque pointer is "
                          "already in use");
    return NULL;
  }
  qjsnapi_host *host = (qjsnapi_host *)calloc(1, sizeof(qjsnapi_host));
  if (!host) {
    JS_ThrowOutOfMemory(ctx);
    return NULL;
  }
  host->rt = JS_GetRuntime(ctx);
  host->ctx = ctx;
  size_t nvalues;
  JSValue *values = host_values(host, &nvalues);
  for (size_t i = 0; i < nvalues; i++) values[i] = JS_UNDEFINED;
  qn_list_init(&host->envs);
  qn_list_init(&host->modules);

  /* %Function.prototype% taken from a fresh function: immune to a script
   * that replaced globalThis.Function. */
  JSValue probe = JS_NewCFunction(ctx, qn_noop, "", 0);
  if (JS_IsException(probe)) goto fail;
  JSValue proto = JS_GetPrototype(ctx, probe);
  JS_FreeValue(ctx, probe);
  if (JS_IsException(proto)) goto fail;
  host->function_proto = proto;

  /* Builtins captured before any user script runs. Errors built by calling
   * the constructors carry a stack trace, unlike JS_NewError(). */
  static const char *const error_names[QN_ERROR_KIND_COUNT] = {
      "Error", "TypeError", "RangeError", "SyntaxError"};
  JSValue global = JS_GetGlobalObject(ctx);
  int r = 0;
  for (int i = 0; i < QN_ERROR_KIND_COUNT && r == 0; i++)
    r = capture_builtin(ctx, global, error_names[i], NULL,
                        &host->error_ctors[i]);
  if (r == 0) r = capture_builtin(ctx, global, "Object", NULL, &host->object_ctor);
  if (r == 0)
    r = capture_builtin(ctx, global, "Object", "freeze", &host->object_freeze);
  if (r == 0)
    r = capture_builtin(ctx, global, "Object", "seal", &host->object_seal);
  if (r == 0) r = capture_builtin(ctx, global, "Symbol", NULL, &host->symbol_ctor);
  if (r == 0) r = capture_builtin(ctx, global, "Symbol", "for", &host->symbol_for);
  JS_FreeValue(ctx, global);
  if (r < 0) goto fail;

  if (qn_function_class_init(host) < 0) goto fail;
  if (qn_lifetime_init(host) < 0) goto fail;
  if (qn_builtins_init(host) < 0) goto fail;

  JS_SetContextOpaque(ctx, host);
  return host;
fail:
  host_free_values(host);
  free(host);
  return NULL;
}

qjsnapi_host *qjsnapi_host_from_context(JSContext *ctx) {
  return (qjsnapi_host *)JS_GetContextOpaque(ctx);
}

void qjsnapi_host_destroy(qjsnapi_host *host) {
  if (!host) return;
  host->destroying = true;
  /* Newest env first. */
  for (qn_list *el = host->envs.prev; el != &host->envs; el = el->prev) {
    qn_env_teardown(QN_CONTAINER_OF(el, struct napi_env__, link));
  }
  qn_modules_free(host);
  while (!qn_list_empty(&host->envs)) {
    qn_env_free(QN_CONTAINER_OF(host->envs.prev, struct napi_env__, link));
  }
  host_free_values(host);
  if (JS_GetContextOpaque(host->ctx) == host)
    JS_SetContextOpaque(host->ctx, NULL);
  free(host);
}

uint32_t qjsnapi_napi_version(void) { return QN_NAPI_VERSION; }

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_get_last_error_info(
    node_api_basic_env env, const napi_extended_error_info **result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  napi_status code = env->last_error.error_code;
  if ((int)code < 0 || code > napi_cannot_run_js) code = napi_generic_failure;
  env->last_error.error_message = env->not_impl_message[0]
                                      ? env->not_impl_message
                                      : qn_error_messages[code];
  if (env->last_error.error_code == napi_ok) qn_clear_last_error(env);
  *result = &env->last_error;
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_version(node_api_basic_env env,
                                                    uint32_t *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = QN_NAPI_VERSION;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_node_version(
    node_api_basic_env env, const napi_node_version **version) {
  static const napi_node_version node_version = {
      QJSNAPI_NODE_VERSION_MAJOR, QJSNAPI_NODE_VERSION_MINOR,
      QJSNAPI_NODE_VERSION_PATCH, "quickjs-napi"};
  CHECK_ENV(env);
  CHECK_ARG(env, version);
  *version = &node_version;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_add_env_cleanup_hook(
    node_api_basic_env env, napi_cleanup_hook fun, void *arg) {
  CHECK_ENV(env);
  CHECK_ARG(env, fun);
  for (qn_list *el = env->cleanup_hooks.next; el != &env->cleanup_hooks;
       el = el->next) {
    qn_cleanup_hook *h = QN_CONTAINER_OF(el, qn_cleanup_hook, link);
    if (h->fun == fun && h->arg == arg) {
      /* Node.js aborts on duplicate registration (CHECK failure). */
      napi_fatal_error("napi_add_env_cleanup_hook", NAPI_AUTO_LENGTH,
                       "cleanup hook registered twice with the same argument",
                       NAPI_AUTO_LENGTH);
    }
  }
  qn_cleanup_hook *h = (qn_cleanup_hook *)malloc(sizeof(qn_cleanup_hook));
  RETURN_STATUS_IF_FALSE(env, h != NULL, napi_generic_failure);
  h->fun = fun;
  h->arg = arg;
  qn_list_add_tail(&h->link, &env->cleanup_hooks);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_remove_env_cleanup_hook(
    node_api_basic_env env, napi_cleanup_hook fun, void *arg) {
  CHECK_ENV(env);
  CHECK_ARG(env, fun);
  for (qn_list *el = env->cleanup_hooks.next; el != &env->cleanup_hooks;
       el = el->next) {
    qn_cleanup_hook *h = QN_CONTAINER_OF(el, qn_cleanup_hook, link);
    if (h->fun == fun && h->arg == arg) {
      qn_list_del(&h->link);
      free(h);
      break;
    }
  }
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_get_module_file_name(node_api_basic_env env, const char **result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);
  *result = env->module_filename;
  return qn_clear_last_error(env);
}

NAPI_EXTERN NAPI_NO_RETURN void NAPI_CDECL
napi_fatal_error(const char *location, size_t location_len,
                 const char *message, size_t message_len) {
  if (!location) location_len = 0;
  else if (location_len == NAPI_AUTO_LENGTH) location_len = strlen(location);
  if (!message) message_len = 0;
  else if (message_len == NAPI_AUTO_LENGTH) message_len = strlen(message);
  fprintf(stderr, "FATAL ERROR: %.*s %.*s\n", (int)location_len,
          location ? location : "", (int)message_len, message ? message : "");
  fflush(stderr);
  abort();
}
