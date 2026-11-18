'use strict';
// quickjs-napi replacement for node-addon-api's test/common/index.js (the
// upstream file needs readline, os and fs/promises). Same exports, same
// semantics for the parts the tests use; async_hooks helpers are absent.
const assert = require('assert');

const mustCallChecks = [];

function runCallChecks (exitCode) {
  if (exitCode !== 0) return;
  const failed = mustCallChecks.filter((c) =>
    'minimum' in c ? c.actual < c.minimum : c.actual !== c.exact);
  for (const c of failed) {
    console.log('Mismatched %s function calls. Expected %s, actual %d.',
      c.name, 'minimum' in c ? `at least ${c.minimum}` : `exactly ${c.exact}`,
      c.actual);
    console.log(c.stack.split('\n').slice(2).join('\n'));
  }
  if (failed.length) process.exit(1);
}

function _mustCallInner (fn, criteria, field) {
  if (typeof fn === 'number') {
    criteria = fn;
    fn = () => {};
  } else if (fn === undefined) {
    fn = () => {};
  }
  if (criteria === undefined) criteria = 1;
  if (typeof criteria !== 'number') {
    throw new TypeError(`Invalid ${field} value: ${criteria}`);
  }
  const context = {
    [field]: criteria,
    actual: 0,
    stack: (new Error()).stack,
    name: fn.name || '<anonymous>'
  };
  if (mustCallChecks.length === 0) process.on('exit', runCallChecks);
  mustCallChecks.push(context);
  return function () {
    context.actual++;
    return fn.apply(this, arguments);
  };
}

exports.mustCall = (fn, exact) => _mustCallInner(fn, exact, 'exact');
exports.mustCallAtLeast = (fn, minimum) =>
  _mustCallInner(fn, minimum, 'minimum');
exports.mustNotCall = (msg) => function mustNotCall () {
  assert.fail(msg || 'function should not have been called');
};

// CMake builds the bindings into test/build/Release whatever the build type.
exports.whichBuildType = async () => 'Release';

const variants = ['binding', 'binding_noexcept', 'binding_noexcept_maybe',
  'binding_custom_namespace'];
const bindingPaths = (buildType) =>
  variants.map((v) => `${__dirname}/../build/${buildType}/${v}.node`);

// As upstream, the first await defers the test until the requiring module
// has finished evaluating (its later `const` declarations are initialized).
exports.runTest = async function (test, buildType) {
  buildType = buildType || await exports.whichBuildType();
  for (const item of bindingPaths(buildType)) {
    await Promise.resolve(test(require(item), { bindingPath: item }))
      .finally(exports.mustCall());
  }
};

exports.runTestWithBindingPath = async function (test, buildType) {
  buildType = buildType || await exports.whichBuildType();
  for (const item of bindingPaths(buildType)) await test(item);
};

exports.runTestWithBuildType = async function (test, buildType) {
  buildType = buildType || await exports.whichBuildType();
  await Promise.resolve(test(buildType)).finally(exports.mustCall());
};

// Upstream spawns `node -e` with readline over stderr; spawnSync gives the
// same result: exit code, signal and the expected stderr lines.
exports.runTestInChildProcess = function ({ suite, testName, expectedStderr, execArgv }) {
  const { spawnSync } = require('child_process');
  return exports.runTestWithBindingPath((bindingName) => {
    const suitePath = `${__dirname}/../child_processes/${suite}`;
    const child = spawnSync(process.execPath, [
      '--expose-gc',
      ...(execArgv ?? []),
      '-e',
      `require(${JSON.stringify(suitePath)}).${testName}(require(${JSON.stringify(bindingName)}))`
    ], { encoding: 'utf8' });
    const stderr = (child.stderr || '').split(/\r?\n/).filter((l) => l !== '');
    const actual = { stderr, code: child.status, signal: child.signal };
    const expected = { stderr: expectedStderr, code: 0, signal: null };
    if (!expectedStderr) {
      delete actual.stderr;
      delete expected.stderr;
    } else {
      actual.stderr = actual.stderr.filter((line) => expectedStderr.includes(line));
    }
    assert.deepStrictEqual(actual, expected,
      `Assertion for child process test ${suite}.${testName} failed:\n` +
      stderr.map((l) => `from child process: ${l}`).join('\n'));
  });
};
