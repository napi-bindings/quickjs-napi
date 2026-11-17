'use strict';
// Edge cases of the M4 APIs. The expected values were produced by Node.js
// 22.23.4 with the same addon; the test runs under both runtimes.
//
//   quickjs-napi test/js/test_node_api.js <addon-dir> [--print]
//   node         test/js/test_node_api.js <addon-dir> [--print]
//
// <addon-dir>/foo%#bar/node_api.node is a copy of the addon (made by the
// build) whose file URL needs percent-encoding.

const args = typeof scriptArgs !== 'undefined' ? scriptArgs
                                               : process.argv.slice(1);
const dir = args[1];
const b = require(dir + '/node_api.node');
const { pathToFileURL } = require('url');

const order = [];
const cases = {
  // Buffer
  // The content is unspecified (Node.js: uninitialized; here zero-filled):
  // only the byte written from C is compared.
  create: () => { const x = b.create(4); return [x instanceof Buffer, x.constructor === Buffer, Buffer.isBuffer(x), x.length, x[0]]; },
  createEmpty: () => { const x = b.create(0); return [x instanceof Buffer, x.length]; },
  copy: () => { const x = b.copy('héllo'); return [x instanceof Buffer, x.toString(), x.length]; },
  external: () => { const x = b.external(); return [x instanceof Buffer, x.toString('latin1')]; },
  infoBuffer: () => b.bufferInfo(Buffer.from('abc')),
  infoSlice: () => b.bufferInfo(Buffer.from('abcdef').subarray(2, 5)),
  infoUint16: () => b.bufferInfo(new Uint16Array([0x0102, 3])),
  infoDataView: () => b.bufferInfo(new DataView(new Uint8Array([9, 8, 7]).buffer, 1)),
  infoArrayBuffer: () => b.bufferInfo(new ArrayBuffer(4)),
  infoObject: () => b.bufferInfo({ length: 1 }),
  fromAb: () => { const ab = new Uint8Array([1, 2, 3, 4]).buffer; const x = b.fromArrayBuffer(ab, 1, 2); x[0] = 77; return [x instanceof Buffer, [...x], new Uint8Array(ab)[1]]; },
  fromAbRange: () => b.fromArrayBuffer(new ArrayBuffer(4), 3, 2),
  fromSab: () => b.fromArrayBuffer(new SharedArrayBuffer(4), 0, 1),
  fromView: () => b.fromArrayBuffer(new Uint8Array(4), 0, 1),
  // make_callback and callback scopes, called from JavaScript: no early
  // microtask checkpoint (the caller's scope is still open in Node.js).
  makeCallbackOrder: () => {
    order.length = 0;
    order.push(0);
    b.makeCallback({}, {}, () => {
      Promise.resolve().then(() => order.push('micro'));
      order.push(1);
    });
    order.push(2);
    return order.slice();
  },
  makeCallbackArgs: () => b.makeCallback({}, 5, function (...a) { return [typeof this, a]; }, 1, 'x'),
  makeCallbackThrows: () => b.makeCallback({}, {}, () => { throw new RangeError('boom'); }),
  makeCallbackNullRecv: () => b.makeCallback({}, null, () => 1),
  asyncInitNull: () => b.asyncInit(null),
  asyncInitPrimitive: () => b.asyncInit(42),
  scopes: () => b.scopes(() => 'inside'),
  // File URL of a path needing percent-encoding.
  filename: () => { const p = dir + '/foo%#bar/node_api.node'; return require(p).filename === pathToFileURL(p).href; },
  encodedName: () => /foo%25%23bar\/node_api\.node$/.test(require(dir + '/foo%#bar/node_api.node').filename),
};

const expected = {
  create: '[true,true,true,4,42]',
  createEmpty: '[true,0]',
  copy: '[true,"héllo",6]',
  external: '[true,"external"]',
  infoBuffer: '{"status":0,"isBuffer":true,"length":3,"first":97}',
  infoSlice: '{"status":0,"isBuffer":true,"length":3,"first":99}',
  infoUint16: '{"status":0,"isBuffer":true,"length":4,"first":2}',
  infoDataView: '{"status":0,"isBuffer":true,"length":2,"first":8}',
  infoArrayBuffer: '{"status":1,"isBuffer":false}',
  infoObject: '{"status":1,"isBuffer":false}',
  fromAb: '[true,[77,3],77]',
  fromAbRange: '{"status":0,"exc":"RangeError:ERR_OUT_OF_RANGE:The byte offset + length is out of range"}',
  fromSab: '{"status":1}',
  fromView: '{"status":1}',
  makeCallbackOrder: '[0,1,2]',
  makeCallbackArgs: '["object",[1,"x"]]',
  makeCallbackThrows: '{"status":10,"exc":"RangeError:undefined:boom"}',
  makeCallbackNullRecv: '{"status":2,"exc":"TypeError:undefined:Cannot convert undefined or null to object"}',
  asyncInitNull: '{"status":2}',
  asyncInitPrimitive: '0',
  scopes: '["inside",14]',
  filename: 'true',
  encodedName: 'true',
  orderAfter: '[0,1,2,"micro"]',
  finalized: '1',
};

// Known deviations of quickjs-napi from Node.js, checked as such.
const deviations = {
  // Node.js returns napi_ok with the RangeError pending (and *result not
  // set); quickjs-napi returns napi_pending_exception.
  fromAbRange: '{"status":10,"exc":"RangeError:ERR_OUT_OF_RANGE:The byte offset + length is out of range"}',
};
const isNode = typeof process !== 'undefined' && !!process.versions && !!process.versions.v8;
if (!isNode) Object.assign(expected, deviations);

function show(v) {
  return JSON.stringify(v, (k, x) => {
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
  for (const key of Object.keys(results)) check(key, results[key]);

  b.leak();
  // The microtask of makeCallbackOrder ran after the script.
  Promise.resolve().then(() => {
    check('orderAfter', show(order));
    // The external buffer's finalizer runs once it is collected (deferred
    // in both runtimes). Needs gc(): node --expose-gc.
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
    console.log(`ok test_node_api (${Object.keys(expected).length} cases)`);
  }
}
