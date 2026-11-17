#!/usr/bin/env python3
"""Vendors the Node.js Node-API conformance tests into test/node/.

    tools/sync_node_tests.py <node-checkout>

<node-checkout> is a checkout of nodejs/node (only test/js-native-api,
test/node-api, test/common and LICENSE are needed, e.g. a sparse clone of the
tag below). For each suite the script copies the tests listed in TESTS into
test/node/<suite>/, then generates test/node/<suite>/tests.cmake from each
binding.gyp, so the build needs neither Python nor node-gyp.

Keys of SKIP_TARGETS, SKIP_SCRIPTS, CHILD_RUNS and PATCHES start with the
suite ("js-native-api/..." or "node-api/..."): both suites have tests with
the same name (test_exception, test_general, test_instance_data).

To enable more tests as milestones land, add their directory names to TESTS
and run the script again.

Every *.js file of a test directory is a test script, as in Node.js. Two
kinds of exceptions are declared below, each with its reason:
  SKIP_TARGETS / SKIP_SCRIPTS  not built / not run;
  CHILD_RUNS                   scripts that re-run themselves through
                               child_process (not available in quickjs-napi):
                               CTest runs the child mode directly and checks
                               its output with regular expressions.
"""

import ast
import os
import re
import shutil
import sys

NODE_TAG = "v22.23.4"

# Tests enabled per suite and milestone (see PIANO-IMPLEMENTAZIONE.md,
# section 6).
TESTS = {
    "js-native-api": [
        # M1
        "2_function_arguments",
        "3_callbacks",
        "4_object_factory",
        "5_function_factory",
        "test_array",
        "test_constructor",
        "test_conversions",
        "test_error",
        "test_handle_scope",
        "test_new_target",
        "test_number",
        "test_properties",
        "test_string",
        "test_symbol",
        # M2
        "6_object_wrap",
        "7_factory_wrap",
        "8_passing_wrapped",
        "test_cannot_run_js",
        "test_exception",
        "test_function",
        "test_general",
        "test_instance_data",
        "test_object",
        "test_reference",
        "test_reference_double_free",
        # M3 (test_sharedarraybuffer is not synced: it needs the
        # NAPI_EXPERIMENTAL node_api_create_sharedarraybuffer)
        "test_bigint",
        "test_dataview",
        "test_date",
        "test_promise",
        "test_typedarray",
    ],
    "node-api": [
        # Not planned: test_async_context and test_make_callback_recurse
        # (async_hooks, domain), test_worker_* (worker_threads).
        # M4
        "1_hello_world",
        "test_buffer",
        "test_env_teardown_gc",
        "test_exception",
        "test_fatal_exception",
        "test_general",
        "test_init_order",
        "test_make_callback",
        "test_null_init",
        "test_reference_by_node_api_version",
        # M5 (libuv event loop, threads)
        "test_async",
        "test_async_cleanup_hook",
        "test_callback_scope",
        "test_cleanup_hook",
        "test_fatal",
        "test_instance_data",
        "test_threadsafe_function",
        "test_uv_loop",
        "test_uv_threadpool_size",
    ],
}

# Options of a test's "// Flags:" line passed to quickjs-napi; the others
# tune V8 (garbage collection) and have no QuickJS counterpart.
SUPPORTED_FLAGS = [
    "--expose-gc",
    "--force-node-api-uncaught-exceptions-policy",
    "--no-force-node-api-uncaught-exceptions-policy",
]

# Targets not built, with the reason.
SKIP_TARGETS = {
    ("js-native-api/6_object_wrap", "myobject_basic_finalizer"):
        "NAPI_EXPERIMENTAL: the project headers do not declare the "
        "experimental APIs",
}

# Scripts not run, with the reason.
SKIP_SCRIPTS = {
    "js-native-api/6_object_wrap/test-basic-finalizer.js":
        "needs myobject_basic_finalizer (NAPI_EXPERIMENTAL)",
    "node-api/test_make_callback/test-async-hooks.js":
        "needs async_hooks",
    "node-api/test_async/test-async-hooks.js": "needs async_hooks",
    "node-api/test_callback_scope/test-async-hooks.js": "needs async_hooks",
    "node-api/test_fatal/test_threads_report.js":
        "needs --report-on-fatalerror (diagnostic reports)",
    "node-api/test_uv_threadpool_size/node-options.js":
        "needs --env-file and child_process",
}

# Scripts that spawn their own child process. `args` are passed to the
# script (process.argv[2]...); with `require` the script is loaded through
# require() instead (module !== require.main), as the Node.js parent does.
# `pass` must match the output; `fail` must not.
CHILD_RUNS = {
    "js-native-api/test_exception/testFinalizerException.js": {
        "args": ["child"],
        "pass": "Error during Finalize",
    },
    "js-native-api/test_general/testEnvCleanup.js": {
        "args": ["child"],
        "pass": "(for simple wrap.*for second wrap|for second wrap.*for simple wrap)",
        "fail": "for wrap, removeWrap|for first wrap",
    },
    "js-native-api/test_instance_data/test.js": {
        "require": True,
        "pass": "deleting addon data",
        "fail": "Mismatched|Error",
    },
    # The parent part runs normally too (also_run, the child_process part is
    # patched out); the child throws from a completion callback.
    "node-api/test_async/test.js": {
        "args": ["child"],
        "pass": "test_async_cb_exception",
        "also_run": True,
    },
    "node-api/test_cleanup_hook/test.js": {
        "args": ["child"],
        "pass": "cleanup[(]42[)]",
    },
    # napi_fatal_error aborts the child (common.nodeProcessAborted).
    "node-api/test_fatal/test.js": {
        "args": ["child"],
        "pass": "FATAL ERROR: test_fatal::Test fatal message",
        "abort": True,
    },
    "node-api/test_fatal/test2.js": {
        "args": ["child"],
        "pass": "FATAL ERROR: test_fatal::Test fatal message",
        "abort": True,
    },
    "node-api/test_fatal/test_threads.js": {
        "args": ["child"],
        "pass": "FATAL ERROR: work_thread foobar",
        "abort": True,
    },
    # Loaded with require() (requireAs): async work, buffer finalizer and
    # thread-safe function with instance data; success is exit code 0.
    "node-api/test_instance_data/test.js": {
        "require": True,
    },
    # The child of testUnref: a thread-safe function unreferenced while its
    # thread still produces; the process must exit (code 0) without waiting.
    # Run with binding.MAX_QUEUE_SIZE (2) and with an unbounded queue (0).
    "node-api/test_threadsafe_function/test.js": {
        "variants": [["child", "2"], ["child", "0"]],
        "also_run": True,
    },
}

# Addons loaded directly in a fresh process (Node: spawnSync with
# `-e require(addon)`), with the regular expression the output must match.
ADDON_RUNS = {
    # testProcessExit: instance data finalized at exit, exit code 0.
    "node-api/test_instance_data": [
        ("test_ref_then_set", "^addon_free[\\r\\n]*$"),
        ("test_set_then_ref", "^addon_free[\\r\\n]*$"),
    ],
}

# Files of Node.js test/common copied verbatim next to our shim.
COMMON_FILES = ["gc.js", "tick.js"]

# Shared headers of each suite (node-api tests include
# ../../js-native-api/common.h).
SHARED_FILES = {
    "js-native-api": ["common.h", "common-inl.h", "entry_point.h"],
    "node-api": [],
}

# Documented deviations, applied right after copying each test (before its
# binding.gyp is read). Each entry is
# (file, old text, new text, reason); the sync fails if `old` is not found,
# so a changed upstream test is never patched silently. Only differences of
# the JS engine, or capabilities the quickjs-napi host does not have
# (worker_threads, child_process, fs, experimental APIs), belong here, never
# Node-API semantics.
PATCHES = [
    ("js-native-api/test_constructor/test.js",
     "/^TypeError: Cannot assign to read only property 'readonlyValue' of "
     "object '#<MyObject>'$/",
     "/^TypeError: (?:Cannot assign to read only property 'readonlyValue' "
     "of object '#<MyObject>'|'readonlyValue' is read-only)$/",
     "engine message: QuickJS words the read-only TypeError differently"),
    ("js-native-api/test_constructor/test.js",
     "/^TypeError: Cannot set property .* of #<.*> which has only a getter$/",
     "/^TypeError: (?:Cannot set property .* of #<.*> which has only a "
     "getter|no setter for property)$/",
     "engine message: QuickJS words the getter-only TypeError differently"),
    ("js-native-api/test_properties/test.js",
     "/^TypeError: Cannot assign to read only property '.*' of object "
     "'#<Object>'$/",
     "/^TypeError: (?:Cannot assign to read only property '.*' of object "
     "'#<Object>'|'.*' is read-only)$/",
     "engine message: QuickJS words the read-only TypeError differently"),
    ("js-native-api/test_properties/test.js",
     "/^TypeError: Cannot set property .* of #<Object> which has only a "
     "getter$/",
     "/^TypeError: (?:Cannot set property .* of #<Object> which has only a "
     "getter|no setter for property)$/",
     "engine message: QuickJS words the getter-only TypeError differently"),
    ("js-native-api/6_object_wrap/test.js",
     "/^TypeError: Cannot set property .* of #<.*> which has only a getter$/",
     "/^TypeError: (?:Cannot set property .* of #<.*> which has only a "
     "getter|no setter for property)$/",
     "engine message: QuickJS words the getter-only TypeError differently"),
    ("js-native-api/test_object/test.js",
     "/Cannot add property w, object is not extensible/",
     "/Cannot add property w, object is not extensible|"
     "^TypeError: object is not extensible$/",
     "engine message: QuickJS words the non-extensible TypeError differently"),
    ("js-native-api/test_object/test.js",
     "/Cannot delete property 'x' of #<Object>/",
     "/Cannot delete property 'x' of #<Object>|"
     "^TypeError: could not delete property$/",
     "engine message: QuickJS words the non-configurable TypeError "
     "differently"),
    ("js-native-api/test_object/test.js",
     "/Cannot assign to read only property 'x' of object '#<Object>/",
     "/Cannot assign to read only property 'x' of object '#<Object>|"
     "^TypeError: 'x' is read-only$/",
     "engine message: QuickJS words the read-only TypeError differently"),
    ("js-native-api/test_object/test_object.c",
     "                                           \"Invalid type index\"));             \\\n"
     "    }                                                                          \\\n",
     "                                           \"Invalid type index\"));             \\\n"
     "      return NULL;                                                             \\\n"
     "    }                                                                          \\\n",
     "test bug: after throwing, the macro went on reading type_tags[index] "
     "out of bounds (undefined behaviour, trapped by UBSan)"),
    ("js-native-api/test_string/test_string.c",
     "  // We do not want the string to be copied.\n"
     "  if (copied) {\n"
     "    return napi_generic_failure;\n"
     "  }\n",
     "  // quickjs-napi: QuickJS strings cannot reference external memory, so\n"
     "  // the content is always copied (copied == true, finalizer already\n"
     "  // run), as with Node.js built with the V8 sandbox.\n"
     "  (void)copied;\n",
     "engine capability: external strings are always copied"),
    ("node-api/1_hello_world/test.js",
     "const { Worker } = require('worker_threads');\n",
     "",
     "host capability: no worker_threads"),
    ("node-api/1_hello_world/test.js",
     "new Worker(`\n"
     "const { parentPort } = require('worker_threads');\n"
     "const msg = require(${JSON.stringify(bindingPath)}).hello();\n"
     "parentPort.postMessage(msg)`, { eval: true })\n"
     "  .on('message', common.mustCall((msg) => assert.strictEqual(msg, 'world')));\n",
     "// quickjs-napi: no worker_threads.\n",
     "host capability: no worker_threads"),
    ("node-api/test_buffer/binding.gyp",
     '        "NAPI_EXPERIMENTAL",\n'
     '        "NODE_API_EXPERIMENTAL_NO_WARNING"\n',
     '        "NAPI_VERSION=10"\n',
     "experimental API: the project headers ignore NAPI_EXPERIMENTAL (it "
     "would select NAPI_VERSION 8); the stable APIs used need version 10"),
    ("node-api/test_buffer/test_buffer.c",
     "static char externalSharedArrayBufferData[1];\n",
     "#if 0 /* quickjs-napi: NAPI_EXPERIMENTAL "
     "node_api_create_external_sharedarraybuffer is not provided */\n"
     "static char externalSharedArrayBufferData[1];\n",
     "experimental API: node_api_create_external_sharedarraybuffer is not "
     "provided"),
    ("node-api/test_buffer/test_buffer.c",
     "static napi_value getDeleterCallCount(",
     "#endif\n\nstatic napi_value getDeleterCallCount(",
     "experimental API: node_api_create_external_sharedarraybuffer is not "
     "provided"),
    ("node-api/test_buffer/test_buffer.c",
     "      DECLARE_NODE_API_PROPERTY(\"newExternalSharedArrayBuffer\",\n"
     "                                newExternalSharedArrayBuffer),\n",
     "",
     "experimental API: node_api_create_external_sharedarraybuffer is not "
     "provided"),
    ("node-api/test_buffer/test.js",
     "  let sab = binding.newExternalSharedArrayBuffer();\n"
     "  assert(util.types.isSharedArrayBuffer(sab));\n"
     "  sab = null;\n"
     "  global.gc();\n"
     "  await tick(10);\n"
     "  console.log('gc3');\n"
     "  assert.strictEqual(binding.getDeleterCallCount(), 3);\n",
     "  // quickjs-napi: node_api_create_external_sharedarraybuffer\n"
     "  // (NAPI_EXPERIMENTAL) is not provided.\n",
     "experimental API: node_api_create_external_sharedarraybuffer is not "
     "provided"),
    ("node-api/test_general/test.js",
     "const tmpdir = require('../../common/tmpdir');\n"
     "const child_process = require('child_process');\n"
     "const fs = require('fs');\n"
     "const path = require('path');\n",
     "",
     "host capability: no child_process, fs, path"),
    ("node-api/test_async/test.js",
     "const child_process = require('child_process');\n",
     "",
     "host capability: no child_process (the child mode runs as its own "
     "CTest, see CHILD_RUNS)"),
    ("node-api/test_async/test.js",
     "const p = child_process.spawnSync(\n"
     "  process.execPath, [ __filename, 'child' ]);\n"
     "assert.ifError(p.error);\n"
     "assert.ok(p.stderr.toString().includes(testException));\n",
     "",
     "host capability: no child_process (the child mode runs as its own "
     "CTest, see CHILD_RUNS)"),
    ("node-api/test_async_cleanup_hook/test.js",
     "const path = require('path');\n"
     "const { Worker } = require('worker_threads');\n"
     "const binding = path.resolve(__dirname, `./build/${common.buildType}/binding`);\n"
     "\n"
     "const w = new Worker(`require(${JSON.stringify(binding)})`, { eval: true });\n"
     "w.on('exit', common.mustCall(() => require(binding)));\n",
     "// quickjs-napi: no worker_threads; the addon is loaded once, in the\n"
     "// main thread, and its async cleanup hooks run when the process exits.\n"
     "require(`./build/${common.buildType}/binding`);\n",
     "host capability: no worker_threads"),
    ("node-api/test_cleanup_hook/test.js",
     "const child_process = require('child_process');\n",
     "",
     "host capability: no child_process (the child mode runs as its own "
     "CTest, see CHILD_RUNS)"),
    ("node-api/test_fatal/test.js",
     "const child_process = require('child_process');\n",
     "",
     "host capability: no child_process (the child mode runs as its own "
     "CTest, see CHILD_RUNS)"),
    ("node-api/test_fatal/test2.js",
     "const child_process = require('child_process');\n",
     "",
     "host capability: no child_process (the child mode runs as its own "
     "CTest, see CHILD_RUNS)"),
    ("node-api/test_fatal/test_threads.js",
     "const child_process = require('child_process');\n",
     "",
     "host capability: no child_process (the child mode runs as its own "
     "CTest, see CHILD_RUNS)"),
    ("node-api/test_threadsafe_function/test.js",
     "const { fork } = require('child_process');\n",
     "",
     "host capability: no child_process"),
    ("node-api/test_threadsafe_function/test.js",
     "// Start a child process to test rapid teardown\n"
     ".then(() => testUnref(binding.MAX_QUEUE_SIZE))\n"
     "\n"
     "// Start a child process with an infinite queue to test rapid teardown\n"
     ".then(() => testUnref(0))\n",
     "// quickjs-napi: no child_process; the child mode of testUnref runs as\n"
     "// its own CTest (CHILD_RUNS).\n",
     "host capability: no child_process"),
    ("node-api/test_threadsafe_function/binding.gyp",
     "        'NAPI_EXPERIMENTAL'\n",
     "        'NAPI_VERSION=10'\n",
     "experimental API: the project headers ignore NAPI_EXPERIMENTAL (it "
     "would select NAPI_VERSION 8, i.e. the legacy uncaught-exception "
     "policy); the target exercises version 10"),
    ("node-api/test_general/test.js",
     "tmpdir.refresh();\n",
     "",
     "host capability: no child_process, fs, path"),
    ("node-api/test_general/test.js",
     "  const urlTestDir = tmpdir.resolve('foo%#bar');\n",
     "  // quickjs-napi: no child_process, fs, path (the encoding of '%' and\n"
     "  // '#' in the file URL is checked by test/js/test_node_api.js).\n"
     "  if (false) {\n"
     "  const urlTestDir = tmpdir.resolve('foo%#bar');\n",
     "host capability: no child_process, fs, path"),
    ("node-api/test_general/test.js",
     "    maxRetries: 256,\n"
     "  });\n"
     "}\n",
     "    maxRetries: 256,\n"
     "  });\n"
     "  }\n"
     "}\n",
     "host capability: no child_process, fs, path"),
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_ROOT = os.path.join(ROOT, "test", "node")


def read_gyp(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"#[^\n]*", "", text)  # gyp files allow Python comments
    return ast.literal_eval(text)


def cmake_list(items):
    return " ".join('"%s"' % i for i in items)


def child_run_line(suite, test, script, run, args, name):
    line = "qjsnapi_node_child_run(%s %s %s" % (suite, test, script)
    if name:
        line += ' NAME "%s"' % name
    if run.get("require"):
        line += " REQUIRE"
    if run.get("abort"):
        line += " ABORT"
    if args:
        line += " ARGS %s" % cmake_list(args)
    if run.get("pass"):
        line += " PASS %s" % cmake_list([run["pass"]])
    if run.get("fail"):
        line += " FAIL %s" % cmake_list([run["fail"]])
    return line + ")"


def script_flags(path):
    """The supported options of the script's "// Flags:" line."""
    try:
        with open(path, encoding="utf-8") as f:
            head = f.read().splitlines()[:5]
    except (OSError, UnicodeDecodeError) as e:
        raise SystemExit("cannot read %s: %s" % (path, e))
    for line in head:
        line = line.strip()
        if line.startswith("// Flags:"):
            return [o for o in line[len("// Flags:"):].split()
                    if o in SUPPORTED_FLAGS]
    return []


def apply_patches(suite, test):
    prefix = "%s/%s/" % (suite, test)
    for rel, old, new, reason in PATCHES:
        if not rel.startswith(prefix):
            continue
        path = os.path.join(TEST_ROOT, rel)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if old not in text:
            raise SystemExit("patch for %s no longer applies (%s)" % (rel, reason))
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text.replace(old, new))
        print("patched %s: %s" % (rel, reason))


def sync_suite(node, suite):
    src_root = os.path.join(node, "test", suite)
    dest = os.path.join(TEST_ROOT, suite)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    for name in SHARED_FILES[suite]:
        shutil.copy2(os.path.join(src_root, name), os.path.join(dest, name))

    lines = [
        "# GENERATED by tools/sync_node_tests.py from Node.js %s - do not edit."
        % NODE_TAG,
        "# qjsnapi_node_test(<suite> <dir> TARGET <name> SOURCES ... DEFINES ...",
        "#                   SCRIPTS ...)",
        "# qjsnapi_node_flags(<suite> <dir> <script> <option>...)",
        "# qjsnapi_node_child_run(<suite> <dir> <script> [NAME <name>]",
        "#                        [REQUIRE] [ABORT]",
        "#                        [ARGS ...] [PASS <regex>] [FAIL <regex>])",
        "# qjsnapi_node_addon_run(<suite> <dir> <target> PASS <regex>)",
        "",
    ]
    for test in TESTS[suite]:
        src = os.path.join(src_root, test)
        dst = os.path.join(dest, test)
        shutil.copytree(src, dst)
        apply_patches(suite, test)
        gyp = read_gyp(os.path.join(dst, "binding.gyp"))
        key = "%s/%s" % (suite, test)
        all_scripts = sorted(f for f in os.listdir(src) if f.endswith(".js"))
        scripts = [f for f in all_scripts
                   if "%s/%s" % (key, f) not in SKIP_SCRIPTS
                   and ("%s/%s" % (key, f) not in CHILD_RUNS
                        or CHILD_RUNS["%s/%s" % (key, f)].get("also_run"))]
        for f in all_scripts:
            flags = script_flags(os.path.join(dst, f))
            if flags and "%s/%s" % (key, f) not in SKIP_SCRIPTS:
                lines.append("qjsnapi_node_flags(%s %s %s %s)"
                             % (suite, test, f, cmake_list(flags)))
        for target in gyp["targets"]:
            if (key, target["target_name"]) in SKIP_TARGETS:
                continue
            sources = [s for s in target.get("sources", [])
                       if s.endswith((".c", ".cc", ".cpp"))]
            lines.append("qjsnapi_node_test(%s %s" % (suite, test))
            lines.append("  TARGET %s" % target["target_name"])
            lines.append("  SOURCES %s" % cmake_list(sources))
            if target.get("defines"):
                lines.append("  DEFINES %s" % cmake_list(target["defines"]))
            lines.append("  SCRIPTS %s)" % cmake_list(scripts))
            # Scripts are registered once, with the first target.
            scripts = []
        for f in all_scripts:
            run = CHILD_RUNS.get("%s/%s" % (key, f))
            if not run:
                continue
            # "variants": one test per argument list, named after it.
            for args, name in (
                    [(v, "%s:%s" % (f, "-".join(v))) for v in run["variants"]]
                    if run.get("variants") else [(run.get("args"), None)]):
                lines.append(child_run_line(suite, test, f, run, args, name))
        for target, regex in ADDON_RUNS.get(key, []):
            lines.append("qjsnapi_node_addon_run(%s %s %s PASS %s)"
                         % (suite, test, target, cmake_list([regex])))
        lines.append("")

    with open(os.path.join(dest, "tests.cmake"), "w", encoding="utf-8",
              newline="\n") as f:
        f.write("\n".join(lines))
    print("synced %d %s tests into %s" % (len(TESTS[suite]), suite, dest))


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    node = argv[0]
    for suite in TESTS:
        if not os.path.isdir(os.path.join(node, "test", suite)):
            raise SystemExit("not a Node.js checkout with test/%s: %s"
                             % (suite, node))

    common_dst = os.path.join(TEST_ROOT, "common")
    for name in COMMON_FILES:
        shutil.copy2(os.path.join(node, "test", "common", name),
                     os.path.join(common_dst, name))
    # Only the Node.js license itself (the tests are Node.js code); the rest
    # of the file covers bundled dependencies that are not vendored here.
    with open(os.path.join(node, "LICENSE"), encoding="utf-8") as f:
        license_text = f.read()
    end = license_text.find("The Node.js license applies to all parts")
    if end < 0:
        raise SystemExit("unexpected Node.js LICENSE layout")
    with open(os.path.join(TEST_ROOT, "LICENSE.nodejs"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(license_text[:end].rstrip() + "\n")

    for suite in TESTS:
        sync_suite(node, suite)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
