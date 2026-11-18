#!/usr/bin/env python3
"""Vendors the node-addon-api test suite into test/node-addon-api.

Usage: tools/sync_node_addon_api.py <node-addon-api checkout>

Copies the headers (napi.h, napi-inl.h, napi-inl.deprecated.h), the license
and the test directory of a node-addon-api release, applies PATCHES (each
with its reason) and writes test/node-addon-api/tests.cmake: the sources of
the eight bindings of test/binding.gyp and one CTest per test module (the
modules that upstream test/index.js would run). test/node-addon-api/harness
holds the quickjs-napi files laid over the copy at build time (common
helpers, runner); it is not touched here.
"""
import ast
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "test", "node-addon-api")

HEADERS = ["napi.h", "napi-inl.h", "napi-inl.deprecated.h", "LICENSE.md"]

# Test directories not copied at all.
EXCLUDED = {
    # Both build an addon with `npm install` (node-gyp, network).
    "addon_build": "builds an addon with npm install",
    "require_basic_finalizers": "builds an addon with npm install "
                                "(and runs only with NAPI_EXPERIMENTAL)",
}

# Test modules that are copied but not run, with the reason.
SKIPPED = {
    "exports": "checks the npm package's index.js (semver, package.json), "
               "not Node-API",
    "objectwrap_worker_thread": "needs worker_threads",
    "error_terminating_environment": "needs worker_threads",
}

# (relative path under test/, old, new, reason). A patch must apply exactly
# once; the sync fails otherwise, so a changed upstream file is noticed.
_NO_ASYNC_HOOKS = (
    "  if (nodeVersion >= 8) {\n",
    "  if (false) { // quickjs-napi: no async_hooks\n",
    "no async_hooks: the tests take their pre-Node.js-8 path, which runs the "
    "same native code without checking hook events")
PATCHES = [
    ("async_context.js",) + _NO_ASYNC_HOOKS,
    ("async_progress_queue_worker.js",) + _NO_ASYNC_HOOKS,
    ("async_progress_worker.js",) + _NO_ASYNC_HOOKS,
    ("async_worker.js",) + _NO_ASYNC_HOOKS,
    ("callbackscope.js",) + _NO_ASYNC_HOOKS,
    ("async_worker.js",
     "      binding.asyncworker.doWork(true, null, function (e) {\n",
     "      // quickjs-napi: {} instead of null, as in the async_hooks path\n"
     "      binding.asyncworker.doWork(true, {}, function (e) {\n",
     "the pre-async_hooks path passes a null resource, which the "
     "NODE_ADDON_API_ENABLE_TYPE_CHECK_ON_AS binding rejects with a fatal "
     "error (Object::CheckCast); Node.js never runs that path"),
    ("function_reference.js",
     "const asyncHook = require('async_hooks');\n",
     "",
     "no async_hooks"),
    ("function_reference.js",
     "  await canCallAsyncFunctionWithDifferentOverloads(binding);\n",
     "  // quickjs-napi: canCallAsyncFunctionWithDifferentOverloads checks "
     "async_hooks events.\n",
     "no async_hooks: that case only checks the hook events"),
    ("array_buffer.js",
     "      const mem = new WebAssembly.Memory({ initial: 1 });\n"
     "      binding.arraybuffer.checkDetachUpdatesData(mem.buffer, () => "
     "mem.grow(1));\n",
     "      // quickjs-napi: no WebAssembly; ArrayBuffer.prototype.transfer\n"
     "      // detaches the buffer from JavaScript as well.\n"
     "      const ab = new ArrayBuffer(65536);\n"
     "      binding.arraybuffer.checkDetachUpdatesData(ab, () => "
     "ab.transfer());\n",
     "no WebAssembly in QuickJS"),
]

# V8 error messages matched by regular expression: QuickJS throws the same
# error type with its own wording.
_V8_MESSAGES = [
    "/Cannot assign to read only property 'x' of object '#<Object>/",
    "/Cannot add property w, object is not extensible/",
    "/Cannot delete property 'x' of #<Object>/",
]


def _message_patches():
    out = []
    for msg in _V8_MESSAGES:
        out.append(("object/object_freeze_seal.js", "}, %s);" % msg,
                    "}, TypeError); // quickjs-napi: V8 message %s" % msg,
                    "V8-specific error message"))
    return out


def copy_tree(src, dst):
    for dirpath, dirnames, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        dirnames[:] = [d for d in dirnames
                       if not (rel == "." and (d in EXCLUDED or d == "build"))]
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in filenames:
            shutil.copy2(os.path.join(dirpath, f), os.path.join(dst, rel, f))


def apply_patches(test_dir, patches):
    for rel, old, new, reason in patches:
        path = os.path.join(test_dir, rel)
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except OSError as e:
            raise SystemExit("cannot read %s: %s" % (path, e))
        count = text.count(old)
        if count == 0:
            raise SystemExit("patch for %s (%s) does not apply" % (rel, reason))
        if rel == "object/object_freeze_seal.js":
            text = text.replace(old, new)  # the same check appears twice
        elif count != 1:
            raise SystemExit("patch for %s (%s) is ambiguous" % (rel, reason))
        else:
            text = text.replace(old, new)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)  # OSError: reported by main()


def test_modules(test_dir):
    """The modules upstream test/index.js runs (loadTestModules)."""
    skip_top = {"binding.cc", "binding.gyp", "build", "common",
                "child_processes", "napi_child.js", "testUtil.js",
                "thunking_manual.cc", "thunking_manual.js", "index.js"}
    out = []

    def walk(directory, pre):
        for f in sorted(os.listdir(directory)):
            if (directory == test_dir and f in skip_top) or f.startswith("."):
                continue
            full = os.path.join(directory, f)
            if os.path.isdir(full):
                if os.path.exists(os.path.join(full, "index.js")):
                    out.append(pre + f)
                else:
                    walk(full, pre + f + "/")
            elif f.endswith(".js"):
                out.append(pre + f[:-3])

    walk(test_dir, "")
    return out


def cmake_list(items):
    return " ".join('"%s"' % i for i in items)


def main():
    try:
        sync()
    except (OSError, shutil.Error) as e:
        # A failed copy or write: the tree may be partial, run the sync again.
        raise SystemExit("sync failed: %s" % e)


def sync():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    if not os.path.isfile(os.path.join(src, "napi.h")):
        raise SystemExit("not a node-addon-api checkout: %s" % src)
    try:
        with open(os.path.join(src, "package.json"), encoding="utf-8") as f:
            version = next(l.split('"')[3] for l in f
                           if l.strip().startswith('"version"'))
        with open(os.path.join(src, "test", "binding.gyp"),
                  encoding="utf-8") as f:
            gyp = ast.literal_eval(f.read())
    except (OSError, StopIteration, ValueError, SyntaxError) as e:
        raise SystemExit("cannot read the node-addon-api checkout: %s" % e)

    # Fresh copy of the vendored files; the harness stays.
    os.makedirs(DEST, exist_ok=True)
    for entry in os.listdir(DEST):
        if entry == "harness":
            continue
        path = os.path.join(DEST, entry)
        if os.path.isdir(path):
            shutil.rmtree(path)
        else:
            os.remove(path)
    for h in HEADERS:
        shutil.copy2(os.path.join(src, h), os.path.join(DEST, h))
    test_dir = os.path.join(DEST, "test")
    copy_tree(os.path.join(src, "test"), test_dir)
    patches = PATCHES + _message_patches()
    apply_patches(test_dir, patches)

    variables = gyp["target_defaults"]["variables"]
    build_sources = list(variables["build_sources"])
    # disable_deprecated is not set: upstream adds object_deprecated.cc.
    build_sources.append("object/object_deprecated.cc")
    modules = test_modules(test_dir)

    lines = [
        "# Generated by tools/sync_node_addon_api.py from node-addon-api %s."
        % version,
        "# Do not edit.",
        "set(QJSNAPI_NAA_VERSION %s)" % version,
        "set(QJSNAPI_NAA_BUILD_SOURCES %s)" % cmake_list(build_sources),
        "set(QJSNAPI_NAA_SWALLOWEXCEPT_SOURCES %s)"
        % cmake_list(variables["build_sources_swallowexcept"]),
        "set(QJSNAPI_NAA_EXCEPT_ALL_SOURCES %s)"
        % cmake_list(variables["build_sources_except_all"]),
        "set(QJSNAPI_NAA_TYPE_CHECK_SOURCES %s)"
        % cmake_list(variables["build_sources_type_check"]),
        "",
        "# qjsnapi_naa_test(<module>): one CTest per test module.",
    ]
    for m in modules:
        if m in SKIPPED:
            lines.append("# skipped: %s (%s)" % (m, SKIPPED[m]))
        else:
            lines.append("qjsnapi_naa_test(%s)" % m)
    with open(os.path.join(DEST, "tests.cmake"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    # Not "VERSION": DEST is on the include path, and on case-insensitive
    # file systems (macOS, Windows) it would shadow the C++ <version> header.
    with open(os.path.join(DEST, "NODE_ADDON_API_VERSION"), "w",
              encoding="utf-8") as f:
        f.write(version + "\n")
    print("node-addon-api %s: %d test modules, %d skipped, %d patches"
          % (version, len(modules) - len(SKIPPED), len(SKIPPED), len(patches)))


if __name__ == "__main__":
    main()
