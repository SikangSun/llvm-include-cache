#!/bin/bash
# The header cache's unit tests, then Clang's Modules and PCH tests.
#
#   BUILD=<the build directory of build.sh> ./tests.sh
set -e
: "${BUILD:?set BUILD to the build directory build.sh used}"
ninja -C "$BUILD" AllClangUnitTests
"$BUILD/tools/clang/unittests/AllClangUnitTests" --gtest_filter='AskListTest.*:ASTCacheDependencyTrackerTest.*:CacheProtocolTest.*:ConcurrencyTest.*:DaemonDecisionsTest.*:DaemonTest.*:DeclFingerprintTest.*:DependencySerializationTest.*:EndToEndTest.*:ExportTest.*:FixedListTest.*:HitTest.*:LoadTest.*:OfflineTest.*:PartExportTest.*:ProducerTest.*:SymbolDependencyTest.*:TwoTreesTest.*'
ninja -C "$BUILD" check-clang-modules check-clang-pch
