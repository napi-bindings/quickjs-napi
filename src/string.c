/*
 * Strings, property keys and symbols.
 *
 * QuickJS has no public UTF-16 API, but its UTF-8 entry points are lossless
 * at the code-unit level:
 *   - JS_NewStringLen accepts surrogates encoded on 3 bytes (WTF-8), so a
 *     UTF-16 buffer with lone surrogates round-trips exactly;
 *   - JS_ToCStringLen2(..., cesu8 = 1) encodes every UTF-16 code unit
 *     separately (CESU-8), so the code units can be recovered one by one.
 * Getters follow V8's semantics used by Node.js: lengths in code units for
 * latin1/utf16, never a partial UTF-8 sequence, lone surrogates replaced by
 * U+FFFD in UTF-8 output, results always NUL-terminated.
 */
#include <stdlib.h>
#include <string.h>

#include "qjsnapi_internal.h"

/* ------------------------------------------------------------------------ */
/* Encoders                                                                 */
/* ------------------------------------------------------------------------ */

static size_t put_utf8(char *q, uint32_t c) {
  if (c < 0x80) {
    q[0] = (char)c;
    return 1;
  } else if (c < 0x800) {
    q[0] = (char)(0xC0 | (c >> 6));
    q[1] = (char)(0x80 | (c & 0x3F));
    return 2;
  } else if (c < 0x10000) {
    q[0] = (char)(0xE0 | (c >> 12));
    q[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    q[2] = (char)(0x80 | (c & 0x3F));
    return 3;
  }
  q[0] = (char)(0xF0 | (c >> 18));
  q[1] = (char)(0x80 | ((c >> 12) & 0x3F));
  q[2] = (char)(0x80 | ((c >> 6) & 0x3F));
  q[3] = (char)(0x80 | (c & 0x3F));
  return 4;
}

static bool is_hi_surrogate(uint32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
static bool is_lo_surrogate(uint32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

/* UTF-16 -> WTF-8: pairs become 4-byte sequences, lone surrogates 3-byte
 * sequences that QuickJS stores back as the same code unit. */
static char *utf16_to_wtf8(const uint16_t *s, size_t n, size_t *out_len) {
  char *buf = (char *)malloc(n * 3 + 1);
  if (!buf) return NULL;
  char *q = buf;
  for (size_t i = 0; i < n; i++) {
    uint32_t c = s[i];
    if (is_hi_surrogate(c) && i + 1 < n && is_lo_surrogate(s[i + 1])) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00u);
      i++;
    }
    q += put_utf8(q, c);
  }
  *out_len = (size_t)(q - buf);
  return buf;
}

static char *latin1_to_utf8(const uint8_t *s, size_t n, size_t *out_len) {
  char *buf = (char *)malloc(n * 2 + 1);
  if (!buf) return NULL;
  char *q = buf;
  for (size_t i = 0; i < n; i++) q += put_utf8(q, s[i]);
  *out_len = (size_t)(q - buf);
  return buf;
}

/* Length in bytes of the UTF-8 / CESU-8 sequence starting with `b`. */
static size_t seq_len(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  return 4;
}

/* Decodes one CESU-8 sequence (1 to 3 bytes) into a UTF-16 code unit. */
static uint16_t cesu8_unit(const uint8_t *p, size_t l) {
  switch (l) {
    case 1:
      return p[0];
    case 2:
      return (uint16_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
    default:
      return (uint16_t)(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) |
                        (p[2] & 0x3F));
  }
}

/* ------------------------------------------------------------------------ */
/* Creation                                                                 */
/* ------------------------------------------------------------------------ */

JSValue qn_new_string_utf8(JSContext *ctx, const char *str, size_t length) {
  if (length == NAPI_AUTO_LENGTH) length = strlen(str);
  return JS_NewStringLen(ctx, length ? str : "", length);
}

static napi_status return_value(napi_env env, JSValue v, napi_value *result) {
  CHECK_JS(env, !JS_IsException(v));
  napi_value h = qn_new_handle(env, v);
  CHECK_HANDLE(env, h);
  *result = h;
  return qn_clear_last_error(env);
}

static napi_status new_utf8(napi_env env, const char *str, size_t length,
                            napi_value *result) {
  CHECK_NEW_STRING_ARGS(env, str, length, result);
  return return_value(env, qn_new_string_utf8(env->ctx, str, length), result);
}

static napi_status new_latin1(napi_env env, const char *str, size_t length,
                              napi_value *result) {
  CHECK_NEW_STRING_ARGS(env, str, length, result);
  if (length == NAPI_AUTO_LENGTH) length = strlen(str);
  size_t len8;
  char *utf8 = latin1_to_utf8((const uint8_t *)str, length, &len8);
  RETURN_STATUS_IF_FALSE(env, utf8 != NULL, napi_generic_failure);
  JSValue v = JS_NewStringLen(env->ctx, utf8, len8);
  free(utf8);
  return return_value(env, v, result);
}

static napi_status new_utf16(napi_env env, const char16_t *str, size_t length,
                             napi_value *result) {
  CHECK_NEW_STRING_ARGS(env, str, length, result);
  if (length == NAPI_AUTO_LENGTH) {
    length = 0;
    while (str[length]) length++;
  }
  size_t len8;
  char *wtf8 = utf16_to_wtf8((const uint16_t *)str, length, &len8);
  RETURN_STATUS_IF_FALSE(env, wtf8 != NULL, napi_generic_failure);
  JSValue v = JS_NewStringLen(env->ctx, wtf8, len8);
  free(wtf8);
  return return_value(env, v, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_latin1(
    napi_env env, const char *str, size_t length, napi_value *result) {
  return new_latin1(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf8(napi_env env,
                                                           const char *str,
                                                           size_t length,
                                                           napi_value *result) {
  return new_utf8(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf16(
    napi_env env, const char16_t *str, size_t length, napi_value *result) {
  return new_utf16(env, str, length, result);
}

/* QuickJS cannot reference external memory from a string: the content is
 * always copied, *copied is set and the finalizer runs immediately, exactly
 * what Node.js does when V8 runs with the pointer-compression sandbox. */
NAPI_EXTERN napi_status NAPI_CDECL node_api_create_external_string_latin1(
    napi_env env, char *str, size_t length,
    node_api_basic_finalize finalize_callback, void *finalize_hint,
    napi_value *result, bool *copied) {
  napi_status s = new_latin1(env, str, length, result);
  if (s == napi_ok) {
    if (copied) *copied = true;
    if (finalize_callback) finalize_callback(env, str, finalize_hint);
  }
  return s;
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_external_string_utf16(
    napi_env env, char16_t *str, size_t length,
    node_api_basic_finalize finalize_callback, void *finalize_hint,
    napi_value *result, bool *copied) {
  napi_status s = new_utf16(env, str, length, result);
  if (s == napi_ok) {
    if (copied) *copied = true;
    if (finalize_callback) finalize_callback(env, str, finalize_hint);
  }
  return s;
}

/* Property keys: V8 internalizes them; QuickJS atomizes strings when they are
 * used as keys, so a plain string is equivalent. */
NAPI_EXTERN napi_status NAPI_CDECL node_api_create_property_key_latin1(
    napi_env env, const char *str, size_t length, napi_value *result) {
  return new_latin1(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_property_key_utf8(
    napi_env env, const char *str, size_t length, napi_value *result) {
  return new_utf8(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_property_key_utf16(
    napi_env env, const char16_t *str, size_t length, napi_value *result) {
  return new_utf16(env, str, length, result);
}

/* ------------------------------------------------------------------------ */
/* Extraction                                                               */
/* ------------------------------------------------------------------------ */

/* Common prologue of the getters: `value` must be a string. Returns the
 * CESU-8 (cesu8 = true) or WTF-8 (cesu8 = false) form, to be released with
 * JS_FreeCString. */
static napi_status get_encoded(napi_env env, napi_value value, bool cesu8,
                               const char **out, size_t *out_len) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, value);
  JSValue v = qn_value(value);
  RETURN_STATUS_IF_FALSE(env, JS_IsString(v), napi_string_expected);
  const char *s = JS_ToCStringLen2(env->ctx, out_len, v, cesu8);
  CHECK_JS(env, s != NULL);
  *out = s;
  return napi_ok;
}

static size_t count_units(const char *s, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len; i += seq_len((uint8_t)s[i])) n++;
  return n;
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_latin1(
    napi_env env, napi_value value, char *buf, size_t bufsize,
    size_t *result) {
  const char *s;
  size_t len;
  napi_status st = get_encoded(env, value, true, &s, &len);
  if (st != napi_ok) return st;
  if (!buf) {
    if (!result) {
      JS_FreeCString(env->ctx, s);
      return qn_set_last_error(env, napi_invalid_arg);
    }
    *result = count_units(s, len);
  } else if (bufsize != 0) {
    /* Each code unit truncated to its low byte (V8 WriteOneByte). */
    size_t copied = 0;
    for (size_t i = 0; i < len && copied < bufsize - 1;) {
      size_t l = seq_len((uint8_t)s[i]);
      buf[copied++] = (char)(cesu8_unit((const uint8_t *)s + i, l) & 0xFF);
      i += l;
    }
    buf[copied] = '\0';
    if (result) *result = copied;
  } else if (result) {
    *result = 0;
  }
  JS_FreeCString(env->ctx, s);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf8(
    napi_env env, napi_value value, char *buf, size_t bufsize,
    size_t *result) {
  const char *s;
  size_t len;
  /* WTF-8: valid pairs are combined, lone surrogates stay 3-byte sequences
   * (ED A0..BF xx), replaced below by U+FFFD (EF BF BD), same length. */
  napi_status st = get_encoded(env, value, false, &s, &len);
  if (st != napi_ok) return st;
  if (!buf) {
    if (!result) {
      JS_FreeCString(env->ctx, s);
      return qn_set_last_error(env, napi_invalid_arg);
    }
    *result = len;
  } else if (bufsize != 0) {
    size_t copied = 0;
    for (size_t i = 0; i < len;) {
      size_t l = seq_len((uint8_t)s[i]);
      if (copied + l > bufsize - 1) break; /* never split a character */
      if (l == 3 && (uint8_t)s[i] == 0xED && (uint8_t)s[i + 1] >= 0xA0) {
        buf[copied] = (char)0xEF;
        buf[copied + 1] = (char)0xBF;
        buf[copied + 2] = (char)0xBD;
      } else {
        memcpy(buf + copied, s + i, l);
      }
      copied += l;
      i += l;
    }
    buf[copied] = '\0';
    if (result) *result = copied;
  } else if (result) {
    *result = 0;
  }
  JS_FreeCString(env->ctx, s);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf16(
    napi_env env, napi_value value, char16_t *buf, size_t bufsize,
    size_t *result) {
  const char *s;
  size_t len;
  napi_status st = get_encoded(env, value, true, &s, &len);
  if (st != napi_ok) return st;
  if (!buf) {
    if (!result) {
      JS_FreeCString(env->ctx, s);
      return qn_set_last_error(env, napi_invalid_arg);
    }
    *result = count_units(s, len);
  } else if (bufsize != 0) {
    size_t copied = 0;
    for (size_t i = 0; i < len && copied < bufsize - 1;) {
      size_t l = seq_len((uint8_t)s[i]);
      buf[copied++] = cesu8_unit((const uint8_t *)s + i, l);
      i += l;
    }
    buf[copied] = 0;
    if (result) *result = copied;
  } else if (result) {
    *result = 0;
  }
  JS_FreeCString(env->ctx, s);
  return qn_clear_last_error(env);
}

/* ------------------------------------------------------------------------ */
/* Symbols                                                                  */
/* ------------------------------------------------------------------------ */

NAPI_EXTERN napi_status NAPI_CDECL napi_create_symbol(napi_env env,
                                                      napi_value description,
                                                      napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  JSValue sym;
  if (description == NULL) {
    sym = JS_Call(env->ctx, env->host->symbol_ctor, JS_UNDEFINED, 0, NULL);
  } else {
    JSValue desc = qn_value(description);
    RETURN_STATUS_IF_FALSE(env, JS_IsString(desc), napi_string_expected);
    sym = JS_Call(env->ctx, env->host->symbol_ctor, JS_UNDEFINED, 1, &desc);
  }
  return return_value(env, sym, result);
}

NAPI_EXTERN napi_status NAPI_CDECL node_api_symbol_for(
    napi_env env, const char *utf8description, size_t length,
    napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  napi_value desc;
  napi_status st = new_utf8(env, utf8description, length, &desc);
  if (st != napi_ok) return st;
  JSValue d = qn_value(desc);
  JSValue sym = JS_Call(env->ctx, env->host->symbol_for, JS_UNDEFINED, 1, &d);
  return return_value(env, sym, result);
}
