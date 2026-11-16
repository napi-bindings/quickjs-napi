'use strict';
// Runs unchanged under quickjs-napi and under Node.js: the Node.js run is the
// reference that keeps these expectations honest.
//
//   quickjs-napi test/js/test_hello.js <addon-dir>
//   node         test/js/test_hello.js <addon-dir>

const args = typeof scriptArgs !== 'undefined' ? scriptArgs
                                               : process.argv.slice(1);
const dir = args[1];
const isNode = typeof process !== 'undefined' && !!process.versions &&
               !!process.versions.node;

let checks = 0;
function eq(actual, expected, what) {
  checks++;
  if (actual !== expected) {
    throw new Error(`${what}: expected ${String(expected)}, got ${String(actual)}`);
  }
}
function throws(fn, check, what) {
  checks++;
  try {
    fn();
  } catch (e) {
    check(e);
    return;
  }
  throw new Error(`${what}: expected an exception`);
}

const hello = require(dir + '/hello.node');

// Basic calls and arguments
eq(hello.hello(), 'world', 'hello()');
eq(hello.echo('x'), 'x', 'echo(string)');
const o = {};
eq(hello.echo(o), o, 'echo(object) keeps identity');
eq(hello.echo(), undefined, 'echo() pads missing arguments with undefined');
eq(hello.argCount(1, 2, 3), 3, 'argCount');
eq(hello.argCount(), 0, 'argCount()');
eq(hello.data(), 'callback data', 'callback data pointer');
const receiver = { m: hello.thisArg };
eq(receiver.m(), receiver, 'this as method receiver');

// Function objects
eq(typeof hello.hello, 'function', 'typeof');
eq(hello.hello.name, 'hello', 'name');
eq(hello.hello.length, 0, 'length');
eq(Object.getPrototypeOf(hello.hello), Function.prototype, 'prototype chain');
eq(hello.hello.call(null), 'world', 'Function.prototype.call');

// Constructor calls and new.target
eq(hello.isConstructCall(), false, 'plain call has no new.target');
eq(new hello.isConstructCall() instanceof hello.isConstructCall, true,
   'construct call returns the receiver when the result is not an object');
const inst = new hello.Ctor();
eq(inst.created, true, 'new Ctor() sees new.target');
eq(inst instanceof hello.Ctor, true, 'instanceof Ctor');
class Sub extends hello.Ctor {}
const sub = new Sub();
eq(sub instanceof Sub, true, 'subclass instance built from new.target');
eq(sub.created, true, 'subclass constructor ran');

// Exceptions
throws(() => hello.throwError(), (e) => {
  eq(e instanceof Error, true, 'throwError: Error');
  eq(e.message, 'boom', 'throwError: message');
  eq(e.code, 'ERR_HELLO', 'throwError: code');
}, 'throwError');
throws(() => hello.throwTypeError(), (e) => {
  eq(e instanceof TypeError, true, 'TypeError');
  eq(e.code, undefined, 'no code when NULL');
}, 'throwTypeError');
throws(() => hello.throwRangeError(), (e) => {
  eq(e instanceof RangeError, true, 'RangeError');
  eq(e.code, 'ERR_RANGE', 'RangeError code');
}, 'throwRangeError');
throws(() => hello.throwSyntaxError(), (e) => {
  eq(e instanceof SyntaxError, true, 'SyntaxError');
}, 'throwSyntaxError');
throws(() => hello.throwValue(42), (e) => eq(e, 42, 'napi_throw of a primitive'),
       'throwValue');
const created = hello.createError();
eq(created instanceof Error, true, 'createError returns an Error');
eq(created.code, 'ERR_CREATED', 'createError code');
eq(created.message, 'created', 'createError message');
const caught = hello.throwAndCatch();
eq(caught.error.message, 'caught', 'get_and_clear_last_exception');
eq(caught.stillPending, false, 'exception cleared');

// Error info, scopes, versions
const info = hello.lastErrorInfo();
eq(info.status, 1, 'napi_invalid_arg');
eq(info.message, 'Invalid argument', 'error message');
for (let i = 0; i < 50; i++) eq(hello.scopes(), 'escaped', 'escapable scope');
eq(hello.version() >= 9, true, 'napi_get_version');
const url = hello.moduleFileName();
eq(url.startsWith('file://'), true, 'module file name is a file:// URL');
eq(url.endsWith('/hello.node'), true, 'module file name ends with the file');

// typeof
eq(hello.typeOf(undefined), 'undefined', 'typeof undefined');
eq(hello.typeOf(null), 'null', 'typeof null');
eq(hello.typeOf(true), 'boolean', 'typeof boolean');
eq(hello.typeOf(1.5), 'number', 'typeof number');
eq(hello.typeOf(7), 'number', 'typeof int');
eq(hello.typeOf('s'), 'string', 'typeof string');
eq(hello.typeOf(Symbol('s')), 'symbol', 'typeof symbol');
eq(hello.typeOf({}), 'object', 'typeof object');
eq(hello.typeOf([]), 'object', 'typeof array');
eq(hello.typeOf(() => 0), 'function', 'typeof function');
eq(hello.typeOf(hello.hello), 'function', 'typeof native function');
eq(hello.typeOf(10n), 'bigint', 'typeof bigint');

// Loader
eq(require(dir + '/hello.node'), hello, 'second require returns cached exports');
eq(require(dir + '/legacy.node').kind, 'legacy', 'napi_module_register');
const answer = require(dir + '/fnexport.node');
eq(typeof answer, 'function', 'init return value replaces exports');
eq(answer(), 42, 'exported function works');
// Node.js 22.23 crashes (SIGSEGV) on an addon declaring an unsupported
// Node-API version instead of throwing; quickjs-napi rejects it cleanly.
if (!isNode) {
  throws(() => require(dir + '/future.node'), (e) => {
    eq(e.code, 'ERR_DLOPEN_FAILED', 'newer Node-API version is rejected');
  }, 'future.node');
}
throws(() => require(dir + '/does-not-exist.node'), (e) => {
  eq(typeof e.code, 'string', 'missing addon has an error code');
}, 'missing addon');

if (typeof gc === 'function') gc();
console.log(`ok test_hello (${isNode ? 'node' : 'quickjs-napi'}, ${checks} checks)`);
