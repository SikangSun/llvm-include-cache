#!/bin/bash
# Builds the plugins against an existing clang build, without rebuilding clang:
# BothnerTrace.so here, and fragcount/FragCount.so.
#
#   BUILD=<llvm-project>/build_rel ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../../../.. && pwd)
BUILD=${BUILD:-$ROOT/build_rel}
for P in BothnerTrace fragcount/FragCount; do
  "$BUILD/bin/clang++" -shared -fPIC -std=c++17 -fno-rtti -O2 \
    -I"$ROOT/llvm/include" -I"$ROOT/clang/include" \
    -I"$BUILD/include" -I"$BUILD/tools/clang/include" \
    "$P.cpp" -o "$P.so"
done
