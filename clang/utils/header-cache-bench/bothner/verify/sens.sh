#!/bin/bash
# Sensitivity: which alternative readings break the exact match on redis.
S=${S:-$(cd "$(dirname "$0")" && pwd)}
B=${B:-$(cd "$(dirname "$0")/.." && pwd)}
mkdir -p $S/sens
run() {  # name labels opts...
  local name=$1 labels=$2; shift 2
  echo "=== $name [$labels] $*"
  nice -n 10 taskset -c 4 python3 $S/reimpl2.py $B/out-record-s0 $B/out-redis-dram $S/sens/$name.json --variants "$labels" "$@" 2>/dev/null
  python3 $S/compare2.py $S/sens/$name.json $B/out-server-s0.json "$labels" --show 3
}
TOK="S2 fb tok,S3G* fb tok,S3G* fb occ tok"
U2="S2,S3,S2G,S3G*,S3G* fb"
G="S2G,S3G,S3G*,S3G* fb"
run tokdecl-ref "$TOK" --tok-declared ref
run tokrec-ast "$U2" --tok-oracle-rec ast
run tokbind-latest "$U2" --tok-binders latest
run tokmain-yes "$U2" --tok-main yes
run toksameloc-keep "$U2" --tok-sameloc keep
run groupsv-first "$G" --group-sv first
run markmembers-yes "$G" --mark-members yes
run emptysep-break "$G" --empty-sep break
run depsmissing-no "S2,S3,S3G*,S3G* fb" --deps-missing no
run groupobs-run "S2G old,S3G old,S3G* old" --group-observer run
run mainid-index "S2,S3,S2G,S3G*,S3G* fb,S2 fb tok" --main-id index
run key-start "S2,S3,S3G*,S3G* fb occ" --key start
run config-model "S2,S3,S3G*,S3G* fb occ" --config config
echo DONE
