#!/bin/bash
# PPH smoke tests. Images: b.h; a.h (links b.h); d.h (carries unmapped c.h as
# text). Refusals: u.h (no guard), inc1/n.h (#include_next). Each main is
# compiled with images and compared with plain clang, with and without the
# modules flags, at -O2 and -O2 -g.
set -u
BIN=$1; cd "$(dirname "$0")"
CC=$BIN/clang
MOD="-fmodules -fno-implicit-module-maps"
rm -rf img gen_* *.s *.log; mkdir img
gen() {  # gen <header> [flags...]
  local h=$1; shift; local g=gen_$(echo $h | tr / _).c
  printf '#include "%s"\n' "$h" > $g
  CLANG_PPH_DIR=img CLANG_PPH_GEN=$PWD/$h $CC -fsyntax-only $MOD "$@" $g 2> $g.log
  echo "gen $h: exit $?; images now: $(find img -name '*.pph' | wc -l)"; grep -v '^$' $g.log | sed 's/^/    /' | head -5
}
gen b.h; gen a.h; gen d.h; gen u.h; gen inc1/n.h -Iinc1 -Iinc2
rc=0
for m in main1 main2 main3 main4; do
  for opt in "-O2" "-O2 -g"; do
    tag=$m$(echo $opt | tr -d ' -')
    $CC $opt -S $m.c -o $tag.plain0.s
    $CC $opt -S $MOD $m.c -o $tag.plainM.s
    CLANG_PPH_DIR=img CLANG_HEADER_CACHE_STATS=2 $CC $opt -S $MOD $m.c -o $tag.pph.s 2> $tag.log
    e=$?
    served=$(grep -c '^ *served' $tag.log); rej=$(grep -A3 'rejected:' $tag.log | grep -v rejected | sed 's/^ */      /' | head -3)
    if cmp -s $tag.plainM.s $tag.pph.s; then r="identical"; else r="DIFFERS"; rc=1; fi
    cmp -s $tag.plain0.s $tag.pph.s && r0="identical" || r0="differs"
    echo "== $m [$opt]: exit $e, served $served header(s); vs plainM: $r; vs plain0: $r0"
    [ -n "$rej" ] && echo "$rej"
    grep -iE 'error|warning' $tag.log | head -3
  done
done
exit $rc
