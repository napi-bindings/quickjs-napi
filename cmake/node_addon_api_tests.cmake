# node-addon-api test suite (vendored by tools/sync_node_addon_api.py),
# included from CMakeLists.txt when QJSNAPI_NODE_ADDON_API_TESTS is ON.
#
# Layout in the build tree, as in a node-addon-api checkout:
#   node-addon-api-tests/index.js           harness/index.js (package stub)
#   node-addon-api-tests/test/...           the vendored test/ directory
#   node-addon-api-tests/test/common/       harness/common (quickjs-napi)
#   node-addon-api-tests/test/run.js        harness/run.js (one module)
#   node-addon-api-tests/test/preload.js    harness/preload.js (--require)
#   node-addon-api-tests/test/qn_assert.js  test/node/common/assert.js
#   node-addon-api-tests/test/build/Release/<binding>.node
# Every module is a CTest named naa/<module>:
#   NODE_OPTIONS=--require=test/preload.js \
#     quickjs-napi --expose-gc test/run.js <module>
# NODE_OPTIONS (not -r) so that the child processes the tests start load
# the preload as well.

set(QJSNAPI_NAA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/test/node-addon-api")
set(QJSNAPI_NAA_BIN "${CMAKE_CURRENT_BINARY_DIR}/node-addon-api-tests")

# The JavaScript side (configure_file re-copies a changed source).
file(GLOB_RECURSE _naa_js RELATIVE "${QJSNAPI_NAA_SRC}/test"
     "${QJSNAPI_NAA_SRC}/test/*.js")
foreach(_f ${_naa_js})
  configure_file("${QJSNAPI_NAA_SRC}/test/${_f}" "${QJSNAPI_NAA_BIN}/test/${_f}"
                 COPYONLY)
endforeach()
configure_file("${QJSNAPI_NAA_SRC}/harness/common/index.js"
               "${QJSNAPI_NAA_BIN}/test/common/index.js" COPYONLY)
configure_file("${QJSNAPI_NAA_SRC}/harness/run.js"
               "${QJSNAPI_NAA_BIN}/test/run.js" COPYONLY)
configure_file("${QJSNAPI_NAA_SRC}/harness/preload.js"
               "${QJSNAPI_NAA_BIN}/test/preload.js" COPYONLY)
configure_file("${QJSNAPI_NAA_SRC}/harness/index.js"
               "${QJSNAPI_NAA_BIN}/index.js" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/test/node/common/assert.js"
               "${QJSNAPI_NAA_BIN}/test/qn_assert.js" COPYONLY)

# One binding of test/binding.gyp. The defines are those of
# node_addon_api.gyp (except.gypi / noexcept.gypi) and common.gypi
# (NAPI_VERSION = process.versions.napi of Node.js 22: 10).
function(qjsnapi_naa_binding name exceptions)
  cmake_parse_arguments(T "" "" "SOURCES;DEFINES" ${ARGN})
  list(TRANSFORM T_SOURCES PREPEND "${QJSNAPI_NAA_SRC}/test/")
  add_library(naa_${name} MODULE ${T_SOURCES})
  set_target_properties(
    naa_${name}
    PROPERTIES OUTPUT_NAME "${name}"
               PREFIX ""
               SUFFIX ".node"
               CXX_STANDARD 17
               CXX_VISIBILITY_PRESET hidden
               LIBRARY_OUTPUT_DIRECTORY "${QJSNAPI_NAA_BIN}/test/build/Release"
               RUNTIME_OUTPUT_DIRECTORY "${QJSNAPI_NAA_BIN}/test/build/Release")
  target_include_directories(naa_${name} PRIVATE include "${QJSNAPI_NAA_SRC}"
                                                 "${QJSNAPI_NAA_SRC}/test/common")
  target_compile_definitions(
    naa_${name} PRIVATE BUILDING_NODE_EXTENSION NAPI_VERSION=10
                        NODE_GYP_MODULE_NAME=${name} ${T_DEFINES})
  if(exceptions)
    target_compile_definitions(naa_${name} PRIVATE NAPI_CPP_EXCEPTIONS)
    target_compile_options(naa_${name} PRIVATE -fexceptions)
  else()
    target_compile_definitions(naa_${name}
                               PRIVATE NODE_ADDON_API_DISABLE_CPP_EXCEPTIONS)
    target_compile_options(naa_${name} PRIVATE -fno-exceptions)
  endif()
  # As node-gyp in Release: assertions stay.
  target_compile_options(naa_${name} PRIVATE -UNDEBUG)
  # napi-inl.h reads DataView values through misaligned pointers
  # (DataView::ReadData): undefined behaviour of node-addon-api itself, which
  # UBSan (QJSNAPI_SANITIZE, zig's Debug default) would report.
  target_compile_options(naa_${name} PRIVATE -fno-sanitize=alignment)
  if(WIN32)
    target_link_libraries(naa_${name} PRIVATE quickjs-napi)
  elseif(APPLE)
    target_link_options(naa_${name} PRIVATE -undefined dynamic_lookup)
  endif()
  add_dependencies(quickjs-napi-tests naa_${name})
endfunction()

# One test module (called by the generated tests.cmake).
function(qjsnapi_naa_test module)
  add_test(NAME naa/${module}
           COMMAND quickjs-napi --expose-gc "${QJSNAPI_NAA_BIN}/test/run.js"
                   "${module}"
           WORKING_DIRECTORY "${QJSNAPI_NAA_BIN}/test")
  set_tests_properties(
    naa/${module}
    PROPERTIES ENVIRONMENT
               "NODE_OPTIONS=--require=${QJSNAPI_NAA_BIN}/test/preload.js")
endfunction()

include("${QJSNAPI_NAA_SRC}/tests.cmake")

# Racy upstream: SignalAfterProgressTestWorker calls Send() then Signal()
# from its thread; if the loop thread runs OnWorkProgress between the two,
# the second call sees no data (count 0, "expect 1 count of data"), under
# Node.js as well. The window is a preemption of the worker thread between
# two calls, so the test runs alone to keep the machine unloaded.
if(TEST naa/async_progress_worker)
  set_tests_properties(naa/async_progress_worker PROPERTIES RUN_SERIAL ON)
endif()

qjsnapi_naa_binding(binding ON SOURCES ${QJSNAPI_NAA_BUILD_SOURCES}
                    DEFINES NODE_ADDON_API_ENABLE_TYPE_CHECK_ON_AS)
qjsnapi_naa_binding(binding_noexcept OFF SOURCES ${QJSNAPI_NAA_BUILD_SOURCES})
qjsnapi_naa_binding(binding_noexcept_maybe OFF
                    SOURCES ${QJSNAPI_NAA_BUILD_SOURCES}
                    DEFINES NODE_ADDON_API_ENABLE_MAYBE)
qjsnapi_naa_binding(binding_custom_namespace OFF
                    SOURCES ${QJSNAPI_NAA_BUILD_SOURCES}
                    DEFINES NAPI_CPP_CUSTOM_NAMESPACE=cstm)
qjsnapi_naa_binding(binding_swallowexcept ON
                    SOURCES ${QJSNAPI_NAA_SWALLOWEXCEPT_SOURCES}
                    DEFINES NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS)
qjsnapi_naa_binding(binding_swallowexcept_noexcept OFF
                    SOURCES ${QJSNAPI_NAA_SWALLOWEXCEPT_SOURCES}
                    DEFINES NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS)
qjsnapi_naa_binding(binding_except_all ON
                    SOURCES ${QJSNAPI_NAA_EXCEPT_ALL_SOURCES}
                    DEFINES NODE_ADDON_API_CPP_EXCEPTIONS_ALL)
qjsnapi_naa_binding(binding_type_check OFF
                    SOURCES ${QJSNAPI_NAA_TYPE_CHECK_SOURCES}
                    DEFINES NODE_ADDON_API_ENABLE_TYPE_CHECK_ON_AS)
