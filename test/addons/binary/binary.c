/*
 * Edge cases of the M3 APIs (ArrayBuffer, TypedArray, DataView, BigInt,
 * Date, Promise). Every function returns plain data; test/js/test_binary.js
 * checks it under quickjs-napi and, as the reference, under Node.js.
 *
 * A call that fails reports {status, exc}, where exc is the pending
 * exception (cleared) or undefined.
 */
#include <node_api.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define ARGS(n)                                                      \
  size_t argc = (n);                                                 \
  napi_value argv[(n) > 0 ? (n) : 1];                                \
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL)

static napi_value num(napi_env env, double d) {
  napi_value v;
  napi_create_double(env, d, &v);
  return v;
}

static napi_value boolean(napi_env env, bool b) {
  napi_value v;
  napi_get_boolean(env, b, &v);
  return v;
}

static void set(napi_env env, napi_value o, const char *k, napi_value v) {
  napi_set_named_property(env, o, k, v);
}

/* {status, exc} for a failed call. */
static napi_value failure(napi_env env, napi_status s) {
  napi_value o, exc = NULL;
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  if (pending) napi_get_and_clear_last_exception(env, &exc);
  napi_create_object(env, &o);
  set(env, o, "status", num(env, s));
  if (exc) set(env, o, "exc", exc);
  return o;
}

static int64_t to_i64(napi_env env, napi_value v) {
  int64_t x = 0;
  napi_get_value_int64(env, v, &x);
  return x;
}

/* ---- BigInt ------------------------------------------------------------ */

/* words(bigint, capacity) -> {count, sign, words: [hex...]}; capacity < 0
 * asks for the count only (sign_bit and words NULL). */
static napi_value Words(napi_env env, napi_callback_info info) {
  ARGS(2);
  int64_t cap = to_i64(env, argv[1]);
  napi_value o;
  napi_create_object(env, &o);
  if (cap < 0) {
    size_t count = 0;
    napi_status s =
        napi_get_value_bigint_words(env, argv[0], NULL, &count, NULL);
    if (s != napi_ok) return failure(env, s);
    set(env, o, "count", num(env, (double)count));
    return o;
  }
  uint64_t words[8];
  memset(words, 0xAB, sizeof words);
  size_t count = (size_t)cap;
  int sign = 7;
  napi_status s =
      napi_get_value_bigint_words(env, argv[0], &sign, &count, words);
  if (s != napi_ok) return failure(env, s);
  set(env, o, "count", num(env, (double)count));
  set(env, o, "sign", num(env, sign));
  napi_value arr;
  napi_create_array(env, &arr);
  /* All 8 slots: shows which ones were written. */
  for (uint32_t i = 0; i < 8; i++) {
    char hex[17];
    snprintf(hex, sizeof hex, "%" PRIx64, words[i]);
    napi_value h;
    napi_create_string_utf8(env, hex, NAPI_AUTO_LENGTH, &h);
    napi_set_element(env, arr, i, h);
  }
  set(env, o, "words", arr);
  return o;
}

/* fromWords(sign, [hex...]) -> bigint */
static napi_value FromWords(napi_env env, napi_callback_info info) {
  ARGS(2);
  int sign = (int)to_i64(env, argv[0]);
  uint32_t n = 0;
  napi_get_array_length(env, argv[1], &n);
  uint64_t words[8] = {0};
  for (uint32_t i = 0; i < n && i < 8; i++) {
    napi_value e;
    char hex[32];
    napi_get_element(env, argv[1], i, &e);
    napi_get_value_string_utf8(env, e, hex, sizeof hex, NULL);
    sscanf(hex, "%" SCNx64, &words[i]);
  }
  napi_value r;
  napi_status s = napi_create_bigint_words(env, sign, n, words, &r);
  return s == napi_ok ? r : failure(env, s);
}

/* tooBig(): word_count beyond any engine limit, before words are read. */
static napi_value TooBig(napi_env env, napi_callback_info info) {
  (void)info;
  uint64_t w[2] = {1, 2};
  napi_value r;
  napi_status s = napi_create_bigint_words(env, 0, 1u << 26, w, &r);
  return failure(env, s);
}

/* int64(v) / uint64(v) -> [value, lossless] or failure */
static napi_value Int64(napi_env env, napi_callback_info info) {
  ARGS(1);
  int64_t x = 0;
  bool lossless = false;
  napi_status s = napi_get_value_bigint_int64(env, argv[0], &x, &lossless);
  if (s != napi_ok) return failure(env, s);
  napi_value arr, v;
  napi_create_array(env, &arr);
  napi_create_bigint_int64(env, x, &v);
  napi_set_element(env, arr, 0, v);
  napi_set_element(env, arr, 1, boolean(env, lossless));
  return arr;
}

static napi_value Uint64(napi_env env, napi_callback_info info) {
  ARGS(1);
  uint64_t x = 0;
  bool lossless = false;
  napi_status s = napi_get_value_bigint_uint64(env, argv[0], &x, &lossless);
  if (s != napi_ok) return failure(env, s);
  napi_value arr, v;
  napi_create_array(env, &arr);
  napi_create_bigint_uint64(env, x, &v);
  napi_set_element(env, arr, 0, v);
  napi_set_element(env, arr, 1, boolean(env, lossless));
  return arr;
}

/* ---- ArrayBuffer / TypedArray / DataView ------------------------------- */

static napi_value BufferInfo(napi_env env, napi_callback_info info) {
  ARGS(1);
  void *data = (void *)1;
  size_t len = 99;
  bool is_ab = false, detached = false;
  napi_status s = napi_get_arraybuffer_info(env, argv[0], &data, &len);
  napi_is_arraybuffer(env, argv[0], &is_ab);
  napi_is_detached_arraybuffer(env, argv[0], &detached);
  napi_value o;
  napi_create_object(env, &o);
  set(env, o, "status", num(env, s));
  set(env, o, "isArrayBuffer", boolean(env, is_ab));
  set(env, o, "detached", boolean(env, detached));
  if (s == napi_ok) {
    set(env, o, "length", num(env, (double)len));
    set(env, o, "hasData", boolean(env, data != NULL));
  }
  return o;
}

static napi_value DetachStatus(napi_env env, napi_callback_info info) {
  ARGS(1);
  return num(env, napi_detach_arraybuffer(env, argv[0]));
}

static napi_value TypedInfo(napi_env env, napi_callback_info info) {
  ARGS(1);
  napi_typedarray_type type = (napi_typedarray_type)-1;
  size_t length = 99, offset = 99;
  void *data = (void *)1;
  napi_value ab;
  bool is_ta = false;
  napi_is_typedarray(env, argv[0], &is_ta);
  napi_status s =
      napi_get_typedarray_info(env, argv[0], &type, &length, &data, &ab, &offset);
  if (s != napi_ok) {
    napi_value f = failure(env, s);
    set(env, f, "isTypedArray", boolean(env, is_ta));
    return f;
  }
  void *ab_data = NULL;
  napi_get_arraybuffer_info(env, ab, &ab_data, NULL);
  napi_value o;
  napi_create_object(env, &o);
  set(env, o, "type", num(env, type));
  set(env, o, "length", num(env, (double)length));
  set(env, o, "byteOffset", num(env, (double)offset));
  set(env, o, "buffer", ab);
  set(env, o, "dataAt",
      data == NULL ? num(env, -1)
                   : num(env, (double)((char *)data - (char *)ab_data)));
  return o;
}

static napi_value CreateTyped(napi_env env, napi_callback_info info) {
  ARGS(4);
  napi_value r;
  napi_status s = napi_create_typedarray(
      env, (napi_typedarray_type)to_i64(env, argv[0]),
      (size_t)to_i64(env, argv[1]), argv[2], (size_t)to_i64(env, argv[3]),
      &r);
  return s == napi_ok ? r : failure(env, s);
}

static napi_value DataViewInfo(napi_env env, napi_callback_info info) {
  ARGS(1);
  size_t len = 99, offset = 99;
  void *data = (void *)1;
  napi_value ab;
  bool is_dv = false;
  napi_is_dataview(env, argv[0], &is_dv);
  napi_status s =
      napi_get_dataview_info(env, argv[0], &len, &data, &ab, &offset);
  if (s != napi_ok) {
    napi_value f = failure(env, s);
    set(env, f, "isDataView", boolean(env, is_dv));
    return f;
  }
  void *ab_data = NULL;
  napi_get_arraybuffer_info(env, ab, &ab_data, NULL);
  napi_value o;
  napi_create_object(env, &o);
  set(env, o, "byteLength", num(env, (double)len));
  set(env, o, "byteOffset", num(env, (double)offset));
  set(env, o, "buffer", ab);
  set(env, o, "dataAt",
      data == NULL ? num(env, -1)
                   : num(env, (double)((char *)data - (char *)ab_data)));
  return o;
}

static napi_value CreateDataView(napi_env env, napi_callback_info info) {
  ARGS(3);
  napi_value r;
  napi_status s = napi_create_dataview(env, (size_t)to_i64(env, argv[0]),
                                       argv[1], (size_t)to_i64(env, argv[2]),
                                       &r);
  return s == napi_ok ? r : failure(env, s);
}

/* An external ArrayBuffer over static memory; the finalizer counts. */
static uint8_t external_bytes[16] = {1, 2, 3, 4};
static int finalized = 0;

static void count_finalize(napi_env env, void *data, void *hint) {
  (void)env;
  (void)hint;
  if (data == external_bytes) finalized++;
}

static napi_value External(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value r;
  napi_status s = napi_create_external_arraybuffer(
      env, external_bytes, sizeof external_bytes, count_finalize, NULL, &r);
  return s == napi_ok ? r : failure(env, s);
}

static napi_value Finalized(napi_env env, napi_callback_info info) {
  (void)info;
  return num(env, finalized);
}

/* ---- Date / Promise ---------------------------------------------------- */

static napi_value CreateDate(napi_env env, napi_callback_info info) {
  ARGS(1);
  double t = 0;
  napi_get_value_double(env, argv[0], &t);
  napi_value r;
  napi_status s = napi_create_date(env, t, &r);
  return s == napi_ok ? r : failure(env, s);
}

static napi_value DateValue(napi_env env, napi_callback_info info) {
  ARGS(1);
  double t = 0;
  bool is_date = false;
  napi_is_date(env, argv[0], &is_date);
  napi_status s = napi_get_date_value(env, argv[0], &t);
  if (s != napi_ok) {
    napi_value f = failure(env, s);
    set(env, f, "isDate", boolean(env, is_date));
    return f;
  }
  return num(env, t);
}

static napi_value IsPromise(napi_env env, napi_callback_info info) {
  ARGS(1);
  bool r = false;
  napi_is_promise(env, argv[0], &r);
  return boolean(env, r);
}

/* settle(resolve?, value): a promise concluded before it is returned. */
static napi_value Settle(napi_env env, napi_callback_info info) {
  ARGS(2);
  bool resolve = false;
  napi_get_value_bool(env, argv[0], &resolve);
  napi_deferred d;
  napi_value p;
  napi_status s = napi_create_promise(env, &d, &p);
  if (s != napi_ok) return failure(env, s);
  s = resolve ? napi_resolve_deferred(env, d, argv[1])
              : napi_reject_deferred(env, d, argv[1]);
  return s == napi_ok ? p : failure(env, s);
}

/* leak(): a deferred never concluded; freed at env teardown. */
static napi_value Leak(napi_env env, napi_callback_info info) {
  (void)info;
  napi_deferred d;
  napi_value p;
  napi_create_promise(env, &d, &p);
  return p;
}

NAPI_MODULE_INIT() {
  napi_property_descriptor d[] = {
#define F(name, fn) {name, NULL, fn, NULL, NULL, NULL, napi_default, NULL}
      F("words", Words),
      F("fromWords", FromWords),
      F("tooBig", TooBig),
      F("int64", Int64),
      F("uint64", Uint64),
      F("bufferInfo", BufferInfo),
      F("detachStatus", DetachStatus),
      F("typedInfo", TypedInfo),
      F("createTyped", CreateTyped),
      F("dataViewInfo", DataViewInfo),
      F("createDataView", CreateDataView),
      F("external", External),
      F("finalized", Finalized),
      F("createDate", CreateDate),
      F("dateValue", DateValue),
      F("isPromise", IsPromise),
      F("settle", Settle),
      F("leak", Leak),
#undef F
  };
  napi_define_properties(env, exports, sizeof d / sizeof d[0], d);
  return exports;
}
