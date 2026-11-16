'use strict';
// Minimal replacement of Node.js test/common for the vendored js-native-api
// tests running under quickjs-napi. Only what those tests use is provided.

const Module = require('module');
if (!Module.builtinModules.includes('assert')) {
  Module.registerBuiltin('assert', require('./assert'));
}
const assert = require('assert');

// The test addons are built into ./build/Release/ next to each test.
const buildType = 'Release';

const mustCallChecks = [];

function runCallChecks(exitCode) {
  if (exitCode !== 0) return;
  const failed = mustCallChecks.filter((c) =>
    'minimum' in c ? c.actual < c.minimum : c.actual !== c.exact);
  for (const c of failed) {
    console.log(`Mismatched ${c.name} function calls. Expected ${
      'minimum' in c ? `at least ${c.minimum}` : `exactly ${c.exact}`}, actual ${c.actual}.`);
    console.log(c.stack.split('\n').slice(2).join('\n'));
  }
  if (failed.length) process.exitCode = 1;
}

function trackCall(fn, field, value) {
  if (mustCallChecks.length === 0) process.on('exit', runCallChecks);
  const context = {
    [field]: value,
    actual: 0,
    stack: new Error().stack || '',
    name: fn.name || '<anonymous>',
  };
  mustCallChecks.push(context);
  return function mustCallWrapper(...args) {
    context.actual++;
    return fn.apply(this, args);
  };
}

function noop() {}

function mustCall(fn, exact) {
  if (typeof fn === 'number') {
    exact = fn;
    fn = noop;
  }
  return trackCall(fn || noop, 'exact', exact === undefined ? 1 : exact);
}

function mustCallAtLeast(fn, minimum) {
  if (typeof fn === 'number') {
    minimum = fn;
    fn = noop;
  }
  return trackCall(fn || noop, 'minimum', minimum === undefined ? 1 : minimum);
}

function mustNotCall(msg) {
  return function mustNotCall(...args) {
    assert.fail(`${msg || 'function should not have been called'} (called with ${args.length} arguments)`);
  };
}

function expectsError(validation, exact) {
  return mustCall((error) => {
    assert.throws(() => { throw error; }, validation);
    return true;
  }, exact);
}

function skip(msg) {
  console.log(`1..0 # Skipped: ${msg}`);
  process.exit(0);
}

module.exports = {
  buildType,
  isWindows: process.platform === 'win32',
  isMacOS: process.platform === 'darwin',
  isLinux: process.platform === 'linux',
  mustCall,
  mustCallAtLeast,
  mustNotCall,
  expectsError,
  platformTimeout: (ms) => ms,
  skip,
};
