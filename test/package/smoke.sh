#!/bin/sh
# Checks an installed (or unzipped) quickjs-napi package.
#
#   test/package/smoke.sh <package dir> [extra cmake configure arguments]
#
# Builds test/package with the package (find_package(qjsnapi)), then runs:
#   - the embedding test linked with the static archives and, if present,
#     with the shared library;
#   - if the package has the executable: a script, and an addon built with
#     the package headers only (test/js/test_hello.js).
#
# Environment:
#   BUILD_DIR  build tree (default: ./package-smoke)
#   RUN        command prefix for the target's binaries, e.g. an emulator
#              ("qemu-arm -L /usr/arm-linux-gnueabihf"); "skip" builds only
#   CC         C compiler, as usual (or a toolchain file in the arguments)
set -eu

if [ $# -lt 1 ]; then
  echo "usage: $0 <package dir> [cmake arguments]" >&2
  exit 2
fi
pkg=$(CDPATH= cd -- "$1" && pwd)
shift
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
build=${BUILD_DIR:-$PWD/package-smoke}

# Paths handed to the target's binaries: native form on Windows (MSYS bash).
native() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi
}

run() {
  echo "+ ${RUN:-} $*"
  # RUN is a command prefix: word splitting is intended.
  # shellcheck disable=SC2086
  ${RUN:-} "$@"
}

if [ -f "$pkg/share/doc/quickjs-napi/BUILDINFO.txt" ]; then
  cat "$pkg/share/doc/quickjs-napi/BUILDINFO.txt"
fi

cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=$(native "$pkg")" "$@"
cmake --build "$build"

if [ "${RUN:-}" = skip ]; then
  echo "built only (RUN=skip)"
  exit 0
fi

run "$build/embed_static"
if [ -f "$build/embed_shared" ] || [ -f "$build/embed_shared.exe" ]; then
  run "$build/embed_shared"
fi

host="$pkg/bin/quickjs-napi"
if [ -f "$host" ] || [ -f "$host.exe" ]; then
  run "$host" -e "
    const b = require('buffer').Buffer.from('aGk=', 'base64');
    if (b.toString() !== 'hi') throw new Error('Buffer: ' + b.toString());
    console.log('script ok');"
  run "$host" "$(native "$root/test/js/test_hello.js")" "$(native "$build/addons")"
else
  echo "no executable in the package: addon test skipped"
fi
echo "package ok"
