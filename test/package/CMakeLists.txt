# Uses an installed quickjs-napi package the way a user would: through
# find_package(qjsnapi), with nothing from this source tree but test sources.
# Run by test/package/smoke.sh (and by the release workflow on every package).
#
#   cmake -S test/package -B build-pkg -DCMAKE_PREFIX_PATH=<package dir>
cmake_minimum_required(VERSION 3.24)
project(qjsnapi_package_test LANGUAGES C)

find_package(qjsnapi CONFIG REQUIRED)

set(_root "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# The embedding test against the three static archives.
add_executable(embed_static "${_root}/test/unit/test_embed.c")
target_link_libraries(embed_static PRIVATE qjsnapi::static)
target_compile_definitions(embed_static PRIVATE NAPI_VERSION=10)

# The same test against the shared library, QuickJS included.
if(QJSNAPI_HAS_SHARED)
  add_executable(embed_shared "${_root}/test/unit/test_embed.c")
  target_link_libraries(embed_shared PRIVATE qjsnapi::shared)
  target_compile_definitions(embed_shared PRIVATE NAPI_VERSION=10)
  if(WIN32)
    # Windows has no rpath: the DLL must be next to the executable.
    add_custom_command(
      TARGET embed_shared POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "$<TARGET_FILE:qjsnapi::shared>" "$<TARGET_FILE_DIR:embed_shared>"
      VERBATIM)
  endif()
endif()

# The addons of test/js/test_hello.js, built with the package headers only
# and loaded by the executable of the package.
if(QJSNAPI_HAS_HOST)
  foreach(_addon hello/hello loader/legacy loader/fnexport loader/future)
    get_filename_component(_name "${_addon}" NAME)
    add_library(${_name} MODULE "${_root}/test/addons/${_addon}.c")
    set_target_properties(
      ${_name}
      PROPERTIES PREFIX ""
                 SUFFIX ".node"
                 C_VISIBILITY_PRESET hidden
                 LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons"
                 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons")
    target_compile_definitions(${_name} PRIVATE NAPI_VERSION=10)
    target_link_libraries(${_name} PRIVATE qjsnapi::addon)
  endforeach()
endif()
