#!/bin/bash
# The six projects' built trees in DRAM (/dev/shm, emptied at reboot), where
# modes_bench.py reads them: no compile reads a file from NFS or disk. Run it
# after record_builds.sh, and again after a reboot.
set -e
: "${BENCH_ROOT:?set BENCH_ROOT to the directory holding the six checkouts}"
DST=${SRC_ROOT:-/dev/shm/bench-src}
mkdir -p "$DST"
for p in redis nginx curl git postgres cpython; do
  rm -rf "$DST/$p.new" && mkdir -p "$DST/$p.new"
  (cd "$BENCH_ROOT/$p" && tar --exclude=./.git -cf - .) | (cd "$DST/$p.new" && tar -xf -)
  rm -rf "$DST/$p" && mv "$DST/$p.new" "$DST/$p"
done
du -sh "$DST"
