# verify-reimpl2 NOTES (core 4)

Task: bring round-1 reimpl (verify-reimpl/reimpl.py, copied here as reimpl_v1_orig.py) to the CURRENT spec
(simulate.py docstrings + design only; NOT simulate_server()/oracles_tok()/groups_of() bodies until results are in),
run on CURRENT out-record-s0 + out-redis-dram, compare with CURRENT out-server-s0.json, every label/pass/compile/field.

## Spec deltas implemented (from docstrings/task text)
- main: flags & 3 (1 UsesMain, 2 DepsMain; record format comment BothnerTrace.cpp ~1280)
- main-file decls unique per (pass, compile) in per-name sequences
- undefined (SV_UNDEFINED=32) right after builtin in design runs
- " fb": skip undefined; compile reusing any unit with SV_UNDEFINED_END(64) -> undo store changes, rerun with all parsed, reason "fallback"
- " tok": names checked += (0,n),(1,n) for n in toknames (frag field 25; span/group = union)
- unsafe2: reuses whose oracle-with-token-names (my reading of oracles_tok) differs
- " old": nested check from old observer (fields 8/9); groups formed once (loader calls groups_of without observer)

## Facts observed in CURRENT out-server-s0.json (pass 1 totals)
- 20 labels; " old" runs identical to fixed runs in all 5 passes x 187 compiles.
- no "unsafe", no "fallback" in any run; "main": 1 in every run.

## done / next
- [x] read spec (docstrings, design 2.4-2.9, 3, 8; plugin output format comment)
- [x] write reimpl2.py; run; compare: 18700/18700 exact (no mismatch to diagnose on redis)
- [x] sensitivity of 13 alternative readings (sens.log); only the token oracle's recursion is pinned by redis
- [x] read simulate_server/oracles_tok/groups_of after results; list code-level differences (G1-G3, T1, T2, K, M)
- [x] synthetic builds for unexercised paths + each divergence (synth.py); record bug for no-callback directives
- [x] explore4.py: key<->start bijective on redis (frags 6376, spans 9255 distinct, 0 collisions either way);
      T1: 16208/152871 non-empty frags have refs|binds names their tokens do not spell (typedef'd tags such as
      (1,_IO_FILE), (1,redisObject)), 14219 with earlier declarers, yet unsafe2 identical (those declarers are in the
      AST deps already); passes 2-5 identical for all 20 labels in out-server-s0.json.
- next: none required; final report.

## Results log
- reimpl2.py S2 only (defaults): 935/935 compiles exact, every field incl. unsafe2 (309 in pass 1). 21 s, 1.3 GB.
  repro: python3 reimpl2.py $B/out-record-s0 $B/out-redis-dram mine-S2.json --variants S2; python3 compare2.py mine-S2.json $B/out-server-s0.json S2
- reimpl2.py ALL 20 labels (defaults), CURRENT records: 18700/18700 (20 labels x 5 passes x 187 compiles) exact,
  every field (i, why, why_ns, saved_ns, saved_def_ns, restored, unsafe2). 199 s, 1.6 GB.
  repro: python3 reimpl2.py $B/out-record-s0 $B/out-redis-dram mine-all.json; python3 compare2.py mine-all.json $B/out-server-s0.json
  NOTE: redis exercises no SV bit except SV_UNDEFINED (83 frags, 75 of them keymeta.h:3770 nested under fixed observer);
  no SV_UNDEFINED_END (so fallback never fires), no param flag (4), one frag with flags&3 (081 zmalloc.h:3234, flags 11).
  => paths builtin/macro args/pending/pragma(weak/attr)/param/fallback/unsafe are NOT exercised by the redis comparison.

## next
- sensitivity: which alternative readings would break the match (pins the spec reading)
- read simulate_server/oracles_tok/groups_of AFTER this (results are in now), compare code paths not exercised on redis
- synthetic builds (tests/server_simulate.py style) for unexercised paths: fallback, tok oracle w/ main decls, SV flags

## Sensitivity (sens.sh -> sens.log): alternative readings on redis
- --tok-declared ref (declared token names also checked as refs): no change (2805/2805)
- --tok-oracle-rec ast (token oracle's deps contribute AST oracles): BREAKS unsafe2 (12-47 compiles/pass) => recursive token oracle pinned
- --tok-binders latest: no change (4675/4675)
- --tok-main yes (main marker for token names declared earlier in the main file): no change (4675/4675)
- --tok-sameloc keep: no change; --group-sv first: no change; --mark-members yes: no change; --empty-sep break: no change
- --deps-missing no: no change; --group-observer run (groups formed under the old observer in " old" runs): no change
- --main-id index (main-file decls identified by compile index, the pre-fix reading): NO change (5610/5610, incl.
  "S2 fb tok") => with flags&3 refusing "main", per-pass identity no longer matters on redis
- --key start (first-token offset, as simulate_server's loc[2]): no change (3740/3740)
- --config config (model's config id instead of full_config): no change (3740/3740)

## After results: read simulate_server/oracles_tok/groups_of (simulate.py 155-227, 509-712). Differences vs my reading
(none visible on redis since 18700/18700 exact):
- G1 groups_of: an EMPTY unit joins a run (member of the group: shape, ids, flags, tok) when the run's last non-empty
  unit has safter==0 and its sep is cond-only; mine keeps empty units out of groups (counted "empty" separately).
- G2 groups_of join test `not u["sep"] & ~SEP_COND` admits sep==0 (only null directives '#' between, Kind 0 in
  BothnerTrace lex()); mine requires sep == SEP_COND exactly.
- G3 boundary test: theirs last non-empty member's safter == 0; mine sa != 1 or next sb != 1 (equivalent when sa==sb,
  which holds for cond-only gaps incl. the K&R rule that clears STRICT on every directive before '{').
- T1 oracles_tok name set = token names (both ns) | refs | binds; mine token names only.
- T2 oracles_tok main marker: flags&2 OR any name in that set with an earlier main-file declaration; mine flags&2 only.
- K  simulate_server keys by loc[2] = START offset (a[1] / span sr[1]), NOT Frag::Key/Span::Key (a[23]/sr[15]);
     design 2.4 says "both move to the post-directive offset (S0)". Mine uses a[23]/sr[15]. Equivalent if start<->key
     is a bijection per file (see key-start sensitivity run).
- M  main decl with order == unit order: theirs inserts it before the unit (sort kind 0<1); mine strictly-less.
- fallback: theirs counts every non-empty unit of the rerun as "fallback" (reason set before any check) = my default.
- uses/deps version: theirs ver[u] (that instance's version); mine processed[key] at parse time; equivalent (argued:
  repeats are always re-parsed so a stale alias always refuses in both).

## Synthetic builds (synth.py; repo BothnerTrace.so + simulate.py vs reimpl2.py; every field, 5 passes)
repro: python3 synth.py synth [case...]   (workdir synth/<case>, build-<case>, theirs-/mine-<case>.json)
- SAME in all 20 labels: vs, undef (fallback 6 in pass 1 = every non-empty unit of 2 reruns), undef3 (fallback only
  in the compiles where sf stays undefined), flags (pending/builtin/param), weak (#pragma weak / clang attribute
  -> both "pending"), rep (repeats, occ, groups).
- DIFF emptyin/emptyin2/emptyend (G1, empty unit inside a group): theirs counts the group only; mine adds "empty";
  emptyin2 (EMPTY empty in e1, `int c;` in e3): theirs "macro", mine "conditionals" -- both refuse.
- DIFF args (G3): F1 "DECL(" ends inside macro args, SAfter == -1 (its #if/#endif E_DIR events precede its token
  events, so After is never set); theirs: no join (safter != 0) -> F1 nested; mine: join (sa != 1) -> one group,
  "macro args". Both refuse.
- DIFF mainvec (T2): N declared in the MAIN file before vec.h (vector_size drops the ref): theirs unsafe2 = 1 per
  pass (main marker for token names declared in the main file), mine 0. Theirs is right (vec_t differs).
- nulldir: SAME, but exposes a RECORD bug (below).
=> all divergences are readings the redis records never exercise: redis has 0 non-empty frags with After/SAfter -1,
   0 frags with sep 0 (explore3.py), and the tokmain/tokbinders/tokdecl sensitivity runs match exactly.

## FINDING (plugin record, reproduced): fragments after directives with no PPCallbacks event get stale nesting
- lex()/fragments() split fragments at EVERY directive line, incl. the null directive '#', #line, #error/#warning,
  #assert, line markers; nesting (Before/SBefore) comes from the last E_DIR event (LastDir) and After/SAfter from the
  next E_DIR. Those directives produce no E_DIR (CB has no callback for them), so the fragment after one inherits
  the view of an OLDER directive.
- repro synth/nulldir: nd.h "struct ns {\n int a;\n#\n int b;\n};" -> frag 2 (" int b; };") sb/sa 1/1, before/after
  1/1 (NOT nested) although the parser is inside struct ns; frag 1 sa -1. simulate.py S2: frag 2 reused in pass 1
  (pass1 {'nested': 2, 'reused': 2} over 2 compiles). A real server's hook fires at the null directive (it returns
  from HandleDirective) inside the struct: the model predicts a reuse a correct server must refuse.
- redis: no effect (0 non-empty frags with After == -1, 0 with sep 0).
- #line repro (synth/linedir2): ld2.h "struct ls {\n int a;\n#line 100\n int b;\n};\n#define AFTER 1\nextern int after_v;\n",
  l1.c/l2.c include it. Record: frag " int b;\n};" sb/sa 1/1, before/after 1/1 (stale view of the #include). simulate.py
  pass 1: S2 {'nested': 2, 'reused': 4}, S3 {'nested': 2, 'reused': 2} (span [ int b; }; #define AFTER; extern ...]
  starts inside struct ls and is reused). #warning behaves the same (synth/linedir: sb 1 after "#warning hi" inside
  struct ws). Directives without E_DIR: null '#', #line, #warning, #error, #assert/#unassert, GNU line markers.
- Completeness of the redis check: a stale SBefore needs parser tokens after the last E_DIR from a fragment H != G;
  H then keeps After == -1 (Cur moves to G before any E_DIR). redis: 0 non-empty frags with After/SAfter == -1
  => no stale nesting on redis.
- synth "pragma" (pending weak + int pad; active clang attribute + int pad; pack(2) in q2 only): SAME in all 20
  labels, pass 1 {'pragma': 9} in both (the "pragma" reason path via SV_WEAK/SV_ATTRIBUTE/prag is exercised).
## FINAL: redis 18700/18700 exact; no mismatch to diagnose. Synthetic divergences = readings (G1, G3, T2), all
   unexercised by redis; one record defect (stale nesting after no-callback directives), no effect on redis.

## Round 3 (2026-11-03): records after the round-2 fixes, simulate.py with span/occ changes
- records re-made (out-record-s0; previous kept as out-record-s0-r2): vs r2 only 82 frags lose SV_UNDEFINED
  (per-fragment rule; hiredis dict.h:4081 keeps it) and 10 frags gain refs __v4sf/__v8sf (convertvector dest type);
  42 refs lists reordered only. No sbefore/safter/prag/key/sep/toknames change.
- simulate.py changes not ported to reimpl2.py: span flags/ids include empty members; occ oracle excludes only the
  same reading. reimpl2.py unchanged, run on new records -> mine-all-r3.json:
  compare2.py vs out-server-s0.json: 18700/18700 exact (every label, pass, compile, field). => neither change is
  exercised on redis; the span change is pinned by tests/server_simulate.py "pm" (fails with the old line).
