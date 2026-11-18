'use strict';
// The executable's Node.js-compatible host features used by the test suites:
// child_process (spawnSync, spawn), -e, -r / NODE_OPTIONS, process.env.
// Every child is this same executable, so the test runs on every platform;
// it passes under Node.js 22 as well:
//
//   quickjs-napi test/js/test_host.js
//   node         test/js/test_host.js

const { spawnSync, spawn } = require('child_process');

let failures = 0;
function check(cond, what) {
  if (!cond) {
    failures++;
    console.log(`FAIL: ${what}`);
  }
}

const exe = process.execPath;
const run = (args, options) =>
  spawnSync(exe, args, Object.assign({ encoding: 'utf8' }, options));

// spawnSync: exit code, both streams, process.argv with -e.
{
  const r = run(['-e', 'console.log(process.argv.length, process.argv[1]);' +
                       'console.error("to stderr"); process.exit(3)', 'arg']);
  check(r.status === 3 && r.signal === null, `exit status 3 (got ${r.status})`);
  check(r.stdout === '2 arg\n', `-e argv (got ${JSON.stringify(r.stdout)})`);
  check(r.stderr === 'to stderr\n', 'stderr captured');
  check(Array.isArray(r.output) && r.output[1] === r.stdout &&
        r.output[2] === r.stderr, 'output array');
  check(typeof r.pid === 'number' && r.pid > 0, 'pid');
}

// Buffers without encoding.
{
  const r = spawnSync(exe, ['-e', 'console.log("x")']);
  check(Buffer.isBuffer(r.stdout) && r.stdout.toString() === 'x\n',
        'stdout is a Buffer without encoding');
}

// A program that does not exist.
{
  const r = spawnSync(exe + '-does-not-exist', []);
  check(r.error instanceof Error && r.error.code === 'ENOENT',
        `ENOENT (got ${r.error && r.error.code})`);
  check(r.status === null, 'no status without a process');
}

// timeout: the child is killed with SIGTERM.
{
  const r = run(['-e', 'setTimeout(() => {}, 20000)'], { timeout: 200 });
  check(r.error && r.error.code === 'ETIMEDOUT',
        `ETIMEDOUT (got ${r.error && r.error.code})`);
  check(r.signal === 'SIGTERM', `killed by SIGTERM (got ${r.signal})`);
}

// process.env, passed on to the child (env option).
{
  const env = Object.assign({}, process.env, { QJSNAPI_TEST_VAR: 'value 1' });
  const r = run(['-e', 'console.log(process.env.QJSNAPI_TEST_VAR)'], { env });
  check(r.stdout === 'value 1\n', `env (got ${JSON.stringify(r.stdout)})`);
  check(typeof process.env.PATH === 'string' ||
        typeof process.env.Path === 'string', 'process.env has PATH');
}

// --require, from the command line and from NODE_OPTIONS (where options
// only Node.js knows are ignored).
{
  const preload = `${__dirname}/fixtures/preload.js`;
  const a = run(['-r', preload, '-e', 'console.log(globalThis.PRELOADED)']);
  check(a.stdout === '42\n', `-r (got ${JSON.stringify(a.stdout)} ${a.stderr})`);
  const env = Object.assign({}, process.env, {
    NODE_OPTIONS: `--max-old-space-size=100 --require=${preload}`
  });
  const b = run(['-e', 'console.log(globalThis.PRELOADED)'], { env });
  check(b.stdout === '42\n',
        `NODE_OPTIONS --require (got ${JSON.stringify(b.stdout)} ${b.stderr})`);
}

// spawn: data, then 'exit', then 'close'; kill().
const events = [];
const child = spawn(exe, ['-e', 'console.log("async child")']);
child.stdout.setEncoding('utf8');
child.stdout.on('data', (d) => events.push(`data:${d.trim()}`));
child.on('exit', (code) => events.push(`exit:${code}`));
child.on('close', (code) => {
  events.push(`close:${code}`);
  check(events.join(' ') === 'data:async child exit:0 close:0',
        `spawn events (got ${events.join(' ')})`);
  const sleeper = spawn(exe, ['-e', 'setTimeout(() => {}, 20000)'],
                        { stdio: 'ignore' });
  sleeper.on('close', (code, signal) => {
    check(code === null && signal === 'SIGTERM',
          `kill (got ${code} ${signal})`);
    check(sleeper.kill() === false, 'kill() after the exit is false');
    // A spawn failure is an 'error' event.
    const bad = spawn(exe + '-does-not-exist', []);
    bad.on('error', (e) => {
      check(e.code === 'ENOENT', `spawn ENOENT (got ${e.code})`);
      finish();
    });
  });
  check(sleeper.kill() === true, 'kill() of a running child');
});

function finish() {
  if (failures) {
    console.log(`${failures} failure(s)`);
    process.exitCode = 1;
  } else {
    console.log('ok');
  }
}
