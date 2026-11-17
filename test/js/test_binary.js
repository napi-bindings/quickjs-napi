'use strict';
// Edge cases of the M3 APIs. The expected values were produced by Node.js
// 22.23.4 with the same addon; the test runs under both runtimes.
//
//   quickjs-napi test/js/test_binary.js <addon-dir> [--print]
//   node         test/js/test_binary.js <addon-dir> [--print]

const args = typeof scriptArgs !== 'undefined' ? scriptArgs
                                               : process.argv.slice(1);
const b = require(args[1] + '/binary.node');

const detachedAb = (n) => { const ab = new ArrayBuffer(n); b.detachStatus(ab); return ab; };

// typedInfo/dataViewInfo with the buffer replaced by an identity check.
function typed(ta) {
  const r = b.typedInfo(ta);
  if ('buffer' in r) r.buffer = r.buffer === ta.buffer;
  return r;
}
function view(dv) {
  const r = b.dataViewInfo(dv);
  if ('buffer' in r) r.buffer = r.buffer === dv.buffer;
  return r;
}

const ab32 = new ArrayBuffer(32);
const u16 = new Uint16Array(16);
b.detachStatus(u16.buffer);
const dv = new DataView(new ArrayBuffer(16), 4, 8);
const dvDetached = new DataView(new ArrayBuffer(16), 4, 8);
b.detachStatus(dvDetached.buffer);
const rab = new ArrayBuffer(8, { maxByteLength: 16 });
const tracking = new Uint8Array(rab);
const fixedInRab = new Uint8Array(rab, 4, 4);
rab.resize(12);
const trackingAfterGrow = typed(tracking);
rab.resize(2);
const ab16 = new ArrayBuffer(16);
class SubDate extends Date {}
class SubPromise extends Promise {}

const cases = {
  // BigInt
  words0: () => b.words(0n, 4),
  words1: () => b.words(1n, 4),
  wordsMinus1: () => b.words(-1n, 4),
  words2p64: () => b.words(2n ** 64n, 4),
  wordsPartial: () => b.words(-(2n ** 128n) + 1n, 1),
  wordsCount: () => b.words(2n ** 64n + 5n, -1),
  wordsCount0: () => b.words(0n, -1),
  wordsNotBigint: () => b.words(123, 1),
  fromEmpty: () => b.fromWords(1, []),
  fromMinusZero: () => b.fromWords(1, ['0', '0']),
  fromTwo: () => b.fromWords(1, ['1', '1']),
  fromMax: () => b.fromWords(0, ['ffffffffffffffff']),
  tooBig: () => b.tooBig(),
  i64Max: () => b.int64(2n ** 63n - 1n),
  i64Wrap: () => b.int64(2n ** 63n),
  i64Neg: () => b.int64(-1n),
  i64Huge: () => b.int64(-(2n ** 70n) - 3n),
  u64Neg: () => b.uint64(-1n),
  u64Wrap: () => b.uint64(2n ** 64n),
  u64Max: () => b.uint64(2n ** 64n - 1n),
  i64NotBigint: () => b.int64(5),
  // ArrayBuffer
  ab: () => b.bufferInfo(new ArrayBuffer(8)),
  abEmpty: () => b.bufferInfo(new ArrayBuffer(0)),
  sab: () => b.bufferInfo(new SharedArrayBuffer(8)),
  notAb: () => b.bufferInfo({}),
  detached: () => b.bufferInfo(detachedAb(8)),
  detachTwice: () => b.detachStatus(detachedAb(4)),
  detachSab: () => b.detachStatus(new SharedArrayBuffer(4)),
  detachObject: () => b.detachStatus({}),
  external: () => { const ab = b.external(); return [b.bufferInfo(ab), new Uint8Array(ab)[3]]; },
  // TypedArray
  int32: () => typed(new Int32Array(ab32, 8, 4)),
  bigint64: () => typed(new BigInt64Array(2)),
  typedDetached: () => typed(u16),
  typedOnDataView: () => typed(dv),
  tracking: () => trackingAfterGrow,
  trackingShrunk: () => typed(tracking),
  outOfBounds: () => typed(fixedInRab),
  misaligned: () => b.createTyped(5, 2, ab16, 2),
  tooLong: () => b.createTyped(5, 5, ab16, 0),
  pastEnd: () => b.createTyped(1, 1, ab16, 16),
  onSab: () => { const t = b.createTyped(1, 2, new SharedArrayBuffer(4), 1); return [Object.prototype.toString.call(t), t.length, t.byteOffset]; },
  badType: () => b.createTyped(99, 1, ab16, 0),
  badBuffer: () => b.createTyped(1, 1, {}, 0),
  // DataView
  dataView: () => view(dv),
  dataViewDetached: () => view(dvDetached),
  dataViewOnTyped: () => view(new Uint8Array(2)),
  dvTooLong: () => b.createDataView(8, ab16, 10),
  dvAtEnd: () => { const d = b.createDataView(0, ab16, 16); return [d.byteLength, d.byteOffset]; },
  dvOnSab: () => b.createDataView(4, new SharedArrayBuffer(8), 0),
  // Date
  date: () => b.dateValue(b.createDate(1.5e12)),
  dateClip: () => b.dateValue(b.createDate(1.9)),
  dateNaN: () => String(b.dateValue(b.createDate(NaN))),
  dateRange: () => String(b.dateValue(b.createDate(8.64e15 + 1))),
  dateSub: () => b.dateValue(new SubDate(7)),
  dateObject: () => b.dateValue({ valueOf() { return 1; } }),
  // Promise
  isPromise: () => [b.isPromise(Promise.resolve()), b.isPromise(new SubPromise(() => {})),
                    b.isPromise({ then() {} }), b.isPromise(1)],
};

const expected = {
  words0: '{"count":0,"sign":0,"words":["abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab"]}',
  words1: '{"count":1,"sign":0,"words":["1","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab"]}',
  wordsMinus1: '{"count":1,"sign":1,"words":["1","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab"]}',
  words2p64: '{"count":2,"sign":0,"words":["0","1","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab"]}',
  wordsPartial: '{"count":2,"sign":1,"words":["ffffffffffffffff","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab","abababababababab"]}',
  wordsCount: '{"count":2}',
  wordsCount0: '{"count":0}',
  wordsNotBigint: '{"status":17}',
  fromEmpty: '"0n"',
  fromMinusZero: '"0n"',
  fromTwo: '"-18446744073709551617n"',
  fromMax: '"18446744073709551615n"',
  tooBig: '{"status":10,"exc":"RangeError:undefined:Maximum BigInt size exceeded"}',
  i64Max: '["9223372036854775807n",true]',
  i64Wrap: '["-9223372036854775808n",false]',
  i64Neg: '["-1n",true]',
  i64Huge: '["-3n",false]',
  u64Neg: '["18446744073709551615n",false]',
  u64Wrap: '["0n",false]',
  u64Max: '["18446744073709551615n",true]',
  i64NotBigint: '{"status":17}',
  ab: '{"status":0,"isArrayBuffer":true,"detached":false,"length":8,"hasData":true}',
  abEmpty: '{"status":0,"isArrayBuffer":true,"detached":false,"length":0,"hasData":false}',
  sab: '{"status":0,"isArrayBuffer":false,"detached":false,"length":8,"hasData":true}',
  notAb: '{"status":1,"isArrayBuffer":false,"detached":false}',
  detached: '{"status":0,"isArrayBuffer":true,"detached":true,"length":0,"hasData":false}',
  detachTwice: '0',
  detachSab: '19',
  detachObject: '19',
  external: '[{"status":0,"isArrayBuffer":true,"detached":false,"length":16,"hasData":true},4]',
  int32: '{"type":5,"length":4,"byteOffset":8,"buffer":true,"dataAt":8}',
  bigint64: '{"type":9,"length":2,"byteOffset":0,"buffer":true,"dataAt":0}',
  typedDetached: '{"type":4,"length":0,"byteOffset":0,"buffer":true,"dataAt":-1}',
  typedOnDataView: '{"status":1,"isTypedArray":false}',
  tracking: '{"type":1,"length":12,"byteOffset":0,"buffer":true,"dataAt":0}',
  trackingShrunk: '{"type":1,"length":2,"byteOffset":0,"buffer":true,"dataAt":0}',
  outOfBounds: '{"type":1,"length":0,"byteOffset":4,"buffer":true,"dataAt":4}',
  misaligned: '{"status":9,"exc":"RangeError:ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT:start offset of Int32Array should be a multiple of 4"}',
  tooLong: '{"status":9,"exc":"RangeError:ERR_NAPI_INVALID_TYPEDARRAY_LENGTH:Invalid typed array length"}',
  pastEnd: '{"status":9,"exc":"RangeError:ERR_NAPI_INVALID_TYPEDARRAY_LENGTH:Invalid typed array length"}',
  onSab: '["[object Uint8Array]",2,1]',
  badType: '{"status":1}',
  badBuffer: '{"status":1}',
  dataView: '{"byteLength":8,"byteOffset":4,"buffer":true,"dataAt":4}',
  dataViewDetached: '{"byteLength":0,"byteOffset":0,"buffer":true,"dataAt":-1}',
  dataViewOnTyped: '{"status":1,"isDataView":false}',
  dvTooLong: '{"status":10,"exc":"RangeError:ERR_NAPI_INVALID_DATAVIEW_ARGS:byte_offset + byte_length should be less than or equal to the size in bytes of the array passed in"}',
  dvAtEnd: '[0,16]',
  dvOnSab: '{"status":1}',
  date: '1500000000000',
  dateClip: '1',
  dateNaN: '"NaN"',
  dateRange: '"NaN"',
  dateSub: '7',
  dateObject: '{"status":18,"isDate":false}',
  isPromise: '[true,true,false,false]',
  settled: '["fulfilled",42,"rejected",true]',
  finalized: '1',
};

// Known deviations of quickjs-napi from Node.js, checked as such.
const deviations = {
  // QuickJS keeps no reachable byte offset for an out-of-bounds view of a
  // resizable buffer; V8 still reports the stored one (4).
  outOfBounds: '{"type":1,"length":0,"byteOffset":0,"buffer":true,"dataAt":0}',
};
const isNode = typeof process !== 'undefined' && !!process.versions && !!process.versions.v8;
if (!isNode) Object.assign(expected, deviations);

// Float16Array (napi_float16_array): an extension here; Node.js 22 has no
// Float16Array and rejects the type with napi_invalid_arg.
if (typeof Float16Array === 'function') {
  cases.float16 = () => { const t = b.createTyped(11, 2, ab16, 0); return [Object.prototype.toString.call(t), t.length, b.typedInfo(t).type]; };
  expected.float16 = '["[object Float16Array]",2,11]';
} else {
  cases.float16 = () => b.createTyped(11, 2, ab16, 0);
  expected.float16 = '{"status":1}';
}

function show(v) {
  return JSON.stringify(v, (k, x) => {
    if (typeof x === 'bigint') return `${x}n`;
    if (x instanceof Error) return `${x.constructor.name}:${x.code}:${x.message}`;
    return x;
  });
}

const results = {};
for (const key of Object.keys(cases)) results[key] = show(cases[key]());

if (args[2] === '--print') {
  for (const key of Object.keys(results)) console.log(`  ${key}: '${results[key]}',`);
} else {
  let failures = 0;
  const check = (key, actual) => {
    if (actual !== expected[key]) {
      console.log(`${key}: expected ${expected[key]}, got ${actual}`);
      failures++;
    }
  };
  for (const key of Object.keys(expected)) {
    if (key in results) check(key, results[key]);
  }
  const extra = Object.keys(results).filter((k) => !(k in expected));
  if (extra.length) {
    console.log(`no expected value for: ${extra.join(', ')}`);
    failures++;
  }

  // Promises concluded from native code, and one never concluded (its
  // deferred is released at teardown: leak-checked under the sanitizers).
  b.leak();
  const err = new TypeError('no');
  Promise.allSettled([b.settle(true, 42), b.settle(false, err)]).then((r) => {
    check('settled', show([r[0].status, r[0].value, r[1].status, r[1].reason === err]));

    // The external ArrayBuffer's finalizer runs once it is collected
    // (deferred in both runtimes). Needs gc(): node --expose-gc.
    if (typeof gc === 'function') {
      gc();
      setTimeout(() => {
        gc();
        setTimeout(() => {
          check('finalized', show(b.finalized()));
          done();
        }, 10);
      }, 10);
    } else {
      done();
    }
  });

  function done() {
    if (failures) throw new Error(`${failures} mismatch(es)`);
    console.log(`ok test_binary (${Object.keys(expected).length} cases)`);
  }
}
