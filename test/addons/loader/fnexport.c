/*
 * The initializer returns a function instead of the exports object: the
 * returned value becomes the module exports (module.exports in Node.js).
 */
#include <node_api.h>

static napi_value Answer(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value result;
  if (napi_create_int32(env, 42, &result) != napi_ok) return NULL;
  return result;
}

NAPI_MODULE_INIT() {
  (void)exports;
  napi_value fn;
  if (napi_create_function(env, "answer", NAPI_AUTO_LENGTH, Answer, NULL,
                           &fn) != napi_ok)
    return NULL;
  return fn;
}
