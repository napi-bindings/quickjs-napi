'use strict';
// Subset of Node.js `assert` (strict mode semantics) used by the vendored
// js-native-api tests. Registered as the 'assert' builtin by ./index.js.

class AssertionError extends Error {
  constructor(message) {
    super(message);
    this.name = 'AssertionError';
    this.code = 'ERR_ASSERTION';
  }
}

function inspect(v, depth = 0) {
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'bigint') return `${v}n`;
  if (typeof v === 'symbol' || typeof v === 'function') return String(v);
  if (Object.is(v, -0)) return '-0';
  if (v === null || typeof v !== 'object') return String(v);
  if (depth > 2) return Array.isArray(v) ? '[Array]' : '[Object]';
  if (v instanceof Error) return `${v.name}: ${v.message}`;
  if (Array.isArray(v)) return `[ ${v.map((x) => inspect(x, depth + 1)).join(', ')} ]`;
  const keys = Reflect.ownKeys(v);
  return `{ ${keys.map((k) => `${String(k)}: ${inspect(v[k], depth + 1)}`).join(', ')} }`;
}

function fail(message) {
  throw message instanceof Error ? message : new AssertionError(message);
}

function ok(value, message) {
  if (!value) fail(message || `The expression evaluated to a falsy value: ${inspect(value)}`);
}

function strictEqual(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    fail(message || `Expected values to be strictly equal:\n  actual:   ${inspect(actual)}\n  expected: ${inspect(expected)}`);
  }
}

function notStrictEqual(actual, expected, message) {
  if (Object.is(actual, expected)) {
    fail(message || `Expected "actual" to be strictly unequal to: ${inspect(expected)}`);
  }
}

function ownKeys(o) {
  return Object.keys(o).concat(
    Object.getOwnPropertySymbols(o).filter((s) => Object.prototype.propertyIsEnumerable.call(o, s)));
}

function isDeepStrictEqual(a, b, seen = new Map()) {
  if (Object.is(a, b)) return true;
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) return false;
  if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
  const tag = Object.prototype.toString.call(a);
  if (tag !== Object.prototype.toString.call(b)) return false;
  if (seen.get(a) === b) return true;
  seen.set(a, b);

  if (a instanceof Date) {
    if (!Object.is(a.getTime(), b.getTime())) return false;
  } else if (a instanceof RegExp) {
    if (String(a) !== String(b) || a.lastIndex !== b.lastIndex) return false;
  } else if (a instanceof Error) {
    if (a.message !== b.message || a.name !== b.name) return false;
  } else if (a instanceof Number || a instanceof String || a instanceof Boolean ||
             a instanceof BigInt || a instanceof Symbol) {
    if (!Object.is(a.valueOf(), b.valueOf())) return false;
  } else if (ArrayBuffer.isView(a)) {
    if (a.byteLength !== b.byteLength) return false;
    const x = new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
    const y = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
    for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false;
  } else if (a instanceof ArrayBuffer) {
    if (!isDeepStrictEqual(new Uint8Array(a), new Uint8Array(b), seen)) return false;
  } else if (a instanceof Map) {
    if (a.size !== b.size) return false;
    for (const [k, v] of a) {
      if (!b.has(k) || !isDeepStrictEqual(v, b.get(k), seen)) return false;
    }
  } else if (a instanceof Set) {
    if (a.size !== b.size) return false;
    for (const v of a) {
      if (b.has(v)) continue;
      if (typeof v !== 'object' || v === null) return false;
      let found = false;
      for (const w of b) if (isDeepStrictEqual(v, w, seen)) { found = true; break; }
      if (!found) return false;
    }
  }
  if (Array.isArray(a) && a.length !== b.length) return false;

  const ka = ownKeys(a);
  const kb = ownKeys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) {
    if (!Object.prototype.propertyIsEnumerable.call(b, k)) return false;
    if (!isDeepStrictEqual(a[k], b[k], seen)) return false;
  }
  return true;
}

function deepStrictEqual(actual, expected, message) {
  if (!isDeepStrictEqual(actual, expected)) {
    fail(message || `Expected values to be strictly deep-equal:\n  actual:   ${inspect(actual)}\n  expected: ${inspect(expected)}`);
  }
}

function notDeepStrictEqual(actual, expected, message) {
  if (isDeepStrictEqual(actual, expected)) {
    fail(message || `Expected "actual" not to be strictly deep-equal to: ${inspect(expected)}`);
  }
}

function checkError(actual, expected, message) {
  if (expected === undefined) return;
  if (expected instanceof RegExp) {
    if (!expected.test(String(actual))) {
      fail(message || `The input did not match the regular expression ${expected}. Input:\n${inspect(String(actual))}`);
    }
    return;
  }
  if (typeof expected === 'function') {
    if (expected.prototype !== undefined && actual instanceof expected) return;
    if (expected === Error || Error.isPrototypeOf(expected)) {
      fail(message || `The error is expected to be an instance of "${expected.name}". Received ${inspect(actual)}`);
    }
    if (expected.call({}, actual) !== true) {
      fail(message || `The ${expected.name ? `"${expected.name}" ` : ''}validation function is expected to return "true". Received ${inspect(actual)}`);
    }
    return;
  }
  if (typeof expected === 'object' && expected !== null) {
    const keys = Object.keys(expected);
    if (expected instanceof Error) keys.push('name', 'message');
    for (const key of keys) {
      const exp = expected[key];
      const act = actual === null || actual === undefined ? undefined : actual[key];
      if (exp instanceof RegExp && typeof act === 'string') {
        if (!exp.test(act)) fail(message || `Expected ${key} to match ${exp}, got ${inspect(act)}`);
      } else if (!isDeepStrictEqual(act, exp)) {
        fail(message || `Expected values to be strictly deep-equal at "${key}":\n  actual:   ${inspect(act)}\n  expected: ${inspect(exp)}`);
      }
    }
    return;
  }
  throw new TypeError(`Invalid expected value: ${inspect(expected)}`);
}

function throws(fn, expected, message) {
  if (typeof fn !== 'function') throw new TypeError('The "fn" argument must be a function');
  if (typeof expected === 'string') {
    message = expected;
    expected = undefined;
  }
  try {
    fn();
  } catch (e) {
    checkError(e, expected, message);
    return;
  }
  fail(message || `Missing expected exception${expected && expected.name ? ` (${expected.name})` : ''}.`);
}

function doesNotThrow(fn, message) {
  try {
    fn();
  } catch (e) {
    fail(typeof message === 'string'
      ? `Got unwanted exception: ${message}\nActual message: "${e && e.message}"`
      : `Got unwanted exception.\nActual message: "${e && e.message}"`);
  }
}

async function rejects(promiseOrFn, expected, message) {
  const p = typeof promiseOrFn === 'function' ? promiseOrFn() : promiseOrFn;
  try {
    await p;
  } catch (e) {
    checkError(e, expected, message);
    return;
  }
  fail(message || 'Missing expected rejection.');
}

function match(string, regexp, message) {
  if (!regexp.test(string)) {
    fail(message || `The input did not match the regular expression ${regexp}. Input:\n${inspect(string)}`);
  }
}

function assert(value, message) { ok(value, message); }
Object.assign(assert, {
  AssertionError,
  ok,
  fail,
  strictEqual,
  notStrictEqual,
  equal: strictEqual,
  notEqual: notStrictEqual,
  deepStrictEqual,
  notDeepStrictEqual,
  deepEqual: deepStrictEqual,
  notDeepEqual: notDeepStrictEqual,
  throws,
  doesNotThrow,
  rejects,
  match,
});
assert.strict = assert;

module.exports = assert;
