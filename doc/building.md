# Building

quickjs-napi builds with CMake (3.24 or newer; 3.25 to use the presets) and
Ninja. It needs the QuickJS sources of `quickjs-build` and fetches libuv at
configure time.

- [Prerequisites](#prerequisites)
- [Presets](#presets)
- [Options](#options)
- [Windows](#windows)
- [What the build produces](#what-the-build-produces)
- [Continuous integration](#continuous-integration)

## Prerequisites

| Requirement | Notes |
|---|---|
| `quickjs-build` checked out **next to** this repository | `../quickjs-build`, branch `feat/quickjs-2026-06-04`; or point `QJSNAPI_QUICKJS_DIR` at it |
| CMake and Ninja | |
| A C17 compiler | gcc or clang on Linux/macOS; zig (`zig cc`) or MSYS2 MinGW on Windows. MSVC is not supported (QuickJS does not build with it) |
| A C++17 compiler | only for the C++ test addons and the node-addon-api suite |
| Network access at configure time | libuv v1.51.0 is fetched with `FetchContent`. Offline: `-DFETCHCONTENT_SOURCE_DIR_LIBUV=<libuv checkout>` |
| Node.js 22 (optional) | runs the `node_parity_*` tests, which execute the JS tests under Node.js as a reference |
| Python 3 (optional) | `tools/*.py` (stub generation check, export check, test vendoring) |

`include/quickjs.h` and `include/quickjs-libc.h` must be **identical** to the
ones in `quickjs-build`: configuration fails otherwise, because a different
header means a different `JSValue` ABI.

## Presets

The presets in `CMakePresets.json` are the reference configurations, used by
CI too. Each one builds in `build/<preset>`:

| Preset | Configuration |
|---|---|
| `unix-debug` | Linux/macOS, Debug. QuickJS's leak assertion at `JS_FreeRuntime` is active. |
| `unix-release` | Linux/macOS, Release |
| `unix-asan` | Linux/macOS, Debug + AddressSanitizer and UndefinedBehaviorSanitizer, QuickJS included |
| `unix-tsan` | Linux/macOS, Debug + ThreadSanitizer, QuickJS and libuv included |
| `windows-zig-debug`, `windows-zig-release` | Windows x64 with `zig cc` |

```sh
cmake --preset unix-debug
cmake --build --preset unix-debug
ctest --preset unix-debug
```

Without presets:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

### Debug builds and the C stack

With Clang (`zig cc` included), Debug builds compile the **QuickJS library
itself with `-O1`**: at `-O0` its interpreter loop needs about 64 KiB of C
stack per JavaScript call, and QuickJS's 1 MiB stack limit would stop at about
16 nested calls. Our own code keeps the Debug flags, and QuickJS keeps its
assertions.

## Options

| Option | Default | |
|---|---|---|
| `QJSNAPI_QUICKJS_DIR` | `../quickjs-build` | QuickJS sources, built as a subproject |
| `QJSNAPI_QUICKJS_LIBRARY` | empty | prebuilt `libqjs.a` when the sources are not available |
| `QJSNAPI_SANITIZE` | `OFF` | ASan + UBSan for everything, QuickJS included |
| `QJSNAPI_TSAN` | `OFF` | ThreadSanitizer for everything (exclusive with `QJSNAPI_SANITIZE`) |
| `QJSNAPI_BUILD_TESTS` | `ON` | test addons and the CTest suite |
| `QJSNAPI_NODE_ADDON_API_TESTS` | `ON` | the node-addon-api suite (8 C++ bindings, about 2 minutes of build) |

## Windows

QuickJS does not support Microsoft Visual C++: the Visual Studio generators
(and clang-cl) are rejected at configure time. Use a MinGW-w64 toolchain with
Ninja, as quickjs-build does.

### zig

Install the zig version in `../quickjs-build/.zigversion` (for example
`winget install zig.zig`). `zig` must be in `PATH`, or set the `ZIG`
environment variable. Use the presets, which pass the toolchain file:

```sh
cmake --preset windows-zig-debug
cmake --build --preset windows-zig-debug
ctest --preset windows-zig-debug
```

In VS Code (CMake Tools) select the configure preset *Windows x64 - zig -
Debug*. Do **not** use a "zig" compiler *kit*: a kit sets `CMAKE_AR=zig;ar`
without the toolchain file, and Ninja then runs the literal command `zig;ar`
(`error: unknown command: ;ar`). If `build/` still holds a tree configured
with a kit or a Visual Studio generator (`build/CMakeCache.txt`), delete it
once before the first configure.

### MSYS2

From a UCRT64 shell (`pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja}`),
use the generic commands above.

### Addons built with MSVC

Addons compiled with MSVC by node-gyp still load in the MinGW-built
`quickjs-napi.exe`: Node-API is a plain C ABI, and node-gyp's delay-load hook
resolves the `napi_*` imports from the running executable.

## What the build produces

| Target | What it is |
|---|---|
| `qjsnapi` | static library: the Node-API implementation and `qjsnapi.h` |
| `quickjs-napi` | the executable ([host.md](host.md)); exports every `napi_*` and `uv_*` symbol |
| `test_embed` | the C embedding test |
| test addons | `test/addons`, the Node.js conformance addons, the 8 node-addon-api bindings |

Two JavaScript sources are **embedded as C byte arrays** at build time by
`cmake/embed.cmake`: `src/buffer.js` (into the library) and
`host/bootstrap.js` (into the executable). Editing them requires a rebuild,
not a reinstall.

How the executable exports its symbols on each platform (whole archive,
`--export-dynamic`, `__declspec(dllexport)`, `-undefined dynamic_lookup` for
addons on macOS) is described in [embedding.md](embedding.md#linking).

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | Preset | Compiler |
|---|---|---|
| `linux-gcc-debug` | `unix-debug` | gcc |
| `linux-gcc-release` | `unix-release` | gcc |
| `linux-clang-debug` | `unix-debug` | clang |
| `linux-gcc-asan-ubsan` | `unix-asan` | gcc |
| `linux-gcc-tsan` | `unix-tsan` | gcc |
| `macos-arm64-debug` | `unix-debug` | Apple clang |
| `windows-x64-zig-debug` | `windows-zig-debug` | zig cc |
| `windows-x64-zig-release` | `windows-zig-release` | zig cc |

The Linux and macOS jobs without sanitizers also run the `node_parity_*`
tests under Node.js 22. Each job uploads its CTest log as an artifact.
