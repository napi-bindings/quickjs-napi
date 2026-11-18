'use strict';
// quickjs-napi runner for one node-addon-api test module, the per-module
// equivalent of upstream test/index.js (preload.js is loaded first):
//   NODE_OPTIONS=--require=.../test/preload.js \
//     quickjs-napi --expose-gc test/run.js <module>
// Requiring the module runs it; it exports a promise (or nothing). The
// module name is taken out of process.argv: some tests decide from
// process.argv whether they run as a child process.
const name = process.argv.splice(2, 1)[0];
(async () => {
  await require('./' + name);
})().catch((error) => {
  console.log(error && error.stack ? `${error}\n${error.stack}` : error);
  process.exit(1);
});
