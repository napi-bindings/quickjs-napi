/*
 * Edge cases of the M1 APIs (property names, strings, coercion, errors),
 * collected into one object. test/js/test_values.js checks the result under
 * quickjs-napi and, as the reference, under Node.js.
 */
#include <node_api.h>
#include <stdio.h>
#include <string.h>
static napi_value Run(napi_env env, napi_callback_info info) {
  size_t argc = 2; napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  napi_value r;
  // keep numbers, own only, all properties
  napi_get_all_property_names(env, argv[0], napi_key_own_only, napi_key_all_properties, napi_key_keep_numbers, &r);
  napi_value out; napi_create_object(env, &out);
  napi_set_named_property(env, out, "ownKeep", r);
  napi_get_all_property_names(env, argv[0], napi_key_include_prototypes, napi_key_enumerable | napi_key_skip_symbols, napi_key_numbers_to_strings, &r);
  napi_set_named_property(env, out, "protoEnum", r);
  napi_get_all_property_names(env, argv[0], napi_key_own_only, napi_key_writable | napi_key_skip_strings, napi_key_keep_numbers, &r);
  napi_set_named_property(env, out, "writableSymbols", r);
  napi_get_all_property_names(env, argv[0], napi_key_own_only, napi_key_configurable, napi_key_keep_numbers, &r);
  napi_set_named_property(env, out, "configurable", r);
  // set on frozen
  napi_value v; napi_create_int32(env, 5, &v);
  napi_status s = napi_set_named_property(env, argv[1], "a", v);
  napi_value sv; napi_create_int32(env, s, &sv); napi_set_named_property(env, out, "setFrozenStatus", sv);
  // utf16 lone surrogate roundtrip
  char16_t lone[] = {0x61, 0xD800, 0x62, 0xDC00, 0};
  napi_value str; napi_create_string_utf16(env, lone, NAPI_AUTO_LENGTH, &str);
  napi_set_named_property(env, out, "lone", str);
  char buf[32]; size_t n; napi_get_value_string_utf8(env, str, buf, sizeof buf, &n);
  napi_value b; napi_create_string_latin1(env, buf, n, &b); // raw bytes as latin1 to inspect
  napi_set_named_property(env, out, "loneUtf8Bytes", b);
  napi_get_value_string_utf8(env, str, NULL, 0, &n);
  napi_value nn; napi_create_uint32(env, (uint32_t)n, &nn); napi_set_named_property(env, out, "loneUtf8Len", nn);
  // utf8 truncation at char boundary: "aé€😀" with bufsize 5
  napi_value s2; napi_create_string_utf8(env, "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", NAPI_AUTO_LENGTH, &s2);
  napi_get_value_string_utf8(env, s2, buf, 5, &n);
  napi_value t; napi_create_uint32(env, (uint32_t)n, &t); napi_set_named_property(env, out, "trunc5", t);
  napi_get_value_string_utf8(env, s2, buf, 7, &n);
  napi_create_uint32(env, (uint32_t)n, &t); napi_set_named_property(env, out, "trunc7", t);
  // utf16 of emoji with bufsize 2 -> 1 unit
  char16_t u[4]; napi_get_value_string_utf16(env, s2, u, 4, &n);
  napi_create_uint32(env, (uint32_t)n, &t); napi_set_named_property(env, out, "utf16n4", t);
  napi_value u3; napi_create_string_utf16(env, u, n, &u3); napi_set_named_property(env, out, "utf16cut", u3);
  // latin1 of non-latin1 chars: low byte
  napi_get_value_string_latin1(env, s2, buf, sizeof buf, &n);
  napi_create_string_latin1(env, buf, n, &b); napi_set_named_property(env, out, "latin1", b);
  // coerce undefined to object
  napi_value undef, o2; napi_get_undefined(env, &undef);
  s = napi_coerce_to_object(env, undef, &o2);
  bool pending; napi_is_exception_pending(env, &pending);
  napi_value exc; napi_get_and_clear_last_exception(env, &exc);
  napi_create_int32(env, s, &sv); napi_set_named_property(env, out, "coerceUndefStatus", sv);
  napi_get_boolean(env, pending, &sv); napi_set_named_property(env, out, "coerceUndefPending", sv);
  // instanceof with non function
  napi_value obj; napi_create_object(env, &obj);
  bool inst; s = napi_instanceof(env, obj, obj, &inst);
  napi_is_exception_pending(env, &pending); napi_get_and_clear_last_exception(env, &exc);
  napi_create_int32(env, s, &sv); napi_set_named_property(env, out, "instanceofStatus", sv);
  napi_set_named_property(env, out, "instanceofError", exc);
  // define_properties conflict on frozen
  napi_property_descriptor d = {"zz", NULL, NULL, NULL, NULL, v, napi_default, NULL};
  s = napi_define_properties(env, argv[1], 1, &d);
  napi_is_exception_pending(env, &pending); if (pending) napi_get_and_clear_last_exception(env, &exc);
  napi_create_int32(env, s, &sv); napi_set_named_property(env, out, "defineFrozenStatus", sv);
  // has_own_property with number key
  napi_value num; napi_create_int32(env, 1, &num);
  bool has; s = napi_has_own_property(env, obj, num, &has);
  napi_create_int32(env, s, &sv); napi_set_named_property(env, out, "hasOwnNumStatus", sv);
  // array with length
  napi_value arr; napi_create_array_with_length(env, 7, &arr); napi_set_named_property(env, out, "arr7", arr);
  // get_value_uint32 of -1, int32 of 2^32+5, double
  napi_value m1; napi_create_double(env, -1, &m1); uint32_t u32; napi_get_value_uint32(env, m1, &u32);
  napi_create_double(env, (double)u32, &t); napi_set_named_property(env, out, "u32m1", t);
  napi_create_double(env, 4294967301.7, &m1); int32_t i32; napi_get_value_int32(env, m1, &i32);
  napi_create_int32(env, i32, &t); napi_set_named_property(env, out, "i32big", t);
  return out;
}
NAPI_MODULE_INIT() { napi_value fn; napi_create_function(env, "run", NAPI_AUTO_LENGTH, Run, NULL, &fn); return fn; }
