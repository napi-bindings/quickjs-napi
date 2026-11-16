/*
 * Test addon for milestone M0. Built exactly like a node-gyp addon: it does
 * NOT link against quickjs-napi and resolves napi_* from the host process,
 * so the same binary also loads in Node.js (see test/js/test_hello.js).
 */
#include <node_api.h>
#include <stdbool.h>
#include <stddef.h>

/* Throws the last error as a JS exception unless one is already pending. */
#define NAPI_CALL(env, call)                                \
  do {                                                      \
    napi_status status_ = (call);                           \
    if (status_ != napi_ok) {                               \
      const napi_extended_error_info *info_ = NULL;         \
      bool pending_ = false;                                \
      napi_get_last_error_info((env), &info_);              \
      napi_is_exception_pending((env), &pending_);          \
      if (!pending_) {                                      \
        napi_throw_error((env), NULL,                       \
                         (info_ && info_->error_message)    \
                             ? info_->error_message         \
                             : "empty error message");      \
      }                                                     \
      return NULL;                                          \
    }                                                       \
  } while (0)

static napi_value Hello(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value result;
  NAPI_CALL(env, napi_create_string_utf8(env, "world", NAPI_AUTO_LENGTH,
                                         &result));
  return result;
}

static napi_value Echo(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  return argv[0]; /* undefined when called without arguments */
}

static napi_value ArgCount(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  NAPI_CALL(env, napi_get_cb_info(env, info, &argc, NULL, NULL, NULL));
  napi_value result;
  NAPI_CALL(env, napi_create_uint32(env, (uint32_t)argc, &result));
  return result;
}

static napi_value ThisArg(napi_env env, napi_callback_info info) {
  napi_value self;
  NAPI_CALL(env, napi_get_cb_info(env, info, NULL, NULL, &self, NULL));
  return self;
}

static napi_value Data(napi_env env, napi_callback_info info) {
  void *data;
  NAPI_CALL(env, napi_get_cb_info(env, info, NULL, NULL, NULL, &data));
  napi_value result;
  NAPI_CALL(env, napi_create_string_utf8(env, (const char *)data,
                                         NAPI_AUTO_LENGTH, &result));
  return result;
}

/* Constructor: `new Ctor()` sets this.created and returns undefined, so the
 * receiver built from new.target.prototype is the result. */
static napi_value Ctor(napi_env env, napi_callback_info info) {
  napi_value self, new_target, flag;
  NAPI_CALL(env, napi_get_cb_info(env, info, NULL, NULL, &self, NULL));
  NAPI_CALL(env, napi_get_new_target(env, info, &new_target));
  NAPI_CALL(env, napi_get_boolean(env, new_target != NULL, &flag));
  NAPI_CALL(env, napi_set_named_property(env, self, "created", flag));
  return NULL;
}

static napi_value IsConstructCall(napi_env env, napi_callback_info info) {
  napi_value new_target, result;
  NAPI_CALL(env, napi_get_new_target(env, info, &new_target));
  NAPI_CALL(env, napi_get_boolean(env, new_target != NULL, &result));
  return result;
}

static napi_value ThrowError(napi_env env, napi_callback_info info) {
  (void)info;
  napi_throw_error(env, "ERR_HELLO", "boom");
  return NULL;
}

static napi_value ThrowTypeError(napi_env env, napi_callback_info info) {
  (void)info;
  napi_throw_type_error(env, NULL, "bad type");
  return NULL;
}

static napi_value ThrowRangeError(napi_env env, napi_callback_info info) {
  (void)info;
  napi_throw_range_error(env, "ERR_RANGE", "out of range");
  return NULL;
}

static napi_value ThrowSyntaxError(napi_env env, napi_callback_info info) {
  (void)info;
  node_api_throw_syntax_error(env, NULL, "bad syntax");
  return NULL;
}

static napi_value ThrowValue(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  NAPI_CALL(env, napi_throw(env, argv[0]));
  return NULL;
}

static napi_value CreateError(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value code, msg, err;
  NAPI_CALL(env, napi_create_string_utf8(env, "ERR_CREATED", NAPI_AUTO_LENGTH,
                                         &code));
  NAPI_CALL(env, napi_create_string_utf8(env, "created", NAPI_AUTO_LENGTH,
                                         &msg));
  NAPI_CALL(env, napi_create_error(env, code, msg, &err));
  return err;
}

/* Throws, then catches the exception from native code: the function must
 * return normally with the caught error. */
static napi_value ThrowAndCatch(napi_env env, napi_callback_info info) {
  (void)info;
  bool pending = false;
  napi_value err, obj, result, flag;
  NAPI_CALL(env, napi_create_object(env, &obj));
  NAPI_CALL(env, napi_throw_error(env, NULL, "caught"));
  napi_is_exception_pending(env, &pending);
  if (!pending) {
    napi_throw_error(env, NULL, "exception should be pending");
    return NULL;
  }
  /* An API that may run JS refuses to start while an exception is pending;
   * on mismatch the pending exception propagates and fails the test. */
  if (napi_set_named_property(env, obj, "x", obj) != napi_pending_exception)
    return NULL;
  napi_get_and_clear_last_exception(env, &err);
  napi_is_exception_pending(env, &pending);
  NAPI_CALL(env, napi_create_object(env, &result));
  NAPI_CALL(env, napi_set_named_property(env, result, "error", err));
  NAPI_CALL(env, napi_get_boolean(env, pending, &flag));
  NAPI_CALL(env, napi_set_named_property(env, result, "stillPending", flag));
  return result;
}

static napi_value LastErrorInfo(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value str, result, code, msg;
  napi_status status = napi_create_string_utf8(env, NULL, 3, &str);
  const napi_extended_error_info *err;
  NAPI_CALL(env, napi_get_last_error_info(env, &err));
  if (err->error_code != status) {
    napi_throw_error(env, NULL, "error_code does not match the status");
    return NULL;
  }
  /* Copy before any further call overwrites the last error. */
  const char *message = err->error_message;
  NAPI_CALL(env, napi_create_int32(env, (int32_t)status, &code));
  NAPI_CALL(env, napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &msg));
  NAPI_CALL(env, napi_create_object(env, &result));
  NAPI_CALL(env, napi_set_named_property(env, result, "status", code));
  NAPI_CALL(env, napi_set_named_property(env, result, "message", msg));
  return result;
}

/* Handle scopes: 1000 values in a scope (crosses arena chunks), an
 * escapable scope whose escaped value outlives it, and the
 * escape-called-twice error. */
static napi_value Scopes(napi_env env, napi_callback_info info) {
  (void)info;
  napi_handle_scope scope;
  NAPI_CALL(env, napi_open_handle_scope(env, &scope));
  for (int i = 0; i < 1000; i++) {
    napi_value tmp;
    NAPI_CALL(env, napi_create_object(env, &tmp));
  }
  NAPI_CALL(env, napi_close_handle_scope(env, scope));

  napi_escapable_handle_scope esc;
  napi_value inner, escaped, again;
  NAPI_CALL(env, napi_open_escapable_handle_scope(env, &esc));
  NAPI_CALL(env, napi_create_string_utf8(env, "escaped", NAPI_AUTO_LENGTH,
                                         &inner));
  NAPI_CALL(env, napi_escape_handle(env, esc, inner, &escaped));
  if (napi_escape_handle(env, esc, inner, &again) !=
      napi_escape_called_twice) {
    napi_throw_error(env, NULL, "second escape must fail");
    return NULL;
  }
  NAPI_CALL(env, napi_close_escapable_handle_scope(env, esc));
  return escaped;
}

static napi_value Version(napi_env env, napi_callback_info info) {
  (void)info;
  uint32_t version;
  napi_value result;
  NAPI_CALL(env, napi_get_version(env, &version));
  NAPI_CALL(env, napi_create_uint32(env, version, &result));
  return result;
}

static napi_value ModuleFileName(napi_env env, napi_callback_info info) {
  (void)info;
  const char *name;
  napi_value result;
  NAPI_CALL(env, node_api_get_module_file_name(env, &name));
  NAPI_CALL(env, napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &result));
  return result;
}

static napi_value TypeOf(napi_env env, napi_callback_info info) {
  static const char *const names[] = {
      "undefined", "null",   "boolean",  "number",   "string",
      "symbol",    "object", "function", "external", "bigint"};
  size_t argc = 1;
  napi_value argv[1], result;
  napi_valuetype type;
  NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  NAPI_CALL(env, napi_typeof(env, argv[0], &type));
  NAPI_CALL(env, napi_create_string_utf8(env, names[type], NAPI_AUTO_LENGTH,
                                         &result));
  return result;
}

static napi_status export_function(napi_env env, napi_value exports,
                                   const char *name, napi_callback cb,
                                   void *data) {
  napi_value fn;
  napi_status s =
      napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, data, &fn);
  if (s != napi_ok) return s;
  return napi_set_named_property(env, exports, name, fn);
}

NAPI_MODULE_INIT() {
  static char data[] = "callback data";
  static const struct {
    const char *name;
    napi_callback cb;
  } fns[] = {
      {"hello", Hello},
      {"echo", Echo},
      {"argCount", ArgCount},
      {"thisArg", ThisArg},
      {"Ctor", Ctor},
      {"isConstructCall", IsConstructCall},
      {"throwError", ThrowError},
      {"throwTypeError", ThrowTypeError},
      {"throwRangeError", ThrowRangeError},
      {"throwSyntaxError", ThrowSyntaxError},
      {"throwValue", ThrowValue},
      {"createError", CreateError},
      {"throwAndCatch", ThrowAndCatch},
      {"lastErrorInfo", LastErrorInfo},
      {"scopes", Scopes},
      {"version", Version},
      {"moduleFileName", ModuleFileName},
      {"typeOf", TypeOf},
  };
  for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
    if (export_function(env, exports, fns[i].name, fns[i].cb, NULL) !=
        napi_ok)
      return NULL;
  }
  if (export_function(env, exports, "data", Data, data) != napi_ok)
    return NULL;
  return exports;
}
