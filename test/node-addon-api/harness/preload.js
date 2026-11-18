'use strict';
// Loaded with --require (NODE_OPTIONS, so child processes load it too)
// before every node-addon-api test script.
require('module').registerBuiltin('assert', require('./qn_assert.js'));
// The tests read the Node.js major version (process.versions.node is not
// set by quickjs-napi, see README): the release whose behaviour is reproduced.
process.versions.node = process.version.slice(1);
