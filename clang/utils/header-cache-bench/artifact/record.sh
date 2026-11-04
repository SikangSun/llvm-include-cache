#!/bin/bash
# Configures and builds each project once with plain clang through cclog,
# which logs every compiler call (record_builds.sh), then copies the built
# trees, generated headers included, to /dev/shm (dram_sources.sh), where
# check.sh reads them. Run dram_sources.sh again after a reboot.
#
#   BIN=<build>/bin BENCH_ROOT=... BENCH_LOGS=<new directory> ./record.sh
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${BIN:?set BIN to the directory build.sh printed}"
: "${BENCH_ROOT:?set BENCH_ROOT to the directory fetch.sh filled}"
: "${BENCH_LOGS:?set BENCH_LOGS to a directory for the compile logs}"
mkdir -p "$BENCH_LOGS"
CCLOG_CLANG=$BIN/clang "$HERE/record_builds.sh"
"$HERE/dram_sources.sh"
