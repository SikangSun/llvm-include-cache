#!/bin/bash
# Builds clang and the header cache daemon from this checkout: release,
# assertions off, X86 only, as the paper's numbers were measured.
#
#   BUILD=<new build directory> ./build.sh
#
# Prints the BIN directory the other scripts take.
set -e
SRC=$(cd "$(dirname "$0")/../../../.." && pwd)
BUILD=${BUILD:-$SRC/build-artifact}
cmake -G Ninja -S "$SRC/llvm" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_ENABLE_ASSERTIONS=OFF > "$BUILD.cmake.log"
ninja -C "$BUILD" clang clang-header-cache-daemon
echo "BIN=$BUILD/bin"
