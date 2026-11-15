/*
 * Legacy registration: napi_module_register() called from a static
 * constructor that runs while the library is being loaded. No
 * napi_register_module_v1 symbol is exported.
 */
#include <node_api.h>

static napi_value Init(napi_env env, napi_value exports) {
  napi_value kind;
  if (napi_create_string_utf8(env, "legacy", NAPI_AUTO_LENGTH, &kind) !=
      napi_ok)
    return NULL;
  if (napi_set_named_property(env, exports, "kind", kind) != napi_ok)
    return NULL;
  return exports;
}

static napi_module legacy_module = {
    NAPI_MODULE_VERSION, 0, __FILE__, Init, "legacy", NULL, {0},
};

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
static void __cdecl register_legacy(void);
__declspec(allocate(".CRT$XCU")) void(__cdecl *register_legacy_)(void) =
    register_legacy;
static void __cdecl register_legacy(void) {
  napi_module_register(&legacy_module);
}
#else
__attribute__((constructor)) static void register_legacy(void) {
  napi_module_register(&legacy_module);
}
#endif
