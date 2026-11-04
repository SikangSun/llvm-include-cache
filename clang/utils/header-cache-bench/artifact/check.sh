#!/bin/bash
# Correctness. For each project, plain clang and the header cache (linked
# copies, the paper's mode) compile every file to an object; then the cache
# compiles every file again on the same daemon, as a rebuild with nothing
# changed does. Each object of the cache is compared byte for byte with
# plain clang's. Prints one line per project.
#
#   BIN=... BENCH_ROOT=... BENCH_LOGS=... [OUT=check-results] [JOBS=N] ./check.sh [project...]
#
# JOBS projects run at once (default: half the CPUs, at most 7), each on
# two CPUs of its own: one for the compiles, one for its daemon.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${BIN:?set BIN to the directory build.sh printed}"
: "${BENCH_ROOT:?}" "${BENCH_LOGS:?}"
OUT=${OUT:-$PWD/check-results}
mkdir -p "$OUT"
PROJECTS=${*:-redis nginx curl git postgres cpython ffmpeg}
NCPU=$(nproc)
JOBS=${JOBS:-$(( NCPU / 2 < 7 ? (NCPU / 2 > 0 ? NCPU / 2 : 1) : 7 ))}
slot=0
for p in $PROJECTS; do
  (cd "$OUT" && BIN=$BIN KIND=full MODES=linkc PASSES=2 REGION_SIZE=16 LEVEL=1 \
     CORE=$((2 * slot % NCPU)) DCORES=$(((2 * slot + 1) % NCPU)) \
     python3 "$HERE/modes_bench.py" "$p" "$p.json" > "$p.log" 2>&1 || echo "$p: the run failed, see $OUT/$p.log") &
  slot=$((slot + 1))
  if [ "$slot" -ge "$JOBS" ]; then wait; slot=0; fi
done
wait
cd "$OUT"
python3 - $PROJECTS <<'PY'
import json, sys
bad = 0
for p in sys.argv[1:]:
    try:
        d = json.load(open(f"{p}.json"))
    except (OSError, ValueError):
        print(f"{p}: no result"); bad += 1; continue
    n = len(d["cmds"])
    first = len(d["differ"].get("linkc", [])); f1 = sum(1 for f in d["failed"] if f[0] == "linkc")
    again = d["passes"][0] if d.get("passes") else {}
    second = len(again.get("differ", {}).get("linkc", [])); f2 = sum(1 for f in again.get("failed", []) if f[0] == "linkc")
    bad += first + f1 + second + f2
    print(f"{p}: {n} files; first build: {first} objects differ, {f1} compiles failed; "
          f"same build again: {second} objects differ, {f2} compiles failed")
print("all objects the same" if not bad else f"{bad} differences or failures")
PY
