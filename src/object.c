/*
 * Objects and properties.
 *
 * Semantics follow Node.js (src/js_native_api_v8.cc):
 *   - the receiver goes through ToObject (CHECK_TO_OBJECT): primitives are
 *     wrapped, null/undefined throw a TypeError and give napi_object_expected
 *     with the exception pending;
 *   - napi_set_* are sloppy-mode [[Set]]: assigning a read-only property
 *     fails silently, as v8::Object::Set does;
 *   - napi_define_properties uses complete property descriptors.
 *
 * Every JSValue/JSAtom acquired in a function is released on every path.
 */
#include <stdlib.h>
#include <string.h>

#include "qjsnapi_internal.h"

JSValue qn_to_object(napi_env env, JSValueConst v) {
  JSContext *ctx = env->ctx;
  if (JS_IsObject(v)) return JS_DupValue(ctx, v);
  if (JS_IsNull(v) || JS_IsUndefined(v))
    return JS_ThrowTypeError(ctx, "Cannot convert undefined or null to object");
  /* Object(primitive) returns the wrapper object. */
  return JS_Call(ctx, env->host->object_ctor, JS_UNDEFINED, 1, &v);
}

/* CHECK_TO_OBJECT: `out` receives a new reference to be freed by the caller. */
#define CHECK_TO_OBJECT(env, out, src)                                         \
  do {                                                                         \
    CHECK_ARG((env), (src));                                                   \
    (out) = qn_to_object((env), qn_value(src));                                \
    RETURN_STATUS_IF_FALSE((env), !JS_IsException(out), napi_object_expected); \
  } while (0)

static bool is_name(JSValueConst v) { return JS_IsString(v) || JS_IsSymbol(v); }

/* ------------------------------------------------------------------------ */
/* Keyed access                                                             */
/* ------------------------------------------------------------------------ */

enum key_kind { KEY_VALUE, KEY_UTF8, KEY_INDEX };

typedef struct key_ref {
  enum key_kind kind;
  napi_value value;
  const char *utf8;
  uint32_t index;
} key_ref;

/* Atom for a key; JS_ATOM_NULL with an exception pending on failure
 * (ToPropertyKey may call user code). */
static JSAtom key_atom(napi_env env, const key_ref *k) {
  switch (k->kind) {
    case KEY_UTF8:
      return JS_NewAtom(env->ctx, k->utf8);
    case KEY_INDEX:
      return JS_NewAtomUInt32(env->ctx, k->index);
    default:
      return JS_ValueToAtom(env->ctx, qn_value(k->value));
  }
}

static napi_status do_set(napi_env env, napi_value object, const key_ref *k,
                          napi_value value) {
  JSContext *ctx = env->ctx;
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  int r = -1;
  JSAtom atom = key_atom(env, k);
  if (atom != JS_ATOM_NULL) {
    /* flags = 0: sloppy-mode [[Set]], no TypeError on read-only targets. */
    r = JS_SetPropertyInternal(ctx, obj, atom,
                               JS_DupValue(ctx, qn_value(value)), obj, 0);
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, obj);
  CHECK_JS_STATUS(env, r >= 0, napi_generic_failure);
  return GET_RETURN_STATUS(env);
}

static napi_status do_get(napi_env env, napi_value object, const key_ref *k,
                          napi_value *result) {
  JSContext *ctx = env->ctx;
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  JSValue v = JS_EXCEPTION;
  JSAtom atom = key_atom(env, k);
  if (atom != JS_ATOM_NULL) {
    v = JS_GetProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, obj);
  CHECK_JS_STATUS(env, !JS_IsException(v), napi_generic_failure);
  napi_value h = qn_new_handle(env, v);
  CHECK_HANDLE(env, h);
  *result = h;
  return GET_RETURN_STATUS(env);
}

static napi_status do_has(napi_env env, JSValueConst obj, const key_ref *k,
                          bool own, bool *result) {
  JSContext *ctx = env->ctx;
  int r = -1;
  JSAtom atom = key_atom(env, k);
  if (atom != JS_ATOM_NULL) {
    r = own ? JS_GetOwnProperty(ctx, NULL, obj, atom)
            : JS_HasProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
  }
  CHECK_JS_STATUS(env, r >= 0, napi_generic_failure);
  *result = r != 0;
  return GET_RETURN_STATUS(env);
}

static napi_status has_keyed(napi_env env, napi_value object, const key_ref *k,
                             bool *result) {
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  napi_status st = do_has(env, obj, k, false, result);
  JS_FreeValue(env->ctx, obj);
  return st;
}

static napi_status do_delete(napi_env env, napi_value object, const key_ref *k,
                             bool *result) {
  JSContext *ctx = env->ctx;
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  int r = -1;
  JSAtom atom = key_atom(env, k);
  if (atom != JS_ATOM_NULL) {
    /* flags = 0: false (not an exception) for non-configurable properties,
     * like v8::Object::Delete. */
    r = JS_DeleteProperty(ctx, obj, atom, 0);
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, obj);
  CHECK_JS_STATUS(env, r >= 0, napi_generic_failure);
  if (result) *result = r != 0;
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_property(napi_env env,
                                                     napi_value object,
                                                     napi_value key,
                                                     napi_value value) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, key);
  CHECK_ARG(env, value);
  key_ref k = {KEY_VALUE, key, NULL, 0};
  return do_set(env, object, &k, value);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_property(napi_env env,
                                                     napi_value object,
                                                     napi_value key,
                                                     bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, key);
  key_ref k = {KEY_VALUE, key, NULL, 0};
  return has_keyed(env, object, &k, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property(napi_env env,
                                                     napi_value object,
                                                     napi_value key,
                                                     napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, key);
  CHECK_ARG(env, result);
  key_ref k = {KEY_VALUE, key, NULL, 0};
  return do_get(env, object, &k, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_property(napi_env env,
                                                        napi_value object,
                                                        napi_value key,
                                                        bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, key);
  key_ref k = {KEY_VALUE, key, NULL, 0};
  return do_delete(env, object, &k, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_own_property(napi_env env,
                                                         napi_value object,
                                                         napi_value key,
                                                         bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, key);
  CHECK_ARG(env, result);
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  if (!is_name(qn_value(key))) {
    JS_FreeValue(env->ctx, obj);
    return qn_set_last_error(env, napi_name_expected);
  }
  key_ref k = {KEY_VALUE, key, NULL, 0};
  napi_status st = do_has(env, obj, &k, true, result);
  JS_FreeValue(env->ctx, obj);
  return st;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_named_property(napi_env env,
                                                           napi_value object,
                                                           const char *utf8name,
                                                           napi_value value) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, object);
  CHECK_ARG(env, utf8name);
  key_ref k = {KEY_UTF8, NULL, utf8name, 0};
  return do_set(env, object, &k, value);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_named_property(napi_env env,
                                                           napi_value object,
                                                           const char *utf8name,
                                                           bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, object);
  CHECK_ARG(env, utf8name);
  key_ref k = {KEY_UTF8, NULL, utf8name, 0};
  return has_keyed(env, object, &k, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_named_property(napi_env env,
                                                           napi_value object,
                                                           const char *utf8name,
                                                           napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  CHECK_ARG(env, utf8name);
  key_ref k = {KEY_UTF8, NULL, utf8name, 0};
  return do_get(env, object, &k, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_element(napi_env env,
                                                    napi_value object,
                                                    uint32_t index,
                                                    napi_value value) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, value);
  key_ref k = {KEY_INDEX, NULL, NULL, index};
  return do_set(env, object, &k, value);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_has_element(napi_env env,
                                                    napi_value object,
                                                    uint32_t index,
                                                    bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  key_ref k = {KEY_INDEX, NULL, NULL, index};
  return has_keyed(env, object, &k, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_element(napi_env env,
                                                    napi_value object,
                                                    uint32_t index,
                                                    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  key_ref k = {KEY_INDEX, NULL, NULL, index};
  return do_get(env, object, &k, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_element(napi_env env,
                                                       napi_value object,
                                                       uint32_t index,
                                                       bool *result) {
  NAPI_PREAMBLE(env);
  key_ref k = {KEY_INDEX, NULL, NULL, index};
  return do_delete(env, object, &k, result);
}

/* ------------------------------------------------------------------------ */
/* Property definition                                                      */
/* ------------------------------------------------------------------------ */

napi_status qn_define_property(napi_env env, JSValueConst obj,
                               const napi_property_descriptor *p) {
  JSContext *ctx = env->ctx;
  JSAtom atom;
  if (p->utf8name != NULL) {
    atom = JS_NewAtom(ctx, p->utf8name);
  } else {
    CHECK_ARG(env, p->name);
    RETURN_STATUS_IF_FALSE(env, is_name(qn_value(p->name)), napi_name_expected);
    atom = JS_ValueToAtom(ctx, qn_value(p->name));
  }
  CHECK_JS(env, atom != JS_ATOM_NULL);

  int flags = JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE;
  if (p->attributes & napi_enumerable) flags |= JS_PROP_ENUMERABLE;
  if (p->attributes & napi_configurable) flags |= JS_PROP_CONFIGURABLE;

  napi_status fail_status;
  int r;
  if (p->getter != NULL || p->setter != NULL) {
    /* Accessor: napi_writable is ignored, a missing half is undefined. */
    JSValue getter = JS_UNDEFINED, setter = JS_UNDEFINED;
    if (p->getter) getter = qn_function_new(env, NULL, 0, p->getter, p->data);
    if (p->setter && !JS_IsException(getter))
      setter = qn_function_new(env, NULL, 0, p->setter, p->data);
    if (JS_IsException(getter) || JS_IsException(setter)) {
      if (!JS_IsException(getter)) JS_FreeValue(ctx, getter);
      JS_FreeAtom(ctx, atom);
      return qn_set_last_error(env, napi_pending_exception);
    }
    flags |= JS_PROP_HAS_GET | JS_PROP_HAS_SET;
    r = JS_DefineProperty(ctx, obj, atom, JS_UNDEFINED, getter, setter, flags);
    JS_FreeValue(ctx, getter);
    JS_FreeValue(ctx, setter);
    fail_status = napi_invalid_arg;
  } else {
    JSValue value;
    if (p->method != NULL) {
      value = qn_function_new(env, NULL, 0, p->method, p->data);
      if (JS_IsException(value)) {
        JS_FreeAtom(ctx, atom);
        return qn_set_last_error(env, napi_pending_exception);
      }
      fail_status = napi_generic_failure;
    } else {
      value = p->value ? JS_DupValue(ctx, qn_value(p->value)) : JS_UNDEFINED;
      fail_status = napi_invalid_arg;
    }
    flags |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
    if (p->attributes & napi_writable) flags |= JS_PROP_WRITABLE;
    r = JS_DefineProperty(ctx, obj, atom, value, JS_UNDEFINED, JS_UNDEFINED,
                          flags);
    JS_FreeValue(ctx, value);
  }
  JS_FreeAtom(ctx, atom);
  /* r == 0: the definition was rejected (non-configurable conflict,
   * non-extensible object) without an exception, like V8 returning false. */
  CHECK_JS_STATUS(env, r > 0, fail_status);
  return napi_ok;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_define_properties(napi_env env, napi_value object, size_t property_count,
                       const napi_property_descriptor *properties) {
  NAPI_PREAMBLE(env);
  if (property_count > 0) CHECK_ARG(env, properties);
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  for (size_t i = 0; i < property_count; i++) {
    napi_status st = qn_define_property(env, obj, &properties[i]);
    if (st != napi_ok) {
      JS_FreeValue(env->ctx, obj);
      return st;
    }
  }
  JS_FreeValue(env->ctx, obj);
  return GET_RETURN_STATUS(env);
}

/* ------------------------------------------------------------------------ */
/* Property names                                                           */
/* ------------------------------------------------------------------------ */

/* Canonical array index ("0", "17", never "017"), < 2^32 - 1. */
static bool parse_array_index(const char *s, size_t len, uint32_t *out) {
  if (len == 0 || len > 10) return false;
  if (len > 1 && s[0] == '0') return false;
  uint64_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    n = n * 10 + (uint64_t)(s[i] - '0');
  }
  if (n >= 4294967295u) return false;
  *out = (uint32_t)n;
  return true;
}

typedef struct atom_set {
  JSAtom *atoms;
  size_t len, cap;
} atom_set;

static bool atom_set_has(const atom_set *s, JSAtom a) {
  for (size_t i = 0; i < s->len; i++)
    if (s->atoms[i] == a) return true;
  return false;
}

static int atom_set_add(JSContext *ctx, atom_set *s, JSAtom a) {
  if (s->len == s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 16;
    JSAtom *n = (JSAtom *)realloc(s->atoms, cap * sizeof(JSAtom));
    if (!n) return -1;
    s->atoms = n;
    s->cap = cap;
  }
  s->atoms[s->len++] = JS_DupAtom(ctx, a);
  return 0;
}

static void atom_set_free(JSContext *ctx, atom_set *s) {
  for (size_t i = 0; i < s->len; i++) JS_FreeAtom(ctx, s->atoms[i]);
  free(s->atoms);
}

/* Converts a key to its JS value: symbols and strings as is, array indices
 * as numbers when `keep_numbers`. */
static JSValue key_value(JSContext *ctx, JSAtom atom, bool keep_numbers) {
  JSValue v = JS_AtomToValue(ctx, atom);
  if (!keep_numbers || !JS_IsString(v)) return v;
  size_t len;
  const char *s = JS_ToCStringLen(ctx, &len, v);
  if (!s) {
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
  }
  uint32_t idx;
  bool is_index = parse_array_index(s, len, &idx);
  JS_FreeCString(ctx, s);
  if (!is_index) return v;
  JS_FreeValue(ctx, v);
  return JS_NewUint32(ctx, idx);
}

/* V8 Object::GetPropertyNames: own keys (and, with kIncludePrototypes, the
 * keys of the prototype chain not shadowed by an earlier object), in
 * OwnPropertyKeys order, filtered by attributes and key type. */
static JSValue collect_names(napi_env env, JSValueConst object, bool own_only,
                             napi_key_filter filter, bool keep_numbers) {
  JSContext *ctx = env->ctx;
  JSValue result = JS_NewArray(ctx);
  if (JS_IsException(result)) return result;
  atom_set seen = {NULL, 0, 0};
  uint32_t out_len = 0;
  JSValue cur = JS_DupValue(ctx, object);
  const bool need_desc =
      (filter & (napi_key_writable | napi_key_configurable)) != 0;

  while (JS_IsObject(cur)) {
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, cur,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK |
                                   JS_GPN_SET_ENUM) < 0)
      goto fail;
    for (uint32_t i = 0; i < len; i++) {
      JSAtom atom = tab[i].atom;
      if (!own_only) {
        if (atom_set_has(&seen, atom)) continue; /* shadowed */
        if (atom_set_add(ctx, &seen, atom) < 0) {
          JS_FreePropertyEnum(ctx, tab, len);
          JS_ThrowOutOfMemory(ctx);
          goto fail;
        }
      }
      if ((filter & napi_key_enumerable) && !tab[i].is_enumerable) continue;
      if (need_desc) {
        JSPropertyDescriptor desc;
        int r = JS_GetOwnProperty(ctx, &desc, cur, atom);
        if (r < 0) {
          JS_FreePropertyEnum(ctx, tab, len);
          goto fail;
        }
        if (r == 0) continue; /* deleted by a getter or proxy trap */
        JS_FreeValue(ctx, desc.value);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
        bool is_accessor = (desc.flags & JS_PROP_GETSET) != 0;
        if ((filter & napi_key_writable) &&
            (is_accessor || !(desc.flags & JS_PROP_WRITABLE)))
          continue;
        if ((filter & napi_key_configurable) &&
            !(desc.flags & JS_PROP_CONFIGURABLE))
          continue;
      }
      JSValue key = key_value(ctx, atom, keep_numbers);
      if (JS_IsException(key)) {
        JS_FreePropertyEnum(ctx, tab, len);
        goto fail;
      }
      bool sym = JS_IsSymbol(key);
      if ((sym && (filter & napi_key_skip_symbols)) ||
          (!sym && (filter & napi_key_skip_strings))) {
        JS_FreeValue(ctx, key);
        continue;
      }
      if (JS_SetPropertyUint32(ctx, result, out_len++, key) < 0) {
        JS_FreePropertyEnum(ctx, tab, len);
        goto fail;
      }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (own_only) break;
    JSValue proto = JS_GetPrototype(ctx, cur);
    JS_FreeValue(ctx, cur);
    cur = proto;
    if (JS_IsException(cur)) goto fail;
  }
  JS_FreeValue(ctx, cur);
  atom_set_free(ctx, &seen);
  return result;
fail:
  if (!JS_IsException(cur)) JS_FreeValue(ctx, cur);
  atom_set_free(ctx, &seen);
  JS_FreeValue(ctx, result);
  return JS_EXCEPTION;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_all_property_names(napi_env env, napi_value object,
                            napi_key_collection_mode key_mode,
                            napi_key_filter key_filter,
                            napi_key_conversion key_conversion,
                            napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  if ((key_mode != napi_key_include_prototypes &&
       key_mode != napi_key_own_only) ||
      (key_conversion != napi_key_keep_numbers &&
       key_conversion != napi_key_numbers_to_strings)) {
    JS_FreeValue(env->ctx, obj);
    return qn_set_last_error(env, napi_invalid_arg);
  }
  JSValue names =
      collect_names(env, obj, key_mode == napi_key_own_only, key_filter,
                    key_conversion == napi_key_keep_numbers);
  JS_FreeValue(env->ctx, obj);
  CHECK_JS_STATUS(env, !JS_IsException(names), napi_generic_failure);
  napi_value h = qn_new_handle(env, names);
  CHECK_HANDLE(env, h);
  *result = h;
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_property_names(napi_env env,
                                                           napi_value object,
                                                           napi_value *result) {
  return napi_get_all_property_names(
      env, object, napi_key_include_prototypes,
      (napi_key_filter)(napi_key_enumerable | napi_key_skip_symbols),
      napi_key_numbers_to_strings, result);
}

/* ------------------------------------------------------------------------ */
/* Prototype, integrity, instanceof                                         */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_get_prototype(napi_env env,
                                                      napi_value object,
                                                      napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  JSValue proto = JS_GetPrototype(env->ctx, obj);
  JS_FreeValue(env->ctx, obj);
  CHECK_JS_STATUS(env, !JS_IsException(proto), napi_generic_failure);
  napi_value h = qn_new_handle(env, proto);
  CHECK_HANDLE(env, h);
  *result = h;
  return GET_RETURN_STATUS(env);
}

static napi_status integrity(napi_env env, napi_value object, bool freeze) {
  NAPI_PREAMBLE(env);
  JSValue obj;
  CHECK_TO_OBJECT(env, obj, object);
  JSValueConst fn =
      freeze ? env->host->object_freeze : env->host->object_seal;
  JSValue r = JS_Call(env->ctx, fn, JS_UNDEFINED, 1, &obj);
  JS_FreeValue(env->ctx, obj);
  CHECK_JS_STATUS(env, !JS_IsException(r), napi_generic_failure);
  JS_FreeValue(env->ctx, r);
  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_object_freeze(napi_env env,
                                                      napi_value object) {
  return integrity(env, object, true);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_object_seal(napi_env env,
                                                    napi_value object) {
  return integrity(env, object, false);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_instanceof(napi_env env,
                                                   napi_value object,
                                                   napi_value constructor,
                                                   bool *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, object);
  CHECK_ARG(env, result);
  *result = false;
  JSValue ctor;
  CHECK_TO_OBJECT(env, ctor, constructor);
  if (!JS_IsFunction(env->ctx, ctor)) {
    JS_FreeValue(env->ctx, ctor);
    napi_throw_type_error(env, "ERR_NAPI_CONS_FUNCTION",
                          "Constructor must be a function");
    return qn_set_last_error(env, napi_function_expected);
  }
  int r = JS_IsInstanceOf(env->ctx, qn_value(object), ctor);
  JS_FreeValue(env->ctx, ctor);
  /* Node.js: CHECK_MAYBE_NOTHING(env, maybe, napi_generic_failure). */
  RETURN_STATUS_IF_FALSE(env, r >= 0, napi_generic_failure);
  *result = r != 0;
  return GET_RETURN_STATUS(env);
}
