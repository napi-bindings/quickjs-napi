# Runs a command that must terminate abnormally (napi_fatal_error aborts the
# process), as Node's tests check with common.nodeProcessAborted. CTest marks
# a test whose process aborts as failed whatever its output, so the command
# runs here: its output is echoed only when it did not exit normally with
# code 0, and the test's PASS_REGULAR_EXPRESSION is matched against it.
#
#   cmake -DEXE=<program> -DARGS=<arg|arg|...> -P expect_abort.cmake
string(REPLACE "|" ";" _args "${ARGS}")
execute_process(
  COMMAND "${EXE}" ${_args}
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _out)
if(_result STREQUAL "0")
  message("expect_abort: the command exited normally (code 0)")
else()
  message("expect_abort: result '${_result}'\n${_out}")
endif()
