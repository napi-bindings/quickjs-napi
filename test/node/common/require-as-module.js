'use strict';
// quickjs-napi: loads a test script through require(), so that
// `module !== require.main`, as the Node.js test does when it re-runs itself
// in a child process (see CHILD_RUNS in tools/sync_node_tests.py).
require(process.argv[2]);
