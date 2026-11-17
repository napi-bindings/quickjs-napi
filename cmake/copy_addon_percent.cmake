# Copies an addon into "<DIR>/foo%#bar/", a directory whose file URL needs
# percent-encoding (test/js/test_node_api.js). The special characters stay
# inside this script, away from the shell running the build step.
#
#   cmake -DINPUT=<addon> -DDIR=<addon-dir> -P copy_addon_percent.cmake
file(MAKE_DIRECTORY "${DIR}/foo%#bar")
file(COPY "${INPUT}" DESTINATION "${DIR}/foo%#bar")
