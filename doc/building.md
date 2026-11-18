# Building

quickjs-napi builds with CMake (3.24 or newer; 3.25 to use the presets) and
Ninja. It needs the QuickJS sources of `quickjs-build` and fetches libuv at
configure time.

- [Prerequisites](#prerequisites)
- [Presets](#presets)
- [Options](#options)
- [Windows](#windows)
- [What the build produces](#what-the-build-produces)
- [Installing](#installing)
- [Release packages](#release-packages)
- [Continuous integration](#continuous-integration)

## Prerequisites

| Requirement | Notes |
|---|---|
| `quickjs-build` checked out **next to** this repository | `../quickjs-build`, its latest release (7.0.0: QuickJS 2026-06-04); or point `QJSNAPI_QUICKJS_DIR` at it |
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
| `QJSNAPI_BUILD_SHARED` | `ON` | also build the shared library (QuickJS and libuv inside); turns on position-independent code for everything |
| `QJSNAPI_INSTALL_HOST` | `ON` | install the `quickjs-napi` executable |
| `QJSNAPI_QUICKJS_BUILD_REF` | empty | the quickjs-build tag recorded in `BUILDINFO.txt` (set by the release workflow) |

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
| `qjsnapi_shared` | shared library `libqjsnapi.so` / `libqjsnapi.dylib` / `qjsnapi.dll`: the same sources plus all of QuickJS and libuv ([embedding.md](embedding.md#from-a-release-package)) |
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

On Windows, QuickJS has no export annotations: `cmake/gen_def.cmake` reads
the symbol index of `libqjs.a` and writes a `.def` file listing its
`JS_*`, `__JS_*` and `js_*` symbols for `qjsnapi.dll` (the zig linker
ignores `--export-all-symbols`).

## Installing

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DQJSNAPI_BUILD_TESTS=OFF       -DCMAKE_INSTALL_PREFIX=/opt/quickjs-napi
cmake --build build
cmake --install build
```

| Path | Content |
|---|---|
| `bin/` | `quickjs-napi`; `qjsnapi.dll` on Windows |
| `include/` | `node_api.h`, `node_api_types.h`, `js_native_api.h`, `js_native_api_types.h`, `qjsnapi.h`, `quickjs.h`, `quickjs-libc.h`, `uv.h`, `uv/*.h` |
| `lib/` | `libqjsnapi.a`, `libqjs.a`, `libuv.a` (three separate archives); `libqjsnapi.so` / `.dylib`; on Windows the import libraries `libqjsnapi.dll.a` (the DLL) and `libquickjs-napi.dll.a` (the executable, for MinGW addons) |
| `lib/cmake/qjsnapi/` | `find_package(qjsnapi)`, targets in [embedding.md](embedding.md#from-a-release-package) |
| `share/doc/quickjs-napi/` | `LICENSE`, `licenses/` (QuickJS, quickjs-build, libuv), `BUILDINFO.txt` (version, target, compiler, QuickJS and libuv versions) |

`test/package/smoke.sh <prefix>` checks an installed tree: it builds
`test/package` with `find_package`, runs the embedding test linked with the
static archives and with the shared library, a script, and the addons of
`test/js/test_hello.js` built with the installed headers only.

## Release packages

`.github/workflows/release.yml` runs when a GitHub release is **published**
(or by hand, `workflow_dispatch`, which does everything but the upload):

1. **prepare**: reads `project(quickjs-napi VERSION x.y.z)` from
   `CMakeLists.txt` and fails if the release tag (`x.y.z` or `vx.y.z`)
   differs; picks the latest release of quickjs-build (or the
   `quickjs_build_ref` input).
2. **ci**: the whole CI suite (`ci.yml`) on that quickjs-build.
3. **build**: one package per platform, Release, tests off, installed and
   zipped as `quickjs-napi-<version>-<platform>.zip` (the layout of
   [Installing](#installing)).
4. **smoke**: `test/package/smoke.sh` on each package, on a machine or
   emulator of its platform.
5. **publish**: only if everything above passed, uploads the zip files and
   `SHA256SUMS` to the release.

| Package | Built with | Smoke test |
|---|---|---|
| `linux-x86_64` | zig, glibc 2.17 | ubuntu, gcc |
| `linux-x86` | zig, glibc 2.17 | ubuntu (32-bit), zig |
| `linux-arm64` | zig, glibc 2.17 | ubuntu arm64, gcc |
| `linux-armv7` | zig, glibc 2.17 | qemu-user |
| `linux-riscv64` | zig, glibc 2.27 | qemu-user |
| `linux-x86_64-musl`, `linux-arm64-musl` | zig, musl | Alpine container, gcc |
| `windows-x86_64`, `windows-x86` | zig, MinGW-w64 UCRT | windows-latest, zig |
| `windows-arm64` | zig, MinGW-w64 UCRT | windows-11-arm, zig |
| `macos-arm64`, `macos-x86_64` | Apple clang, macOS 11+ | native runners |
| `freebsd-x86_64` | zig | compiled and linked only (no hosted FreeBSD runner) |

**musl.** zig links musl executables statically by default, and a static
musl executable cannot `dlopen()` addons. The executable is therefore linked
**dynamically** against the system musl (`/lib/ld-musl-<arch>.so.1`, as on
Alpine); the libraries are the same as for the other platforms. If a musl
smoke test fails because of the executable, set `-DQJSNAPI_INSTALL_HOST=OFF`
for that package in `release.yml`: it then holds the headers and libraries
only (`find_package` reports `QJSNAPI_HAS_HOST` false), for embedders.

**Not packaged:** WebAssembly (no `dlopen`, and libuv does not support WASI),
builds with MSVC (QuickJS does not compile with it; MSVC addons still load in
`quickjs-napi.exe`), Android and iOS.

To release: bump `VERSION` in `CMakeLists.txt`, merge, then publish a
release whose tag is that version.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request, and from the
release workflow (`workflow_call`). It builds against the latest
quickjs-build release, or the `quickjs_build_ref` input:

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
