# Objects and properties

[Node-API reference](README.md) › Objects

Implemented in `src/object.c`. All functions here may run JavaScript
(getters, setters, proxy traps, `Symbol.hasInstance`) and refuse to start
with an exception pending.

| Group | Functions |
|---|---|
| By key (`napi_value`) | [`napi_set_property`](#napi_set_property), [`napi_get_property`](#napi_get_property), [`napi_has_property`](#napi_has_property), [`napi_has_own_property`](#napi_has_own_property), [`napi_delete_property`](#napi_delete_property) |
| By name (C string) | [`napi_set_named_property`](#napi_set_named_property), [`napi_get_named_property`](#napi_get_named_property), [`napi_has_named_property`](#napi_has_named_property) |
| By index | [`napi_set_element`](#napi_set_element), [`napi_get_element`](#napi_get_element), [`napi_has_element`](#napi_has_element), [`napi_delete_element`](#napi_delete_element) |
| Definition | [`napi_define_properties`](#napi_define_properties) |
| Enumeration | [`napi_get_property_names`](#napi_get_property_names), [`napi_get_all_property_names`](#napi_get_all_property_names) |
| Prototype and integrity | [`napi_get_prototype`](#napi_get_prototype), [`napi_object_freeze`](#napi_object_freeze), [`napi_object_seal`](#napi_object_seal), [`napi_instanceof`](#napi_instanceof) |

## Common rules

These follow Node.js (`src/js_native_api_v8.cc`):

- **The receiver goes through `ToObject`.** Primitives are wrapped (so
  `napi_get_named_property(env, a_string, "length", ...)` works), while `null`
  and `undefined` throw a `TypeError` and return `napi_object_expected` with
  the exception pending.
- **Keys go through `ToPropertyKey`.** A `napi_value` key may be any value: a
  number becomes its string form, an object is converted with `toString` /
  `Symbol.toPrimitive`, which may run user code. Only `napi_has_own_property`
  and `napi_define_properties` require a string or symbol
  (`napi_name_expected`).
- **Setting is a sloppy-mode `[[Set]]`.** Assigning to a read-only property or
  to a frozen object **fails silently** and returns `napi_ok`, like
  `v8::Object::Set`. Setters and proxy `set` traps run.
- **Deleting is a sloppy-mode `delete`.** A non-configurable property is not
  deleted, and `*result` is `false`; the call still returns `napi_ok`.

---

## By key

### `napi_set_property`

```c
napi_status napi_set_property(napi_env env, napi_value object, napi_value key,
                              napi_value value);
```

`object[key] = value`.

### `napi_get_property`

```c
napi_status napi_get_property(napi_env env, napi_value object, napi_value key,
                              napi_value *result);
```

Stores `object[key]` (getters and the prototype chain included; `undefined`
when absent).

### `napi_has_property`

```c
napi_status napi_has_property(napi_env env, napi_value object, napi_value key,
                              bool *result);
```

`key in object`: own or inherited.

### `napi_has_own_property`

```c
napi_status napi_has_own_property(napi_env env, napi_value object,
                                  napi_value key, bool *result);
```

`Object.prototype.hasOwnProperty.call(object, key)`. The key must be a string
or a symbol; anything else returns `napi_name_expected` (no conversion).

### `napi_delete_property`

```c
napi_status napi_delete_property(napi_env env, napi_value object,
                                 napi_value key, bool *result);
```

`delete object[key]`. `*result` (optional, may be `NULL`) is `true` when the
property is gone afterwards (including when it never existed) and `false`
when it is non-configurable.

## By name

The name is a NUL-terminated UTF-8 string. These are shortcuts for the keyed
functions with a string key.

### `napi_set_named_property`

```c
napi_status napi_set_named_property(napi_env env, napi_value object,
                                    const char *utf8name, napi_value value);
```

`object[utf8name] = value`.

### `napi_get_named_property`

```c
napi_status napi_get_named_property(napi_env env, napi_value object,
                                    const char *utf8name, napi_value *result);
```

Stores `object[utf8name]`.

### `napi_has_named_property`

```c
napi_status napi_has_named_property(napi_env env, napi_value object,
                                    const char *utf8name, bool *result);
```

`utf8name in object`.

## By index

`index` is an array index (`0` to `2^32 - 2` are array indices; larger values
are ordinary integer-like keys).

### `napi_set_element`

```c
napi_status napi_set_element(napi_env env, napi_value object, uint32_t index,
                             napi_value value);
```

`object[index] = value`. On an array, extends `length` when needed.

### `napi_get_element`

```c
napi_status napi_get_element(napi_env env, napi_value object, uint32_t index,
                             napi_value *result);
```

Stores `object[index]` (`undefined` for holes and missing elements).

### `napi_has_element`

```c
napi_status napi_has_element(napi_env env, napi_value object, uint32_t index,
                             bool *result);
```

`index in object` (`false` for holes).

### `napi_delete_element`

```c
napi_status napi_delete_element(napi_env env, napi_value object,
                                uint32_t index, bool *result);
```

`delete object[index]`; `*result` as for `napi_delete_property`.

## Definition

### `napi_define_properties`

```c
napi_status napi_define_properties(napi_env env, napi_value object,
                                   size_t property_count,
                                   const napi_property_descriptor *properties);
```

Defines each descriptor with `Object.defineProperty` semantics (a **complete**
descriptor: attributes not set are `false`).

```c
typedef struct {
  const char *utf8name;      /* key as UTF-8, or NULL to use `name` */
  napi_value name;           /* string or symbol key */
  napi_callback method;      /* data property holding a new function */
  napi_callback getter;      /* accessor property */
  napi_callback setter;
  napi_value value;          /* data property holding this value */
  napi_property_attributes attributes;
  void *data;                /* passed to method / getter / setter */
} napi_property_descriptor;
```

| Descriptor | Defines |
|---|---|
| `getter` and/or `setter` | an accessor property; a missing half is `undefined`; `napi_writable` is ignored |
| `method` | a data property whose value is a new function (named after nothing, `length` 0) calling `method` with `data` |
| `value` (or nothing) | a data property holding `value` (or `undefined`) |

Attributes: `napi_writable`, `napi_enumerable`, `napi_configurable`, and the
combinations `napi_default` (none), `napi_default_method` (writable +
configurable, like class methods) and `napi_default_jsproperty` (all three,
like an assignment). `napi_static` is only meaningful for
[`napi_define_class`](functions.md#napi_define_class).

Definitions are applied in order and stop at the first failure:

- a key that is neither `utf8name` nor a string/symbol `name` returns
  `napi_name_expected`;
- a definition the object refuses (redefining a non-configurable property,
  adding to a non-extensible object) returns `napi_invalid_arg` for data and
  accessor properties and `napi_generic_failure` for methods, as in Node.js.

## Enumeration

### `napi_get_property_names`

```c
napi_status napi_get_property_names(napi_env env, napi_value object,
                                    napi_value *result);
```

Stores an array with the **enumerable string keys** of `object` and of its
prototype chain, array indices converted to strings: the keys a `for...in`
loop visits. Equivalent to `napi_get_all_property_names(env, object,
napi_key_include_prototypes, napi_key_enumerable | napi_key_skip_symbols,
napi_key_numbers_to_strings, result)`.

### `napi_get_all_property_names`

```c
napi_status napi_get_all_property_names(napi_env env, napi_value object,
                                        napi_key_collection_mode key_mode,
                                        napi_key_filter key_filter,
                                        napi_key_conversion key_conversion,
                                        napi_value *result);
```

Stores an array of keys, like V8's `Object::GetPropertyNames`:

| Parameter | Values |
|---|---|
| `key_mode` | `napi_key_own_only`, or `napi_key_include_prototypes` (keys of the prototype chain that are not shadowed by an earlier object) |
| `key_filter` | bit mask: `napi_key_all_properties` (0), `napi_key_writable`, `napi_key_enumerable`, `napi_key_configurable` (keep only keys with these attributes), `napi_key_skip_strings`, `napi_key_skip_symbols` |
| `key_conversion` | `napi_key_keep_numbers` (array indices as Numbers) or `napi_key_numbers_to_strings` |

Keys are in `OwnPropertyKeys` order for each object (integer indices
ascending, then strings in creation order, then symbols). An unknown
`key_mode` or `key_conversion` returns `napi_invalid_arg`. Proxy traps
(`ownKeys`, `getOwnPropertyDescriptor`) run.

## Prototype and integrity

### `napi_get_prototype`

```c
napi_status napi_get_prototype(napi_env env, napi_value object,
                               napi_value *result);
```

`Object.getPrototypeOf(object)` (`null` at the end of the chain; the proxy
`getPrototypeOf` trap runs).

### `napi_object_freeze`

```c
napi_status napi_object_freeze(napi_env env, napi_value object);
```

`Object.freeze(object)`, through the builtin captured at startup.

### `napi_object_seal`

```c
napi_status napi_object_seal(napi_env env, napi_value object);
```

`Object.seal(object)`, through the builtin captured at startup.

### `napi_instanceof`

```c
napi_status napi_instanceof(napi_env env, napi_value object,
                            napi_value constructor, bool *result);
```

`object instanceof constructor`, `Symbol.hasInstance` included. When
`constructor` is not a function, a `TypeError` with code
`ERR_NAPI_CONS_FUNCTION` ("Constructor must be a function") is thrown and the
function returns `napi_function_expected`, as in Node.js.
