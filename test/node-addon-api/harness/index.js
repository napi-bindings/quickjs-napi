'use strict';
// Stand-in for node-addon-api's package entry point (index.js), required by
// test/napi_child.js: the Node-API module flag is never needed.
module.exports = {
  isNodeApiBuiltin: true,
  needsFlag: false
};
