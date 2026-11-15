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

#ifdef __cplusplus
}
#endif

#endif /* QJSNAPI_H */
