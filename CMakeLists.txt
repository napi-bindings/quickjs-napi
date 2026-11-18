cmake_minimum_required(VERSION 3.24)

# The release workflow checks that the tag of a release matches this version.
project(quickjs-napi VERSION 0.0.2 LANGUAGES C)

# ------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------
option(QJSNAPI_SANITIZE
       "Build everything (QuickJS included) with AddressSanitizer and UBSan" OFF)
option(QJSNAPI_TSAN
       "Build everything (QuickJS and libuv included) with ThreadSanitizer" OFF)
option(QJSNAPI_BUILD_TESTS "Build the test addons and the test suite" ON)
option(QJSNAPI_NODE_ADDON_API_TESTS
       "Build and run the node-addon-api test suite (C++, 8 bindings)" ON)
option(QJSNAPI_BUILD_SHARED
       "Also build libqjsnapi as a shared library, QuickJS and libuv included"
       ON)
# Recorded in BUILDINFO.txt of the installed tree (set by the release workflow).
set(QJSNAPI_QUICKJS_BUILD_REF
    ""
    CACHE STRING "Tag or commit of quickjs-build, for BUILDINFO.txt")

# The shared library contains QuickJS and libuv: every object must be
# position-independent, theirs included.
if(QJSNAPI_BUILD_SHARED)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(QJSNAPI_QUICKJS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-build"
    CACHE PATH "Source tree of quickjs-build (QuickJS 2026-06-04)")
set(QJSNAPI_QUICKJS_LIBRARY
    ""
    CACHE FILEPATH
          "Prebuilt libqjs.a, used only when QJSNAPI_QUICKJS_DIR has no sources")

# QuickJS uses GNU C extensions and quickjs-libc relies on MinGW headers:
# Microsoft Visual C++ (and clang-cl) cannot build it. Fail early with the
# supported alternatives instead of an error deep inside quickjs.c.
if(MSVC)
  message(
    FATAL_ERROR
      "QuickJS cannot be compiled with Microsoft Visual C++ (or clang-cl).\n"
      "On Windows use a MinGW-w64 toolchain with Ninja, for example:\n"
      "  zig:   cmake -S . -B build -G Ninja "
      "-DCMAKE_TOOLCHAIN_FILE=../quickjs-build/toolchains/zig.cmake "
      "-DZIG_TARGET=x86_64-windows-gnu\n"
      "  MSYS2: from a UCRT64 shell, cmake -S . -B build -G Ninja\n"
      "Addons built with MSVC (node-gyp) still load: Node-API is a C ABI.")
endif()

# Sanitizers are global so that QuickJS itself is instrumented too: a leak or
# overflow inside the engine caused by a wrong JSValue lifetime must be
# reported at its origin.
if(QJSNAPI_SANITIZE)
  if(MSVC)
    add_compile_options(/fsanitize=address)
  else()
    set(_qjsnapi_san -fsanitize=address,undefined -fno-omit-frame-pointer
                     -fno-sanitize-recover=undefined)
    add_compile_options(${_qjsnapi_san})
    add_link_options(${_qjsnapi_san})
  endif()
endif()
if(QJSNAPI_TSAN)
  if(QJSNAPI_SANITIZE)
    message(FATAL_ERROR "QJSNAPI_TSAN and QJSNAPI_SANITIZE are exclusive")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

# ------------------------------------------------------------------------------
# libuv
# ------------------------------------------------------------------------------
# The event loop of Node-API (napi_get_uv_event_loop, async work, thread-safe
# functions), linked statically like in Node.js. Fetched at configure time;
# to build offline point FETCHCONTENT_SOURCE_DIR_LIBUV at a libuv checkout of
# the same tag.
include(FetchContent)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(QJSNAPI_LIBUV_TAG v1.51.0)
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG ${QJSNAPI_LIBUV_TAG}
  GIT_SHALLOW TRUE)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD) # FetchContent_Populate, for EXCLUDE_FROM_ALL
endif()
FetchContent_GetProperties(libuv)
if(NOT libuv_POPULATED)
  FetchContent_Populate(libuv)
  add_subdirectory("${libuv_SOURCE_DIR}" "${libuv_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()
# Addons call uv_* too (node-gyp gives them uv.h): the executable exports
# libuv like Node-API. On Windows that needs __declspec(dllexport) on the
# definitions, which uv.h emits for BUILDING_UV_SHARED.
if(WIN32)
  target_compile_definitions(uv_a PRIVATE BUILDING_UV_SHARED=1)
  # MinGW headers (zig cc) define _WIN32_WINNT before libuv's own value.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(uv_a PRIVATE -Wno-macro-redefined)
  endif()
endif()

# ------------------------------------------------------------------------------
# QuickJS
# ------------------------------------------------------------------------------
if(EXISTS "${QJSNAPI_QUICKJS_DIR}/CMakeLists.txt")
  # The vendored headers in include/ must match the engine being linked:
  # a different quickjs.h means a different JSValue ABI.
  foreach(_hdr quickjs.h quickjs-libc.h)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/include/${_hdr}" _ours)
    file(SHA256 "${QJSNAPI_QUICKJS_DIR}/include/${_hdr}" _theirs)
    if(NOT _ours STREQUAL _theirs)
      message(
        FATAL_ERROR
          "include/${_hdr} differs from ${QJSNAPI_QUICKJS_DIR}/include/${_hdr}.\n"
          "Copy the header from quickjs-build so both sides agree on the "
          "QuickJS ABI.")
    endif()
  endforeach()
  set(QJS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(QJS_BUILD_LIBC ON CACHE BOOL "" FORCE)
  add_subdirectory("${QJSNAPI_QUICKJS_DIR}" quickjs EXCLUDE_FROM_ALL)
  # QuickJS's cutils.h falls back to `#define offsetof(t, f) ((size_t)
  # &((t *)0)->f)` when <stddef.h> was not included before it. With glibc
  # some earlier header always pulls it in; with the MinGW-w64 headers none
  # does, so container_of() becomes a member access through a null pointer:
  # undefined behaviour that `zig cc` traps at the first JS_NewRuntime() in
  # Debug builds (UBSan is on by default without -O). The sources are kept
  # unmodified, so force the compiler's offsetof in instead.
  if(WIN32 AND NOT MSVC)
    target_compile_options(qjs PRIVATE "SHELL:-include stddef.h")
  endif()
  # The interpreter loop (JS_CallInternal) is one huge function. At -O0
  # Clang, `zig cc` included, gives each of its locals a separate stack slot:
  # one JS call then takes ~64 KiB of C stack and the default 1 MiB QuickJS
  # stack limit allows ~16 nested calls. -O1 brings a frame back to a few
  # KiB; assertions (the leak check of JS_FreeRuntime) stay enabled. Our own
  # code keeps the Debug flags.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(qjs PRIVATE $<$<CONFIG:Debug>:-O1>)
  endif()
elseif(QJSNAPI_QUICKJS_LIBRARY)
  add_library(qjs STATIC IMPORTED)
  set_target_properties(qjs PROPERTIES IMPORTED_LOCATION
                                       "${QJSNAPI_QUICKJS_LIBRARY}")
  if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(qjs INTERFACE m Threads::Threads ${CMAKE_DL_LIBS})
  elseif(WIN32)
    target_link_libraries(qjs INTERFACE m pthread)
  endif()
else()
  message(
    FATAL_ERROR
      "QuickJS not found: set QJSNAPI_QUICKJS_DIR to the quickjs-build "
      "sources or QJSNAPI_QUICKJS_LIBRARY to a prebuilt libqjs.a")
endif()

# ------------------------------------------------------------------------------
# libqjsnapi: the Node-API implementation
# ------------------------------------------------------------------------------
set(QJSNAPI_SOURCES
    src/async.c
    src/env.c
    src/error.c
    src/function.c
    src/buffer.c
    src/builtins.c
    src/lifetime.c
    src/loader.c
    src/loop.c
    src/node_api.c
    src/object.c
    src/scope.c
    src/string.c
    src/tsfn.c
    src/value.c
    src/stubs_generated.c)

# JavaScript sources embedded into the library and the host as C byte arrays.
set(QJSNAPI_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
function(qjsnapi_embed input name)
  add_custom_command(
    OUTPUT "${QJSNAPI_GENERATED_DIR}/${name}.h"
    COMMAND
      "${CMAKE_COMMAND}" -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/${input}
      -DOUTPUT=${QJSNAPI_GENERATED_DIR}/${name}.h -DNAME=qn_${name} -P
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
    DEPENDS ${input} cmake/embed.cmake
    COMMENT "Embedding ${input}"
    VERBATIM)
endfunction()
qjsnapi_embed(src/buffer.js buffer_js)
qjsnapi_embed(host/bootstrap.js bootstrap_js)

add_library(qjsnapi STATIC ${QJSNAPI_SOURCES}
                           "${QJSNAPI_GENERATED_DIR}/buffer_js.h")
set_target_properties(
  qjsnapi
  PROPERTIES C_STANDARD 17
             C_STANDARD_REQUIRED ON
             C_VISIBILITY_PRESET hidden
             POSITION_INDEPENDENT_CODE ON)
# The implementation is compiled against Node-API 10. Addons pick their own
# NAPI_VERSION; it is not propagated to consumers.
target_compile_definitions(qjsnapi PRIVATE NAPI_VERSION=10)
target_include_directories(qjsnapi PUBLIC include PRIVATE src
                                                          "${QJSNAPI_GENERATED_DIR}")
target_link_libraries(qjsnapi PUBLIC qjs uv_a ${CMAKE_DL_LIBS})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(qjsnapi PRIVATE -Wall -Wextra -Wno-unused-parameter
                                         -Wmissing-prototypes)
endif()

# ------------------------------------------------------------------------------
# Host executable
# ------------------------------------------------------------------------------
# Addons resolve napi_* and uv_* from the process, as from node: every
# Node-API function and all of libuv must be exported by the executable.
#   - WHOLE_ARCHIVE keeps the objects of functions the host never calls.
#   - Linux/BSD: ENABLE_EXPORTS adds --export-dynamic.
#   - Windows: the definitions carry __declspec(dllexport) (NAPI_EXTERN), and
#     ENABLE_EXPORTS produces the import library used by the test addons.
#     Addons built by node-gyp import from "node.exe" and are redirected to
#     the current process by node-gyp's win_delay_load_hook.
#   - macOS: addons are linked with -undefined dynamic_lookup.
# CMake's built-in WHOLE_ARCHIVE feature emits --push-state/--pop-state for
# GNU-style linkers, which lld in MinGW mode (zig cc) rejects: define the
# feature explicitly for each linker family.
if(MSVC)
  set(CMAKE_C_LINK_LIBRARY_USING_QJSNAPI_WHOLE_ARCHIVE "/WHOLEARCHIVE:<LIBRARY>")
elseif(APPLE)
  set(CMAKE_C_LINK_LIBRARY_USING_QJSNAPI_WHOLE_ARCHIVE
      "LINKER:-force_load,<LIB_ITEM>")
else()
  set(CMAKE_C_LINK_LIBRARY_USING_QJSNAPI_WHOLE_ARCHIVE
      "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
endif()
set(CMAKE_C_LINK_LIBRARY_USING_QJSNAPI_WHOLE_ARCHIVE_SUPPORTED TRUE)

add_executable(quickjs-napi host/main.c host/spawn.c
                            "${QJSNAPI_GENERATED_DIR}/bootstrap_js.h")
target_include_directories(quickjs-napi PRIVATE "${QJSNAPI_GENERATED_DIR}")
set_target_properties(quickjs-napi PROPERTIES C_STANDARD 17 ENABLE_EXPORTS ON)
target_link_libraries(
  quickjs-napi PRIVATE "$<LINK_LIBRARY:QJSNAPI_WHOLE_ARCHIVE,qjsnapi>")
# uv_a comes in through qjsnapi's link interface: whole archive as well.
set_target_properties(quickjs-napi PROPERTIES LINK_LIBRARY_OVERRIDE_uv_a
                                              QJSNAPI_WHOLE_ARCHIVE)
if(WIN32 AND NOT MSVC)
  # static libgcc / libwinpthread, as for the quickjs-build tools
  target_link_options(quickjs-napi PRIVATE -static)
endif()
# zig links musl executables statically by default, and a static musl
# executable cannot dlopen(): it could not load any addon. -dynamic links
# against the system's musl (/lib/ld-musl-<arch>.so.1, as on Alpine).
if(DEFINED ZIG_TARGET AND ZIG_TARGET MATCHES "-musl")
  target_link_options(quickjs-napi PRIVATE -dynamic)
endif()

# ------------------------------------------------------------------------------
# Shared library: libqjsnapi.so / libqjsnapi.dylib / qjsnapi.dll
# ------------------------------------------------------------------------------
# The same sources as the static library, linked with all of QuickJS and libuv
# (whole archive). An embedder must create its JSRuntime and JSContext with
# the QuickJS inside it: two copies of QuickJS in one process do not share
# their class IDs and atoms. So the library exports, besides napi_*, uv_* and
# qjsnapi_*, the QuickJS API (JS_*):
#   - Linux/BSD/macOS: QuickJS is compiled with default visibility, libuv and
#     our functions mark their API visible (UV_EXTERN, NAPI_EXTERN,
#     QJSNAPI_EXTERN); everything else of ours is hidden.
#   - Windows: napi_*, uv_* and qjsnapi_* carry __declspec(dllexport);
#     QuickJS has no export annotation, so a .def file lists the JS_*, __JS_*
#     and js_* symbols defined by libqjs.a, read from its symbol index
#     (cmake/gen_def.cmake).
#     zig's linker ignores --export-all-symbols.
if(QJSNAPI_BUILD_SHARED)
  set(_qjsnapi_def)
  if(WIN32)
    set(_qjsnapi_def "${QJSNAPI_GENERATED_DIR}/qjsnapi_quickjs.def")
    set(_qjsnapi_strip OFF)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(_qjsnapi_strip ON) # x86: C names are decorated with "_"
    endif()
    add_custom_command(
      OUTPUT "${_qjsnapi_def}"
      COMMAND "${CMAKE_COMMAND}" "-DLIB=$<TARGET_FILE:qjs>"
              "-DSTRIP_UNDERSCORE=${_qjsnapi_strip}"
              "-DOUTPUT=${_qjsnapi_def}"
              # __JS_*: called by the inline functions of quickjs.h
              "-DREGEX=^(__)?(JS|js)_[A-Za-z0-9_]+$" -P
              "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_def.cmake"
      DEPENDS "$<TARGET_FILE:qjs>" cmake/gen_def.cmake
      COMMENT "Listing the QuickJS API exported by qjsnapi.dll"
      VERBATIM)
  endif()
  add_library(qjsnapi_shared SHARED ${QJSNAPI_SOURCES}
                                    "${QJSNAPI_GENERATED_DIR}/buffer_js.h"
                                    ${_qjsnapi_def})
  set_target_properties(
    qjsnapi_shared
    PROPERTIES OUTPUT_NAME qjsnapi
               C_STANDARD 17
               C_STANDARD_REQUIRED ON
               C_VISIBILITY_PRESET hidden)
  target_compile_definitions(qjsnapi_shared PRIVATE NAPI_VERSION=10)
  target_include_directories(
    qjsnapi_shared PUBLIC include PRIVATE src "${QJSNAPI_GENERATED_DIR}")
  target_link_libraries(
    qjsnapi_shared PRIVATE "$<LINK_LIBRARY:QJSNAPI_WHOLE_ARCHIVE,qjs,uv_a>"
                           ${CMAKE_DL_LIBS})
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qjsnapi_shared PRIVATE -Wall -Wextra
                                                  -Wno-unused-parameter
                                                  -Wmissing-prototypes)
  endif()
  if(WIN32 AND NOT MSVC)
    # qjsnapi.dll and libqjsnapi.dll.a, next to the static libqjsnapi.a
    set_target_properties(qjsnapi_shared PROPERTIES PREFIX "")
  endif()
endif()

# ------------------------------------------------------------------------------
# Installation (the layout of the release packages, see doc/building.md)
# ------------------------------------------------------------------------------
#   bin/      quickjs-napi, qjsnapi.dll (Windows)
#   include/  the Node-API headers, qjsnapi.h, quickjs.h, quickjs-libc.h, uv.h
#   lib/      libqjsnapi.a, libqjs.a, libuv.a: three archives, so that an
#             embedder can link its own compatible QuickJS or libuv instead;
#             the shared library; the import libraries (Windows)
#   lib/cmake/qjsnapi/          find_package(qjsnapi)
#   share/doc/quickjs-napi/     licenses and BUILDINFO.txt
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(QJSNAPI_INSTALL_HOST "Install the quickjs-napi executable" ON)

if(QJSNAPI_INSTALL_HOST)
  # ARCHIVE: libquickjs-napi.dll.a on Windows, the import library that
  # addons built with MinGW link against (the test addons do the same).
  install(TARGETS quickjs-napi RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                               ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(TARGETS qjsnapi ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(QJSNAPI_BUILD_SHARED)
  install(TARGETS qjsnapi_shared RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                                 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                                 ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(FILES "$<TARGET_FILE:qjs>" DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RENAME libqjs.a)
install(FILES "$<TARGET_FILE:uv_a>" DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RENAME libuv.a)

install(
  FILES include/js_native_api.h
        include/js_native_api_types.h
        include/node_api.h
        include/node_api_types.h
        include/qjsnapi.h
        include/quickjs.h
        include/quickjs-libc.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# uv.h is part of the addon API (node-gyp puts it on the include path too).
install(DIRECTORY "${libuv_SOURCE_DIR}/include/"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Licenses: ours, QuickJS (MIT), quickjs-build (Apache-2.0), libuv (MIT and
# the licenses of the code it includes).
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
set(_qjsnapi_licenses "${libuv_SOURCE_DIR}/LICENSE" libuv.txt
                      "${libuv_SOURCE_DIR}/LICENSE-extra" libuv-extra.txt)
if(EXISTS "${QJSNAPI_QUICKJS_DIR}/LICENSE.quickjs")
  list(APPEND _qjsnapi_licenses "${QJSNAPI_QUICKJS_DIR}/LICENSE.quickjs"
       QuickJS.txt "${QJSNAPI_QUICKJS_DIR}/LICENSE" quickjs-build.txt)
endif()
while(_qjsnapi_licenses)
  list(POP_FRONT _qjsnapi_licenses _src _name)
  install(FILES "${_src}" DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
          RENAME ${_name})
endwhile()

# What the package was built from.
if(DEFINED ZIG_TARGET)
  set(_qjsnapi_target "${ZIG_TARGET}")
else()
  set(_qjsnapi_target "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(_qjsnapi_quickjs "prebuilt ${QJSNAPI_QUICKJS_LIBRARY}")
if(EXISTS "${QJSNAPI_QUICKJS_DIR}/VERSION")
  file(STRINGS "${QJSNAPI_QUICKJS_DIR}/VERSION" _qjsnapi_quickjs LIMIT_COUNT 1)
endif()
if(QJSNAPI_QUICKJS_BUILD_REF)
  string(APPEND _qjsnapi_quickjs " (quickjs-build ${QJSNAPI_QUICKJS_BUILD_REF})")
endif()
file(
  WRITE "${CMAKE_CURRENT_BINARY_DIR}/BUILDINFO.txt"
  "quickjs-napi ${PROJECT_VERSION}\n"
  "Node-API:   10 (Node.js 22 behaviour)\n"
  "Target:     ${_qjsnapi_target}\n"
  "Compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\n"
  "Build type: ${CMAKE_BUILD_TYPE}\n"
  "QuickJS:    ${_qjsnapi_quickjs}\n"
  "libuv:      ${QJSNAPI_LIBUV_TAG}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/BUILDINFO.txt"
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# find_package(qjsnapi): the system libraries of each archive, as linked here.
function(qjsnapi_plain_link_libraries target out)
  get_target_property(_libs ${target} INTERFACE_LINK_LIBRARIES)
  set(_plain)
  foreach(_lib IN LISTS _libs)
    if(_lib MATCHES "^\\$<LINK_ONLY:(.+)>$")
      set(_lib "${CMAKE_MATCH_1}")
    endif()
    if(_lib AND NOT _lib MATCHES "\\$<")
      list(APPEND _plain "${_lib}")
    endif()
  endforeach()
  set(${out} "${_plain}" PARENT_SCOPE)
endfunction()
qjsnapi_plain_link_libraries(qjs QJSNAPI_PKG_QJS_LIBS)
qjsnapi_plain_link_libraries(uv_a QJSNAPI_PKG_UV_LIBS)
set(QJSNAPI_PKG_CORE_LIBS "${CMAKE_DL_LIBS}")
set(QJSNAPI_PKG_HOST ${QJSNAPI_INSTALL_HOST})
set(QJSNAPI_PKG_SHARED ${QJSNAPI_BUILD_SHARED})
if(WIN32)
  set(QJSNAPI_PKG_SHARED_FILE "${CMAKE_INSTALL_BINDIR}/qjsnapi.dll")
  set(QJSNAPI_PKG_SHARED_IMPLIB "${CMAKE_INSTALL_LIBDIR}/libqjsnapi.dll.a")
  set(QJSNAPI_PKG_HOST_IMPLIB "${CMAKE_INSTALL_LIBDIR}/libquickjs-napi.dll.a")
elseif(APPLE)
  set(QJSNAPI_PKG_SHARED_FILE "${CMAKE_INSTALL_LIBDIR}/libqjsnapi.dylib")
else()
  set(QJSNAPI_PKG_SHARED_FILE "${CMAKE_INSTALL_LIBDIR}/libqjsnapi.so")
endif()
set(QJSNAPI_PKG_HOST_FILE
    "${CMAKE_INSTALL_BINDIR}/quickjs-napi${CMAKE_EXECUTABLE_SUFFIX}")
set(QJSNAPI_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/qjsnapi")
configure_package_config_file(
  cmake/qjsnapiConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/qjsnapiConfig.cmake"
  INSTALL_DESTINATION ${QJSNAPI_CMAKE_DIR}
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_BINDIR)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/qjsnapiConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qjsnapiConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/qjsnapiConfigVersion.cmake"
        DESTINATION ${QJSNAPI_CMAKE_DIR})

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
if(QJSNAPI_BUILD_TESTS)
  # Some Node.js tests are C++ (6_object_wrap, 7_factory_wrap, ...). The zig
  # toolchain of quickjs-build only defines the C compiler: use `zig c++`
  # with the same target.
  if(DEFINED ZIG AND DEFINED ZIG_TARGET AND NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${ZIG}" c++)
    set(CMAKE_CXX_COMPILER_TARGET "${ZIG_TARGET}")
    set(CMAKE_CXX_FLAGS_RELEASE_INIT "-g0")
    set(CMAKE_CXX_FLAGS_MINSIZEREL_INIT "-g0")
  endif()
  enable_language(CXX)
  enable_testing()
  set(QJSNAPI_ADDON_DIR "${CMAKE_CURRENT_BINARY_DIR}/addons")
  add_custom_target(quickjs-napi-tests ALL)

  # Test addons are built like node-gyp builds them: they include the
  # Node-API headers only and do NOT link against libqjsnapi.
  function(qjsnapi_test_addon name source)
    add_library(${name} MODULE ${source})
    set_target_properties(
      ${name}
      PROPERTIES PREFIX ""
                 SUFFIX ".node"
                 C_STANDARD 11
                 C_VISIBILITY_PRESET hidden
                 LIBRARY_OUTPUT_DIRECTORY "${QJSNAPI_ADDON_DIR}"
                 RUNTIME_OUTPUT_DIRECTORY "${QJSNAPI_ADDON_DIR}")
    target_include_directories(${name} PRIVATE include "${libuv_SOURCE_DIR}/include")
    target_compile_definitions(${name} PRIVATE BUILDING_NODE_EXTENSION
                                               NAPI_VERSION=10)
    if(WIN32)
      target_link_libraries(${name} PRIVATE quickjs-napi)
    elseif(APPLE)
      target_link_options(${name} PRIVATE -undefined dynamic_lookup)
    endif()
    add_dependencies(quickjs-napi-tests ${name})
  endfunction()

  qjsnapi_test_addon(hello test/addons/hello/hello.c)
  qjsnapi_test_addon(legacy test/addons/loader/legacy.c)
  qjsnapi_test_addon(fnexport test/addons/loader/fnexport.c)
  qjsnapi_test_addon(future test/addons/loader/future.c)
  qjsnapi_test_addon(values test/addons/values/values.c)
  qjsnapi_test_addon(binary test/addons/binary/binary.c)
  qjsnapi_test_addon(node_api test/addons/node_api/node_api.c)
  add_custom_command(
    TARGET node_api POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -DINPUT=$<TARGET_FILE:node_api>
            -DDIR=${QJSNAPI_ADDON_DIR} -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_addon_percent.cmake"
    VERBATIM)

  add_test(NAME js_hello
           COMMAND quickjs-napi
                   "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_hello.js"
                   "${QJSNAPI_ADDON_DIR}")

  # Node.js js-native-api conformance suite (vendored by
  # tools/sync_node_tests.py). Each test runs from a copy of its directory in
  # the build tree, so that `require('./build/Release/<addon>')` and
  # `require('../../common')` resolve exactly as in the Node.js repository.
  set(QJSNAPI_NODE_TESTS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/test/node")
  set(QJSNAPI_NODE_TESTS_BIN "${CMAKE_CURRENT_BINARY_DIR}/node-tests")
  foreach(_f index.js assert.js gc.js tick.js require-as-module.js)
    configure_file("${QJSNAPI_NODE_TESTS_SRC}/common/${_f}"
                   "${QJSNAPI_NODE_TESTS_BIN}/common/${_f}" COPYONLY)
  endforeach()

  # <suite> is js-native-api or node-api (test/node/<suite>/<dir>).
  function(qjsnapi_node_test suite dir)
    cmake_parse_arguments(T "" "TARGET" "SOURCES;DEFINES;SCRIPTS" ${ARGN})
    set(_src "${QJSNAPI_NODE_TESTS_SRC}/${suite}/${dir}")
    set(_bin "${QJSNAPI_NODE_TESTS_BIN}/${suite}/${dir}")
    string(REPLACE "-" "_" _suite_id "${suite}")
    set(_lib "node_${_suite_id}_${dir}_${T_TARGET}")
    list(TRANSFORM T_SOURCES PREPEND "${_src}/")
    add_library(${_lib} MODULE ${T_SOURCES})
    set_target_properties(
      ${_lib}
      PROPERTIES OUTPUT_NAME "${T_TARGET}"
                 PREFIX ""
                 SUFFIX ".node"
                 C_VISIBILITY_PRESET hidden
                 CXX_VISIBILITY_PRESET hidden
                 CXX_STANDARD 17
                 LIBRARY_OUTPUT_DIRECTORY "${_bin}/build/Release"
                 RUNTIME_OUTPUT_DIRECTORY "${_bin}/build/Release")
    target_include_directories(${_lib} PRIVATE include "${libuv_SOURCE_DIR}/include")
    target_compile_definitions(
      ${_lib} PRIVATE BUILDING_NODE_EXTENSION
                      NODE_GYP_MODULE_NAME=${T_TARGET} ${T_DEFINES})
    # node-gyp does not define NDEBUG in Release, and the tests rely on it:
    # test_uv_loop calls its callback inside assert().
    target_compile_options(${_lib} PRIVATE -UNDEBUG)
    if(WIN32)
      target_link_libraries(${_lib} PRIVATE quickjs-napi)
    elseif(APPLE)
      target_link_options(${_lib} PRIVATE -undefined dynamic_lookup)
    endif()
    add_dependencies(quickjs-napi-tests ${_lib})
    foreach(_script ${T_SCRIPTS})
      configure_file("${_src}/${_script}" "${_bin}/${_script}" COPYONLY)
      add_test(NAME node/${suite}/${dir}/${_script}
               COMMAND quickjs-napi
                       ${QJSNAPI_NODE_FLAGS_${suite}_${dir}_${_script}}
                       "${_bin}/${_script}"
               WORKING_DIRECTORY "${_bin}")
    endforeach()
  endfunction()

  # The supported options of a script's "// Flags:" line.
  function(qjsnapi_node_flags suite dir script)
    set(QJSNAPI_NODE_FLAGS_${suite}_${dir}_${script} ${ARGN} PARENT_SCOPE)
  endfunction()

  # A script that Node.js runs from a child process of itself (spawnSync,
  # fork): the child mode runs directly. REQUIRE loads the script with
  # require(), as the parent does; ABORT expects an abnormal termination
  # (cmake/expect_abort.cmake); PASS / FAIL check the output, otherwise the
  # exit code decides. Named <script>:child when the script also runs as a
  # normal test; NAME replaces <script> in the test name.
  function(qjsnapi_node_child_run suite dir script)
    cmake_parse_arguments(T "REQUIRE;ABORT" "NAME" "ARGS;PASS;FAIL" ${ARGN})
    set(_bin "${QJSNAPI_NODE_TESTS_BIN}/${suite}/${dir}")
    configure_file("${QJSNAPI_NODE_TESTS_SRC}/${suite}/${dir}/${script}"
                   "${_bin}/${script}" COPYONLY)
    set(_flags ${QJSNAPI_NODE_FLAGS_${suite}_${dir}_${script}})
    if(T_REQUIRE)
      set(_args ${_flags} "${QJSNAPI_NODE_TESTS_BIN}/common/require-as-module.js"
                "${_bin}/${script}" ${T_ARGS})
    else()
      set(_args ${_flags} "${_bin}/${script}" ${T_ARGS})
    endif()
    if(T_ABORT)
      list(JOIN _args "|" _joined)
      set(_cmd "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:quickjs-napi>"
               "-DARGS=${_joined}" -P
               "${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_abort.cmake")
    else()
      set(_cmd quickjs-napi ${_args})
    endif()
    if(T_NAME)
      set(_name node/${suite}/${dir}/${T_NAME})
    else()
      set(_name node/${suite}/${dir}/${script})
      if(TEST ${_name})
        set(_name "${_name}:child")
      endif()
    endif()
    add_test(NAME ${_name} COMMAND ${_cmd} WORKING_DIRECTORY "${_bin}")
    if(T_PASS)
      set_tests_properties(${_name} PROPERTIES
                           PASS_REGULAR_EXPRESSION "${T_PASS}")
    endif()
    if(T_FAIL)
      set_tests_properties(${_name} PROPERTIES
                           FAIL_REGULAR_EXPRESSION "${T_FAIL}")
    endif()
  endfunction()

  # An addon loaded alone in a fresh process (Node: spawnSync with
  # `-e require(addon)`); the output must match PASS.
  function(qjsnapi_node_addon_run suite dir target)
    cmake_parse_arguments(T "" "" "PASS" ${ARGN})
    set(_bin "${QJSNAPI_NODE_TESTS_BIN}/${suite}/${dir}")
    string(REPLACE "-" "_" _suite_id "${suite}")
    set(_name node/${suite}/${dir}/${target}.node)
    add_test(NAME ${_name}
             COMMAND quickjs-napi
                     "${QJSNAPI_NODE_TESTS_BIN}/common/require-as-module.js"
                     "$<TARGET_FILE:node_${_suite_id}_${dir}_${target}>"
             WORKING_DIRECTORY "${_bin}")
    set_tests_properties(${_name} PROPERTIES PASS_REGULAR_EXPRESSION "${T_PASS}")
  endfunction()

  include("${QJSNAPI_NODE_TESTS_SRC}/js-native-api/tests.cmake")
  include("${QJSNAPI_NODE_TESTS_SRC}/node-api/tests.cmake")

  if(QJSNAPI_NODE_ADDON_API_TESTS)
    include(cmake/node_addon_api_tests.cmake)
  endif()

  add_test(NAME js_values
           COMMAND quickjs-napi
                   "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_values.js"
                   "${QJSNAPI_ADDON_DIR}")
  add_test(NAME js_binary
           COMMAND quickjs-napi
                   "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_binary.js"
                   "${QJSNAPI_ADDON_DIR}")
  add_test(NAME js_node_api
           COMMAND quickjs-napi
                   "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_node_api.js"
                   "${QJSNAPI_ADDON_DIR}")
  # Host features of the executable (child_process, -e, -r, NODE_OPTIONS).
  add_test(NAME js_host
           COMMAND quickjs-napi "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_host.js")
  set_tests_properties(js_host PROPERTIES PASS_REGULAR_EXPRESSION "^ok")

  add_executable(test_embed test/unit/test_embed.c)
  set_target_properties(test_embed PROPERTIES C_STANDARD 17)
  target_link_libraries(test_embed PRIVATE qjsnapi)
  target_compile_definitions(test_embed PRIVATE NAPI_VERSION=10)
  add_test(NAME unit_embed COMMAND test_embed)

  # Reference run: the same addons and script under Node.js. Skipped with
  # sanitizers: ASan-instrumented addons cannot be loaded by a regular node.
  find_program(QJSNAPI_NODE node)
  if(QJSNAPI_NODE AND NOT CMAKE_CROSSCOMPILING AND NOT QJSNAPI_SANITIZE
     AND NOT QJSNAPI_TSAN)
    foreach(_t hello values binary node_api)
      add_test(NAME node_parity_${_t}
               COMMAND "${QJSNAPI_NODE}" --expose-gc
                       "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_${_t}.js"
                       "${QJSNAPI_ADDON_DIR}")
    endforeach()
    add_test(NAME node_parity_host
             COMMAND "${QJSNAPI_NODE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_host.js")
    # The NODE_OPTIONS of the environment must not reach Node.js itself.
    set_tests_properties(node_parity_host PROPERTIES
                         PASS_REGULAR_EXPRESSION "^ok" ENVIRONMENT "NODE_OPTIONS=")
  endif()

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME stubs_up_to_date
             COMMAND Python3::Interpreter
                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_stubs.py" --check)
    add_test(NAME exports_complete
             COMMAND Python3::Interpreter
                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_exports.py"
                     $<TARGET_FILE:quickjs-napi>)
  endif()

  # Leaks the vendored Node.js tests make by design (see the file).
  if(QJSNAPI_SANITIZE)
    get_property(_tests DIRECTORY PROPERTY TESTS)
    set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT
                 "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/test/lsan.supp:print_suppressions=0")
  endif()
endif()
