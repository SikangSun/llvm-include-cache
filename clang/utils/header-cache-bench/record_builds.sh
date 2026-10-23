#!/bin/bash
# Builds the six projects once through cclog, recording every compile command
# (BENCH_LOGS/log-<project>) for modes_bench.py to replay. Run it with the
# sources at BENCH_ROOT (one checkout per project, at the commits in
# HISTORY.md), then copy the built trees to DRAM with dram_sources.sh: the
# replays need the generated headers the builds made.
set -e
: "${BENCH_ROOT:?set BENCH_ROOT to the directory holding the six checkouts}"
: "${BENCH_LOGS:?set BENCH_LOGS to a directory for the compile logs}"
: "${CCLOG_CLANG:?set CCLOG_CLANG to a release clang of this tree}"
export CCLOG_CLANG
CC=$(cd "$(dirname "$0")" && pwd)/cclog
J=${J:-32}
log() { L=$BENCH_LOGS/log-$1; rm -rf "$L"; mkdir -p "$L"; echo "$L"; }
cd "$BENCH_ROOT/redis" && git clean -fdxq && CCLOG=$(log redis) make -j$J CC=$CC MALLOC=libc > /dev/null
cd "$BENCH_ROOT/nginx" && git clean -fdxq && ./auto/configure --with-cc=$CC --without-http_rewrite_module --without-http_gzip_module > /dev/null && CCLOG=$(log nginx) make -j$J > /dev/null
cd "$BENCH_ROOT/curl" && git clean -fdxq && autoreconf -fi > /dev/null && ./configure CC=$CC --without-ssl --without-libpsl --disable-shared > /dev/null && CCLOG=$(log curl) make -j$J > /dev/null
cd "$BENCH_ROOT/git" && git clean -fdxq && CCLOG=$(log git) make -j$J CC=$CC > /dev/null
cd "$BENCH_ROOT/postgres" && git clean -fdxq && ./configure CC=$CC --without-icu --without-readline --without-zlib > /dev/null && CCLOG=$(log postgres) make -j$J > /dev/null
cd "$BENCH_ROOT/cpython" && git clean -fdxq && ./configure CC=$CC > /dev/null && CCLOG=$(log cpython) make -j$J > /dev/null
for p in redis nginx curl git postgres cpython; do echo "$p: $(ls "$BENCH_LOGS/log-$p" | wc -l) compiler calls"; done
