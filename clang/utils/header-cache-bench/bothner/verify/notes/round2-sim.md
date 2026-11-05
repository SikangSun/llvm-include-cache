# verify-sim2: adversarial review of simulate.py S0 additions (round 2)

Core 6. Repo files untouched. All experiments under this directory.

## done / next
- [ ] read design 2.4-2.9, 3, 8; paper p.26-33
- [ ] read simulate.py server code (groups_of, oracles_full, simulate_server, scoring)
- [ ] build instrumented copy + small-build harness (on FIXED code)
- [ ] item 1 faithfulness
- [ ] item 2 groups_of
- [ ] item 3 simulate_server
- [ ] item 4 scoring / start-up credit
- [ ] item 5 oracle meaningfulness

## findings

### F-score-1 (item 4, VERIFIED by data): predicted time below the main+directive floor
- Claim: pred = plain - SCALE*saved - credit; floor = span - saved (traced work the server still does).
  With SCALE=1, pred - floor = (plain - span) - credit exactly. So pred < floor iff plain - span < 1.65 ms.
- Data: only hyperloglog.c (i=131, plain-span 0.24 ms) and crccombine.c (i=124, 0.64 ms). Excess credit 2.4 ms of
  15.6 s plain: negligible on rows.
- Why plain-span is small there: regression over 187 compiles: plain - span = 7.64 ms - 14.3 ns/token (resid sd 0.87 ms).
  The timing run's span carries ~14 ns/token of plugin overhead that the plain run does not; token-heavy compiles
  (406k, 307k tokens) lose the whole outside-span margin. Over the build ~151 ms = 1.6% of fragment time: same sign and
  size as D6/D7's blanking result (model overstates saving ~+0.9..1.3%). Generous to Bothner -> bound stays an upper bound.
- Not a simulator bug; a cosmetic per-compile inconsistency. A cap credit<=max(0,plain-span)+pre would remove it.
- Repro: python3 snippet in this file's history (regression over out-recompile-dram.json vs out-redis-dram span_ns).

### F-neg-1 (item 1/5, REPRODUCED): negative lookups inside function bodies are not checked by the modeled rule
- A failed file-scope lookup of a tag in an inline function body (struct x *p = v;) declares a LOCAL tag;
  the unit has no bind and no ref for (1,'x'), so nothing records the negative dependency.
- Repro: verify-sim2/cases/t1_tag (h.h: static inline int sz(void *v) { struct x *p = v; return p != 0; };
  c1: h.h only; c2: x.h (struct x {..}) then h.h). Every pass: c2 h.h passes all checks -> oracle differs ->
  'unsafe' (S2, S2 fb, S3G* fb). Same with sizeof(struct y *) / casts (t1_tag2).
- Design 3 says the server records "names looked up and not found"; the simulator's 'negative' only covers names the
  unit DECLARES at file scope. So the model's rule is narrower than the design's.
- Block-scope extern (t1_bext) IS handled: local extern decls count as file scope in Refs -> 'deps'.
- Effect on redis rows: none (oracle unsafe = 0 in every run means no such instance). On other projects the S0 model
  would report 'unsafe' where the designed server would report 'negative' (charged as parse either way).

### F-tok-names (item 5, VERIFIED): unsafe2's token-only names on redis are all coincidental
- a3_toknames.py: S3G* fb tok refusals caused only by token names: struct fields (read/write/close, free, dup, rax,
  significand, brk, select[], log, rewind), parameters (link, stream in a comment?/param, index, user),
  attribute names (malloc in __attribute__((malloc)), printf in format()), tag-vs-typedef namespace
  (RedisModuleKeyOptCtx). None is a real AST-dropped dependency.

### F-occ-oracle (item 5, REPRODUCED): under occurrence keys both oracles are blind to deps between readings of one location
- oracles_full / oracles_tok drop deps whose loc equals the unit's loc ("earlier readings of its own location").
  Under occurrence=True, (loc,1) and (loc,2) are different keys/versions, yet the 2nd reading's oracle ignores the 1st.
- Repro: verify-sim2/t2_occ.py (case cases/t2_occ). h.h read twice; 2nd reading:
  enum { second = sizeof(int __attribute__((vector_size(first * 4)))) }, first from the 1st reading (BASEVAL 4 or 8).
  plain clang: c1 second==64, c2 second==128 (static asserts pass). 'S2 fb occ' and 'S3G* fb occ' reuse c1's 2nd
  reading in c2 (wrong: 64 instead of 128) with unsafe 0 AND unsafe2 0. 'S3G* fb occ tok' refuses (deps).
- Non-occ runs: the 2nd reading is a repeat -> parsed, so unaffected.
- Needs an AST-dropped dep between readings of the SAME location (AST-visible ones are caught by the version checks).
