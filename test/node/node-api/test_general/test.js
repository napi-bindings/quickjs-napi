'use strict';

const common = require('../../common');
const url = require('url');
const filename = require.resolve(`./build/${common.buildType}/test_general`);
const test_general = require(filename);
const assert = require('assert');


{
  // TODO(gabrielschulhof): This test may need updating if/when the filename
  // becomes a full-fledged URL.
  assert.strictEqual(test_general.filename, url.pathToFileURL(filename).href);
}

{
  // quickjs-napi: no child_process, fs, path (the encoding of '%' and
  // '#' in the file URL is checked by test/js/test_node_api.js).
  if (false) {
  const urlTestDir = tmpdir.resolve('foo%#bar');
  const urlTestFile = path.join(urlTestDir, path.basename(filename));
  fs.mkdirSync(urlTestDir, { recursive: true });
  fs.copyFileSync(filename, urlTestFile);
  // Use a child process as indirection so that the built-in modules is not loaded
  // into this process and can be removed here.
  const reportedFilename = child_process.spawnSync(
    process.execPath,
    ['-p', `require(${JSON.stringify(urlTestFile)}).filename`],
    { encoding: 'utf8' }).stdout.trim();
  assert.doesNotMatch(reportedFilename, /foo%#bar/);
  assert.strictEqual(reportedFilename, url.pathToFileURL(urlTestFile).href);
  fs.rmSync(urlTestDir, {
    force: true,
    recursive: true,
    maxRetries: 256,
  });
  }
}

{
  const [ major, minor, patch, release ] = test_general.testGetNodeVersion();
  assert.strictEqual(process.version.split('-')[0],
                     `v${major}.${minor}.${patch}`);
  assert.strictEqual(release, process.release.name);
}
