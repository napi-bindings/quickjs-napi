'use strict';
const common = require('../../common');
// quickjs-napi: no worker_threads; the addon is loaded once, in the
// main thread, and its async cleanup hooks run when the process exits.
require(`./build/${common.buildType}/binding`);
