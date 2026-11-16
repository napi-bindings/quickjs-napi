#!/usr/bin/env python3
"""Vendors the Node.js js-native-api conformance tests into test/node/.

    tools/sync_node_tests.py <node-checkout>

<node-checkout> is a checkout of nodejs/node (only test/js-native-api and
LICENSE are needed, e.g. a sparse clone of the tag below). The script copies
the tests listed in TESTS, then generates test/node/js-native-api/tests.cmake
from each binding.gyp, so the build needs neither Python nor node-gyp.

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

# Tests enabled per milestone (see PIANO-IMPLEMENTAZIONE.md, section 6).
TESTS = [
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
]

# Targets not built, with the reason.
SKIP_TARGETS = {
    ("6_object_wrap", "myobject_basic_finalizer"):
        "NAPI_EXPERIMENTAL: the project headers do not declare the "
        "experimental APIs",
}

# Scripts not run, with the reason.
SKIP_SCRIPTS = {
    "6_object_wrap/test-basic-finalizer.js":
        "needs myobject_basic_finalizer (NAPI_EXPERIMENTAL)",
}

# Scripts that spawn their own child process. `args` are passed to the
# script (process.argv[2]...); with `require` the script is loaded through
# require() instead (module !== require.main), as the Node.js parent does.
# `pass` must match the output; `fail` must not.
CHILD_RUNS = {
    "test_exception/testFinalizerException.js": {
        "args": ["child"],
        "pass": "Error during Finalize",
    },
    "test_general/testEnvCleanup.js": {
        "args": ["child"],
        "pass": "(for simple wrap.*for second wrap|for second wrap.*for simple wrap)",
        "fail": "for wrap, removeWrap|for first wrap",
    },
    "test_instance_data/test.js": {
        "require": True,
        "pass": "deleting addon data",
        "fail": "Mismatched|Error",
    },
}

# Files of Node.js test/common copied verbatim next to our shim.
COMMON_FILES = ["gc.js"]

SHARED_FILES = ["common.h", "common-inl.h", "entry_point.h"]

# Documented deviations, applied after copying. Each entry is
# (file, old text, new text, reason); the sync fails if `old` is not found,
# so a changed upstream test is never patched silently. Only differences of
# the JS engine itself belong here, never Node-API semantics.
PATCHES = [
    ("test_constructor/test.js",
     "/^TypeError: Cannot assign to read only property 'readonlyValue' of "
     "object '#<MyObject>'$/",
     "/^TypeError: (?:Cannot assign to read only property 'readonlyValue' "
     "of object '#<MyObject>'|'readonlyValue' is read-only)$/",
     "engine message: QuickJS words the read-only TypeError differently"),
    ("test_constructor/test.js",
     "/^TypeError: Cannot set property .* of #<.*> which has only a getter$/",
     "/^TypeError: (?:Cannot set property .* of #<.*> which has only a "
     "getter|no setter for property)$/",
     "engine message: QuickJS words the getter-only TypeError differently"),
    ("test_properties/test.js",
     "/^TypeError: Cannot assign to read only property '.*' of object "
     "'#<Object>'$/",
     "/^TypeError: (?:Cannot assign to read only property '.*' of object "
     "'#<Object>'|'.*' is read-only)$/",
     "engine message: QuickJS words the read-only TypeError differently"),
    ("test_properties/test.js",
     "/^TypeError: Cannot set property .* of #<Object> which has only a "
     "getter$/",
     "/^TypeError: (?:Cannot set property .* of #<Object> which has only a "
     "getter|no setter for property)$/",
     "engine message: QuickJS words the getter-only TypeError differently"),
    ("6_object_wrap/test.js",
     "/^TypeError: Cannot set property .* of #<.*> which has only a getter$/",
     "/^TypeError: (?:Cannot set property .* of #<.*> which has only a "
     "getter|no setter for property)$/",
     "engine message: QuickJS words the getter-only TypeError differently"),
    ("test_object/test.js",
     "/Cannot add property w, object is not extensible/",
     "/Cannot add property w, object is not extensible|"
     "^TypeError: object is not extensible$/",
     "engine message: QuickJS words the non-extensible TypeError differently"),
    ("test_object/test.js",
     "/Cannot delete property 'x' of #<Object>/",
     "/Cannot delete property 'x' of #<Object>|"
     "^TypeError: could not delete property$/",
     "engine message: QuickJS words the non-configurable TypeError "
     "differently"),
    ("test_object/test.js",
     "/Cannot assign to read only property 'x' of object '#<Object>/",
     "/Cannot assign to read only property 'x' of object '#<Object>|"
     "^TypeError: 'x' is read-only$/",
     "engine message: QuickJS words the read-only TypeError differently"),
    ("test_object/test_object.c",
     "                                           \"Invalid type index\"));             \\\n"
     "    }                                                                          \\\n",
     "                                           \"Invalid type index\"));             \\\n"
     "      return NULL;                                                             \\\n"
     "    }                                                                          \\\n",
     "test bug: after throwing, the macro went on reading type_tags[index] "
     "out of bounds (undefined behaviour, trapped by UBSan)"),
    ("test_string/test_string.c",
     "  // We do not want the string to be copied.\n"
     "  if (copied) {\n"
     "    return napi_generic_failure;\n"
     "  }\n",
     "  // quickjs-napi: QuickJS strings cannot reference external memory, so\n"
     "  // the content is always copied (copied == true, finalizer already\n"
     "  // run), as with Node.js built with the V8 sandbox.\n"
     "  (void)copied;\n",
     "engine capability: external strings are always copied"),
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "test", "node", "js-native-api")


def read_gyp(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"#[^\n]*", "", text)  # gyp files allow Python comments
    return ast.literal_eval(text)


def cmake_list(items):
    return " ".join('"%s"' % i for i in items)


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    src_root = os.path.join(argv[0], "test", "js-native-api")
    if not os.path.isdir(src_root):
        raise SystemExit("not a Node.js checkout: %s" % argv[0])

    if os.path.isdir(DEST):
        shutil.rmtree(DEST)
    os.makedirs(DEST)
    for name in SHARED_FILES:
        shutil.copy2(os.path.join(src_root, name), os.path.join(DEST, name))
    common_dst = os.path.join(os.path.dirname(DEST), "common")
    for name in COMMON_FILES:
        shutil.copy2(os.path.join(argv[0], "test", "common", name),
                     os.path.join(common_dst, name))
    # Only the Node.js license itself (the tests are Node.js code); the rest
    # of the file covers bundled dependencies that are not vendored here.
    with open(os.path.join(argv[0], "LICENSE"), encoding="utf-8") as f:
        license_text = f.read()
    end = license_text.find("The Node.js license applies to all parts")
    if end < 0:
        raise SystemExit("unexpected Node.js LICENSE layout")
    with open(os.path.join(os.path.dirname(DEST), "LICENSE.nodejs"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(license_text[:end].rstrip() + "\n")

    lines = [
        "# GENERATED by tools/sync_node_tests.py from Node.js %s - do not edit."
        % NODE_TAG,
        "# qjsnapi_node_test(<dir> TARGET <name> SOURCES ... DEFINES ...",
        "#                   SCRIPTS ...)",
        "# qjsnapi_node_child_run(<dir> <script> [REQUIRE] ARGS ... PASS <regex>",
        "#                        [FAIL <regex>])",
        "",
    ]
    for test in TESTS:
        src = os.path.join(src_root, test)
        dst = os.path.join(DEST, test)
        shutil.copytree(src, dst)
        gyp = read_gyp(os.path.join(src, "binding.gyp"))
        all_scripts = sorted(f for f in os.listdir(src) if f.endswith(".js"))
        scripts = [f for f in all_scripts
                   if "%s/%s" % (test, f) not in SKIP_SCRIPTS
                   and "%s/%s" % (test, f) not in CHILD_RUNS]
        for target in gyp["targets"]:
            if (test, target["target_name"]) in SKIP_TARGETS:
                continue
            sources = [s for s in target.get("sources", [])
                       if s.endswith((".c", ".cc", ".cpp"))]
            lines.append("qjsnapi_node_test(%s" % test)
            lines.append("  TARGET %s" % target["target_name"])
            lines.append("  SOURCES %s" % cmake_list(sources))
            if target.get("defines"):
                lines.append("  DEFINES %s" % cmake_list(target["defines"]))
            lines.append("  SCRIPTS %s)" % cmake_list(scripts))
            # Scripts are registered once, with the first target.
            scripts = []
        for f in all_scripts:
            run = CHILD_RUNS.get("%s/%s" % (test, f))
            if not run:
                continue
            line = "qjsnapi_node_child_run(%s %s" % (test, f)
            if run.get("require"):
                line += " REQUIRE"
            if run.get("args"):
                line += " ARGS %s" % cmake_list(run["args"])
            line += " PASS %s" % cmake_list([run["pass"]])
            if run.get("fail"):
                line += " FAIL %s" % cmake_list([run["fail"]])
            lines.append(line + ")")
        lines.append("")

    with open(os.path.join(DEST, "tests.cmake"), "w", encoding="utf-8",
              newline="\n") as f:
        f.write("\n".join(lines))

    for rel, old, new, reason in PATCHES:
        path = os.path.join(DEST, rel)
        if not os.path.exists(path):
            continue  # test not enabled
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if old not in text:
            raise SystemExit("patch for %s no longer applies (%s)" % (rel, reason))
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text.replace(old, new))
        print("patched %s: %s" % (rel, reason))
    print("synced %d tests from Node.js into %s" % (len(TESTS), DEST))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
