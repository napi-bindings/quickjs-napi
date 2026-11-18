# Values

[Node-API reference](README.md) › Values

Implemented in `src/value.c`. Semantics follow `src/js_native_api_v8.cc` of
Node.js: **getters never coerce** (a value of the wrong type gives
`napi_*_expected`), while the `napi_coerce_*` functions apply the ECMAScript
conversions and may run JavaScript.

| Group | Functions |
|---|---|
| Singletons | [`napi_get_undefined`](#napi_get_undefined), [`napi_get_null`](#napi_get_null), [`napi_get_global`](#napi_get_global), [`napi_get_boolean`](#napi_get_boolean) |
| Creation | [`napi_create_object`](#napi_create_object), [`napi_create_array`](#napi_create_array), [`napi_create_array_with_length`](#napi_create_array_with_length), [`napi_create_double`](#napi_create_double), [`napi_create_int32`](#napi_create_int32), [`napi_create_uint32`](#napi_create_uint32), [`napi_create_int64`](#napi_create_int64) |
| Type and getters | [`napi_typeof`](#napi_typeof), [`napi_get_value_double`](#napi_get_value_double), [`napi_get_value_int32`](#napi_get_value_int32), [`napi_get_value_uint32`](#napi_get_value_uint32), [`napi_get_value_int64`](#napi_get_value_int64), [`napi_get_value_bool`](#napi_get_value_bool) |
| Coercion | [`napi_coerce_to_bool`](#napi_coerce_to_bool), [`napi_coerce_to_number`](#napi_coerce_to_number), [`napi_coerce_to_object`](#napi_coerce_to_object), [`napi_coerce_to_string`](#napi_coerce_to_string) |
| Arrays and comparison | [`napi_is_array`](#napi_is_array), [`napi_get_array_length`](#napi_get_array_length), [`napi_strict_equals`](#napi_strict_equals) |

All the creation and getter functions work while an exception is pending.

---

## Singletons

### `napi_get_undefined`

```c
napi_status napi_get_undefined(napi_env env, napi_value *result);
```

Stores `undefined`.

### `napi_get_null`

```c
napi_status napi_get_null(napi_env env, napi_value *result);
```

Stores `null`.

### `napi_get_global`

```c
napi_status napi_get_global(napi_env env, napi_value *result);
```

Stores the global object of the context (`globalThis`).

### `napi_get_boolean`

```c
napi_status napi_get_boolean(napi_env env, bool value, napi_value *result);
```

Stores `true` or `false`.

## Creation

### `napi_create_object`

```c
napi_status napi_create_object(napi_env env, napi_value *result);
```

Creates an empty ordinary object (`{}`) whose prototype is
`Object.prototype`.

### `napi_create_array`

```c
napi_status napi_create_array(napi_env env, napi_value *result);
```

Creates an empty array (`[]`).

### `napi_create_array_with_length`

```c
napi_status napi_create_array_with_length(napi_env env, size_t length,
                                          napi_value *result);
```

Creates an array whose `length` is `length`, with no elements (holes), like
`new Array(length)`. As with V8's `Array::New(int)`, a length above
`INT32_MAX` gives an empty array instead of failing.

### `napi_create_double`

```c
napi_status napi_create_double(napi_env env, double value, napi_value *result);
```

Creates a Number from a double (including `NaN`, `±Infinity`, `-0`).

### `napi_create_int32`

```c
napi_status napi_create_int32(napi_env env, int32_t value, napi_value *result);
```

Creates a Number from a signed 32-bit integer.

### `napi_create_uint32`

```c
napi_status napi_create_uint32(napi_env env, uint32_t value,
                               napi_value *result);
```

Creates a Number from an unsigned 32-bit integer.

### `napi_create_int64`

```c
napi_status napi_create_int64(napi_env env, int64_t value, napi_value *result);
```

Creates a **Number** (not a BigInt) from a signed 64-bit integer. Values
beyond ±2^53 lose precision, as in V8. Use
[`napi_create_bigint_int64`](binary-data.md#napi_create_bigint_int64) for exact
64-bit values.

## Type and getters

### `napi_typeof`

```c
napi_status napi_typeof(napi_env env, napi_value value,
                        napi_valuetype *result);
```

Stores the type of `value`, like the `typeof` operator except that:

- `null` is `napi_null` (not `napi_object`);
- values created by [`napi_create_external`](lifetime.md#napi_create_external)
  are `napi_external` (JavaScript sees `typeof` `'object'`).

Possible results: `napi_undefined`, `napi_null`, `napi_boolean`,
`napi_number`, `napi_bigint`, `napi_string`, `napi_symbol`, `napi_object`,
`napi_function`, `napi_external`.

### `napi_get_value_double`

```c
napi_status napi_get_value_double(napi_env env, napi_value value,
                                  double *result);
```

Stores the value of a Number. `napi_number_expected` for anything else
(strings, BigInts and `Number` objects included).

### `napi_get_value_int32`

```c
napi_status napi_get_value_int32(napi_env env, napi_value value,
                                 int32_t *result);
```

Stores the ECMAScript `ToInt32` of a Number: truncation towards zero, then
modulo 2^32 into the signed range. `NaN` and `±Infinity` give `0`.
`napi_number_expected` when `value` is not a Number.

### `napi_get_value_uint32`

```c
napi_status napi_get_value_uint32(napi_env env, napi_value value,
                                  uint32_t *result);
```

Stores the ECMAScript `ToUint32` of a Number (same bit pattern as
`ToInt32`; `-1` gives `4294967295`). `NaN` and `±Infinity` give `0`.

### `napi_get_value_int64`

```c
napi_status napi_get_value_int64(napi_env env, napi_value value,
                                 int64_t *result);
```

Stores the integer value of a Number, like V8's `Value::IntegerValue`:
truncation towards zero, **saturating** at `INT64_MIN` / `INT64_MAX` outside
the 64-bit range. `NaN` and `±Infinity` give `0`, as in Node.js.

### `napi_get_value_bool`

```c
napi_status napi_get_value_bool(napi_env env, napi_value value, bool *result);
```

Stores the value of a Boolean primitive. `napi_boolean_expected` for anything
else (truthy values are not converted: use `napi_coerce_to_bool`).

## Coercion

These follow the ECMAScript abstract operations and may call user code
(`valueOf`, `toString`, `Symbol.toPrimitive`, proxies). If it throws, the
exception stays pending.

### `napi_coerce_to_bool`

```c
napi_status napi_coerce_to_bool(napi_env env, napi_value value,
                                napi_value *result);
```

`ToBoolean(value)`. Never runs user code, but is still refused while an
exception is pending, as in Node.js.

### `napi_coerce_to_number`

```c
napi_status napi_coerce_to_number(napi_env env, napi_value value,
                                  napi_value *result);
```

`ToNumber(value)`. A Number is returned as is. Symbols and BigInts throw a
`TypeError`; the function then returns `napi_number_expected` with the
exception pending.

### `napi_coerce_to_object`

```c
napi_status napi_coerce_to_object(napi_env env, napi_value value,
                                  napi_value *result);
```

`ToObject(value)`: objects are returned as is, primitives are wrapped (`Number`,
`String`, `Boolean`, `Symbol`, `BigInt` objects), `null` and `undefined` throw
a `TypeError` and return `napi_object_expected`.

### `napi_coerce_to_string`

```c
napi_status napi_coerce_to_string(napi_env env, napi_value value,
                                  napi_value *result);
```

`ToString(value)`. Symbols throw a `TypeError` (`napi_string_expected`, with
the exception pending).

## Arrays and comparison

### `napi_is_array`

```c
napi_status napi_is_array(napi_env env, napi_value value, bool *result);
```

`Array.isArray(value)`. It never throws: a revoked Proxy reports `false`.
A Proxy whose target is an array reports `true`, like `Array.isArray`; V8's
`IsArray` reports `false` for every Proxy (see
[compatibility.md](../compatibility.md)).

### `napi_get_array_length`

```c
napi_status napi_get_array_length(napi_env env, napi_value value,
                                  uint32_t *result);
```

Stores the `length` of an array. `napi_array_expected` when `value` is not an
array (typed arrays included).

### `napi_strict_equals`

```c
napi_status napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs,
                               bool *result);
```

`lhs === rhs` (`NaN !== NaN`, `+0 === -0`, objects by identity, strings and
BigInts by value).
