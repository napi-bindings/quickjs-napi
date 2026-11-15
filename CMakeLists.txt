cmake_minimum_required(VERSION 3.24)

project(quickjs-napi LANGUAGES C)

# ------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------
option(QJSNAPI_SANITIZE
       "Build everything (QuickJS included) with AddressSanitizer and UBSan" OFF)
option(QJSNAPI_LIBUV "Event loop integration through libuv (milestone M5)" OFF)
option(QJSNAPI_BUILD_TESTS "Build the test addons and the test suite" ON)

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

if(QJSNAPI_LIBUV)
  message(FATAL_ERROR "QJSNAPI_LIBUV belongs to milestone M5 and is not "
                      "available yet")
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
    src/env.c
    src/error.c
    src/function.c
    src/loader.c
    src/object.c
    src/scope.c
    src/string.c
    src/value.c
    src/stubs_generated.c)

add_library(qjsnapi STATIC ${QJSNAPI_SOURCES})
set_target_properties(
  qjsnapi
  PROPERTIES C_STANDARD 17
             C_STANDARD_REQUIRED ON
             C_VISIBILITY_PRESET hidden
             POSITION_INDEPENDENT_CODE ON)
# The implementation is compiled against Node-API 10. Addons pick their own
# NAPI_VERSION; it is not propagated to consumers.
target_compile_definitions(qjsnapi PRIVATE NAPI_VERSION=10)
target_include_directories(qjsnapi PUBLIC include PRIVATE src)
target_link_libraries(qjsnapi PUBLIC qjs ${CMAKE_DL_LIBS})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(qjsnapi PRIVATE -Wall -Wextra -Wno-unused-parameter
                                         -Wmissing-prototypes)
endif()

# ------------------------------------------------------------------------------
# Host executable
# ------------------------------------------------------------------------------
# Addons resolve napi_* from the process: every Node-API function must be
# exported by the executable.
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

set(QJSNAPI_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${QJSNAPI_GENERATED_DIR}/bootstrap_js.h"
  COMMAND
    "${CMAKE_COMMAND}" -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/host/bootstrap.js
    -DOUTPUT=${QJSNAPI_GENERATED_DIR}/bootstrap_js.h -DNAME=qn_bootstrap_js -P
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS host/bootstrap.js cmake/embed.cmake
  COMMENT "Embedding host/bootstrap.js"
  VERBATIM)

add_executable(quickjs-napi host/main.c
                            "${QJSNAPI_GENERATED_DIR}/bootstrap_js.h")
target_include_directories(quickjs-napi PRIVATE "${QJSNAPI_GENERATED_DIR}")
set_target_properties(quickjs-napi PROPERTIES C_STANDARD 17 ENABLE_EXPORTS ON)
target_link_libraries(
  quickjs-napi PRIVATE "$<LINK_LIBRARY:QJSNAPI_WHOLE_ARCHIVE,qjsnapi>")
if(WIN32 AND NOT MSVC)
  # static libgcc / libwinpthread, as for the quickjs-build tools
  target_link_options(quickjs-napi PRIVATE -static)
endif()

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
if(QJSNAPI_BUILD_TESTS)
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
    target_include_directories(${name} PRIVATE include)
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
  foreach(_f index.js assert.js)
    configure_file("${QJSNAPI_NODE_TESTS_SRC}/common/${_f}"
                   "${QJSNAPI_NODE_TESTS_BIN}/common/${_f}" COPYONLY)
  endforeach()

  function(qjsnapi_node_test dir)
    cmake_parse_arguments(T "" "TARGET" "SOURCES;DEFINES;SCRIPTS" ${ARGN})
    set(_src "${QJSNAPI_NODE_TESTS_SRC}/js-native-api/${dir}")
    set(_bin "${QJSNAPI_NODE_TESTS_BIN}/js-native-api/${dir}")
    set(_lib "node_${dir}_${T_TARGET}")
    list(TRANSFORM T_SOURCES PREPEND "${_src}/")
    add_library(${_lib} MODULE ${T_SOURCES})
    set_target_properties(
      ${_lib}
      PROPERTIES OUTPUT_NAME "${T_TARGET}"
                 PREFIX ""
                 SUFFIX ".node"
                 C_VISIBILITY_PRESET hidden
                 LIBRARY_OUTPUT_DIRECTORY "${_bin}/build/Release"
                 RUNTIME_OUTPUT_DIRECTORY "${_bin}/build/Release")
    target_include_directories(${_lib} PRIVATE include)
    target_compile_definitions(
      ${_lib} PRIVATE BUILDING_NODE_EXTENSION
                      NODE_GYP_MODULE_NAME=${T_TARGET} ${T_DEFINES})
    if(WIN32)
      target_link_libraries(${_lib} PRIVATE quickjs-napi)
    elseif(APPLE)
      target_link_options(${_lib} PRIVATE -undefined dynamic_lookup)
    endif()
    add_dependencies(quickjs-napi-tests ${_lib})
    foreach(_script ${T_SCRIPTS})
      configure_file("${_src}/${_script}" "${_bin}/${_script}" COPYONLY)
      add_test(NAME node/${dir}/${_script}
               COMMAND quickjs-napi "${_bin}/${_script}"
               WORKING_DIRECTORY "${_bin}")
    endforeach()
  endfunction()

  include("${QJSNAPI_NODE_TESTS_SRC}/js-native-api/tests.cmake")

  add_test(NAME js_values
           COMMAND quickjs-napi
                   "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_values.js"
                   "${QJSNAPI_ADDON_DIR}")

  add_executable(test_embed test/unit/test_embed.c)
  set_target_properties(test_embed PROPERTIES C_STANDARD 17)
  target_link_libraries(test_embed PRIVATE qjsnapi)
  target_compile_definitions(test_embed PRIVATE NAPI_VERSION=10)
  add_test(NAME unit_embed COMMAND test_embed)

  # Reference run: the same addons and script under Node.js. Skipped with
  # sanitizers: ASan-instrumented addons cannot be loaded by a regular node.
  find_program(QJSNAPI_NODE node)
  if(QJSNAPI_NODE AND NOT CMAKE_CROSSCOMPILING AND NOT QJSNAPI_SANITIZE)
    foreach(_t hello values)
      add_test(NAME node_parity_${_t}
               COMMAND "${QJSNAPI_NODE}"
                       "${CMAKE_CURRENT_SOURCE_DIR}/test/js/test_${_t}.js"
                       "${QJSNAPI_ADDON_DIR}")
    endforeach()
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
endif()
