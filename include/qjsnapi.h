/*
 * quickjs-napi - embedding API.
 *
 * Node-API implementation for QuickJS. An embedder creates one host per
 * JSContext, loads `.node` addons through it, and destroys the host right
 * before JS_FreeContext():
 *
 *     JSRuntime *rt = JS_NewRuntime();
 *     JSContext *ctx = JS_NewContext(rt);
 *     qjsnapi_host *host = qjsnapi_host_create(ctx);
 *     JSValue exports;
 *     if (qjsnapi_load_addon(host, "addon.node", &exports) < 0) {
 *       // a JS exception is pending on ctx
 *     }
 *     ...
 *     JS_FreeValue(ctx, exports);
 *     qjsnapi_host_destroy(host);  // runs cleanup hooks and finalizers
 *     JS_FreeContext(ctx);
 *     JS_FreeRuntime(rt);
 */
#ifndef QJSNAPI_H
#define QJSNAPI_H

#include <stdint.h>

#include "quickjs.h"
#include "node_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qjsnapi_host qjsnapi_host;

/* Creates the Node-API host for `ctx` and stores it as the context opaque
 * pointer. Returns NULL on failure (a JS exception may be pending). */
qjsnapi_host *qjsnapi_host_create(JSContext *ctx);

/* Returns the host attached to `ctx`, or NULL. */
qjsnapi_host *qjsnapi_host_from_context(JSContext *ctx);

/* Tears down every env (cleanup hooks LIFO, instance data finalizers),
 * releases the module cache and detaches the host from the context. Must be
 * called before JS_FreeContext(). No JavaScript may run afterwards. */
void qjsnapi_host_destroy(qjsnapi_host *host);

/* Loads a `.node` shared library and runs its Node-API initializer.
 * On success returns 0 and stores a new reference to the exports object in
 * *exports. On failure returns -1 with a JS exception pending on the
 * context. Loading the same path twice returns the cached exports. */
int qjsnapi_load_addon(qjsnapi_host *host, const char *path, JSValue *exports);

/* Runs the initializer of an addon linked statically into the embedder, as
 * if it had been loaded from `name`. `module_api_version` follows the
 * node_api_module_get_api_version_v1 semantics (0 selects the default, 8). */
int qjsnapi_init_static_addon(qjsnapi_host *host, const char *name,
                              napi_addon_register_func init,
                              int32_t module_api_version, JSValue *exports);

/* Highest Node-API version supported by this build. */
uint32_t qjsnapi_napi_version(void);

/* ---- Finalizers -----------------------------------------------------------
 *
 * As in Node.js, the finalizers of objects collected by the garbage collector
 * (napi_wrap, napi_add_finalizer, napi_create_external) are not run inside
 * the GC: they are queued and run later, at a point where JavaScript may run
 * (Node.js uses a SetImmediate callback). The embedder's event loop calls
 * qjsnapi_host_run_finalizers() once per iteration, outside of any JS call.
 * Finalizers still queued when the host is destroyed run then, with
 * JavaScript disabled (napi_cannot_run_js). */

/* Runs the queued finalizers of every env. Returns how many ran. */
size_t qjsnapi_host_run_finalizers(qjsnapi_host *host);

/* Non-zero when finalizers are queued: the event loop must not exit yet. */
int qjsnapi_host_has_pending_finalizers(qjsnapi_host *host);

/* Called with an exception that no JavaScript frame can catch: thrown by a
 * finalizer. Node.js triggers process 'uncaughtException' (or exits). The
 * handler takes ownership of `exception`. Without a handler the exception is
 * printed on stderr and dropped. */
typedef void (*qjsnapi_uncaught_fn)(qjsnapi_host *host, JSContext *ctx,
                                    JSValue exception, void *opaque);
void qjsnapi_host_set_uncaught_handler(qjsnapi_host *host,
                                       qjsnapi_uncaught_fn fn, void *opaque);

#ifdef __cplusplus
}
#endif

#endif /* QJSNAPI_H */
