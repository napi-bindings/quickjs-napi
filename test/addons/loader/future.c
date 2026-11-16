/*
 * Declares a Node-API version newer than the runtime supports: loading must
 * fail with ERR_DLOPEN_FAILED before the initializer runs.
 */
#include <node_api.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

EXPORT int32_t node_api_module_get_api_version_v1(void) { return 9999; }

EXPORT napi_value napi_register_module_v1(napi_env env, napi_value exports) {
  napi_throw_error(env, NULL, "the initializer must not run");
  return exports;
}
