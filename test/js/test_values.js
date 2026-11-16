'use strict';
// Edge cases of the M1 APIs. The expected values were produced by Node.js
// 22.23.4 with the same addon; the test runs under both runtimes.
//
//   quickjs-napi test/js/test_values.js <addon-dir>
//   node         test/js/test_values.js <addon-dir>

const args = typeof scriptArgs !== 'undefined' ? scriptArgs
                                               : process.argv.slice(1);
const run = require(args[1] + '/values.node');

const sym = Symbol('s');
const o = Object.create({ protoEnum: 1, shadow: 'proto', [Symbol('ps')]: 1 });
o[2] = 'two'; o[0] = 'zero'; o.b = 1; o.a = 2; o.shadow = 'own'; o[sym] = 3;
o['4294967295'] = 'notindex';
Object.defineProperty(o, 'hidden', { value: 1, enumerable: false, writable: false, configurable: true });
Object.defineProperty(o, 'acc', { get() { return 1; }, enumerable: true, configurable: false });
Object.defineProperty(o, Symbol('ro'), { value: 1, enumerable: true, writable: false });
const frozen = Object.freeze({ a: 1 });

const expected = {
  ownKeep: '["number:0","number:2","string:b","string:a","string:shadow","string:4294967295","string:hidden","string:acc","Symbol(s)","Symbol(ro)"]',
  protoEnum: '["string:0","string:2","string:b","string:a","string:shadow","string:4294967295","string:acc","string:protoEnum"]',
  writableSymbols: '["Symbol(s)"]',
  configurable: '["number:0","number:2","string:b","string:a","string:shadow","string:4294967295","string:hidden","Symbol(s)"]',
  setFrozenStatus: '0',
  lone: '"61 d800 62 dc00"',
  loneUtf8Bytes: '"61 ef bf bd 62 ef bf bd"',
  loneUtf8Len: '8',
  trunc5: '3',
  trunc7: '6',
  utf16n4: '3',
  utf16cut: '"61 e9 20ac"',
  latin1: '"61 e9 ac 3d 0"',
  coerceUndefStatus: '2',
  coerceUndefPending: 'true',
  instanceofStatus: '5',
  instanceofError: '"TypeError:ERR_NAPI_CONS_FUNCTION:Constructor must be a function"',
  defineFrozenStatus: '1',
  hasOwnNumStatus: '4',
  arr7: '[null,null,null,null,null,null,null]',
  u32m1: '4294967295',
  i32big: '5',
};

function show(v) {
  if (Array.isArray(v)) {
    v = v.map((x) => typeof x === 'symbol' ? x.toString() : `${typeof x}:${String(x)}`);
  }
  if (typeof v === 'string') {
    v = Array.from(v).map((c) => c.codePointAt(0).toString(16)).join(' ');
  }
  if (v instanceof Error) v = `${v.constructor.name}:${v.code}:${v.message}`;
  return JSON.stringify(v);
}

const r = run(o, frozen);
let failures = 0;
for (const key of Object.keys(expected)) {
  const actual = show(r[key]);
  if (actual !== expected[key]) {
    console.log(`${key}: expected ${expected[key]}, got ${actual}`);
    failures++;
  }
}
const extra = Object.keys(r).filter((k) => !(k in expected));
if (extra.length) {
  console.log(`unexpected keys: ${extra.join(', ')}`);
  failures++;
}
if (failures) throw new Error(`${failures} mismatch(es)`);
console.log(`ok test_values (${Object.keys(expected).length} cases)`);
