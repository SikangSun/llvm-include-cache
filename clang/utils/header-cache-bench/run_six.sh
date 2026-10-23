#!/bin/bash
# One standard run: the six first builds, front end (-fsyntax-only), plain
# clang and the cache modes interleaved compile by compile, one project per
# compile core, each project's daemons on a core of their own. With
# FULL_CORES, full builds also run (on other cores) to compare every object
# file with plain clang's; those are not for timing.
#
#   OUT=results/<name> MODES="top" ./run_six.sh
#
# env: BIN, BENCH_LOGS, BENCH_ROOT (as for modes_bench.py); OUT; MODES
# (default "top"; see modes_bench.py's ALL); CORES and DCORES, six
# comma-separated cores each (default 2..7 and 8..13); FULL_CORES and
# FULL_DCORES likewise for full builds (default none); LEVEL (1).
# Keep every other process, and the hyperthread siblings of these cores, off
# the machine while it runs.
set -e
: "${OUT:?set OUT to a results directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
MODES=${MODES:-top}
IFS=, read -ra C <<< "${CORES:-2,3,4,5,6,7}"
IFS=, read -ra D <<< "${DCORES:-8,9,10,11,12,13}"
IFS=, read -ra FC <<< "${FULL_CORES:-}"
IFS=, read -ra FD <<< "${FULL_DCORES:-}"
mkdir -p "$OUT" && cd "$OUT"
i=0
for p in redis nginx curl git postgres cpython; do
  dcs=$(printf "${D[$i]},%.0s" $MODES); dcs=${dcs%,}
  MODES="$MODES" DUMP=$PWD/$p LEVEL=${LEVEL:-1} KIND=syntax CORE=${C[$i]} DCORES=$dcs python3 "$HERE/modes_bench.py" $p $p.json > $p.log 2>&1 &
  if [ ${#FC[@]} -eq 6 ]; then
    fdcs=$(printf "${FD[$i]},%.0s" $MODES); fdcs=${fdcs%,}
    MODES="$MODES" KIND=full CORE=${FC[$i]} DCORES=$fdcs python3 "$HERE/modes_bench.py" $p full-$p.json > full-$p.log 2>&1 &
  fi
  i=$((i + 1))
done
wait
tail -q -n1 *.log
python3 - <<'PY'
import json, math, glob
P = ["postgres", "git", "cpython", "curl", "redis", "nginx"]
j = {p: json.load(open(f"{p}.json")) for p in P}
for m in j["postgres"]["modes"][1:]:
    s = [sum(j[p]["T"]["plain"]) / sum(j[p]["T"][m]) for p in P]
    print(f"{m}: " + ", ".join(f"{p} {x:.2f}x" for p, x in zip(P, s)) +
          f"; geometric mean {math.exp(sum(map(math.log, s)) / len(s)):.3f}x")
PY
