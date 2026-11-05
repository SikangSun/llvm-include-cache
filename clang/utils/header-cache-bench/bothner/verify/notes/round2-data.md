# verify-data2 notes (second round of data checks, core 7)

Snapshots: sim_cur.py = simulate.py (md5 86f52499...), BothnerTrace_cur.cpp = plugin as of 18:34.
Pre-fix simulator: ../verify-data/sim_copy.py (first round's copy).

## done / next
- [x] 1 fix-by-fix (F1.1-F1.3)
- [x] 2 root cause on server.c (F2)
- [x] 3 repeats / occurrence keys (F3)
- [~] 4 SV_UNDEFINED: F4a done (same-unit definitions); task4.py (per-file loss) running
- [ ] 5 scoring sanity: task5.py written (JSON+timing), misc.py (fix-1 count, restore consistency) written; run after task4

## findings

### F1.1 (RUN) Records: the only non-timing difference pre-fix -> current is SV_UNDEFINED (32) on 83 frag instances in 80 compiles
- recdiff.py / recdiff.out: every frag field 0-24 except timing (5,6) and every span field compared, names translated by string.
  Only field 20 differs, bits (added 32, removed 0) on 83 instances. No other field (17/18 strict nesting, 22 refs, 13 flags,
  ids, uses, deps, binds, key, sep, prag), no span field, no defs/main_decls/entries/config/full_config differ.
  Field 25 (toknames) is new in the current records (pre had 25 fields).
- The 83: keymeta.h:3770 x75; strbuf.h:1590 x2 (lua strbuf.c, lua_cjson.c); hiredis async.c: ./dict.c:2124, ./dict.h:4081;
  redismodule.h:98820 x3 (157, vector-sets 182, 183); vector-sets fastjson.c:1406 (182). SV_UNDEFINED_END (64): 0 instances.
- Current records: SV_ARGS on 0 instances, SV_BUILTIN/PENDING/WEAK/ATTRIBUTE on 0. flags&1 on 1 instance, flags&2 on 1:
  the same one (zmalloc.c's ./zmalloc.h:3234, flags 11 = uses main | deps main | main uses it; zlibc_free defined in
  zmalloc.c before #include "zmalloc.h"). So fix 1 (flags&3) changes nothing: that unit was already refused as "main".
- tests/server_record.py with the current .so: all ok (args2.h end flag, kr.h, clu.h refs done, und.h [96,0]) -> fixes 2-5
  are in the build; the zero counts on redis are data facts, not a missing fix.
  => units flagged on redis: fix1 0 (beyond the 1 already refused), fix2 0, fix3 0, fix4 0, fix5 83.

### F1.2 (RUN) Results: every " fb" run == pre-fix run, field for field, every compile, every pass
- cmp_results.py / cmp_results.out: pre S2,S3,S2G,S3G,S3G* vs cur "X fb": 0 compiles differ in why/why_ns/saved_ns/
  saved_def_ns/restored in all 5 passes. Also pre "S2 occ"/"S3G* occ" == cur "S2 fb occ"/"S3G* fb occ" exactly.
- No "fallback" reason in any of the 20 runs x 5 passes x 187 compiles (fallback never triggered; consistent with 0 SV_UNDEFINED_END).
- Design runs vs pre (pass 2): S2 & S2G: 6 compiles, undefined +8 inst (35.22 ms), uses -6 (-33.41 ms), reused -2 (-1.81 ms).
  S3 & S3G: 80 compiles, undefined +83 (121.26 ms), uses -77 (-81.58), reused -6 (-39.68 ms).
  S3G*: 80 compiles, undefined +83 (121.26), uses +888 (+1025.76), negative -3 (-1.77), reused -968 (-1145.25 ms).
  "old" runs move exactly as their fixed-observer twins. All moves are fix 5 (2.8's rule) only.

### F1.3 (RUN) Predicted rows, pre-fix -> current design (rows.py / rows.out; all 187 compiles, recompilation)
- S2 1.5415x -> 1.5413x (+1.81 ms); S2G 1.7247 -> 1.7244 (+1.81 ms); S3 1.6360 -> 1.6292 (+39.68 ms); S3G 1.6583 -> 1.6513 (+39.68 ms);
  S3G* 2.1745 -> 1.8753 (+1145.25 ms). redis-server (src/) only: S2/S2G unchanged (the 8 S2 'undefined' units are outside src/),
  S3/S3G +37.86 ms, S3G* 2.3293 -> 1.9461 (+1143.43 ms). Every " fb" row == the pre-fix row to the ns.
- The model section of the log (first 111 lines) is byte-identical between out-sim-s0-pre.log and out-sim-s0.log.

### F2 (RUN) Root cause on server.c (task2.py / task2.out): CONFIRMED
- S2: _IO_FILE's 5 option-2 fragments (1824,3175,3424,3447,3572; seps cond only) are nested; mutex's 5 (900..1550) nested.
  server.c roots: uses<-nested FILE 9.2%, deps<-nested FILE 3.0% of its header time; build: 8.3% + 3.7%.
  Counterfactual (all compiles): FILE reusable 1.541x->1.656x; mutex alone 1.547x; both 1.662x.
- S2G: _IO_FILE = one group [1824,3175,3424,3447,3572] sb/sa 1/1 -> reused (114.3 us in server.c). mutex splits at
  `# define __PTHREAD_MUTEX_HAVE_PREV 1` (unit 1550's sep = cond|define): [900,1000,1154,1186] 1/0 and [1550] 0/1, both nested.
  Build root uses<-nested struct_mutex.h 10.5% (server.c 12.2%). Counterfactual mutex reusable: 1.724x->1.934x (66.0%->76.3%);
  FILE reusable: no change.
- S3/S3G: _IO_FILE's last span 3572 = `char _unused2[..]; };` + the 7 #define lines after it (defines 7, sb 0). S3G groups:
  [1824,3175,3424,3447] 1/0 and [3572] 0/1 (cannot join: no #define in a group). Root uses<-nested FILE 12.0% + deps 3.5%.
  Counterfactual: 3572 without its defines joins -> 1.651x->1.767x (58.5%->64.7%), same as FILE reusable.
- S3G*: one group [1824..3572] (defines 7) and one mutex group [864,1000,1154,1186,1550] (defines 2), both 1/1, reused;
  making the files reusable changes nothing; S3G*'s top root is keymeta.h 'undefined' (10.9%).
- Note: in server.c alone, the S3/S3G top root by first failing use is uses<-negative __stdarg_va_list.h (22.8%/23.0% of
  server.c's header time); attribution follows the first failing use only, so the counterfactuals are the better evidence.
- server.c's own reused share under the counterfactuals (task2b.out): S2 30.3% -> FILE 36.6 / mutex 30.7 / both 37.0;
  S2G 39.3 -> mutex 50.2 (FILE no change); S3 29.4 -> FILE 32.5; S3G 30.9 -> FILE 33.9 = 3572-without-defines 33.9.
  server.c has 28.8-29.9% of its header time in 'repeat' (mathcalls).

### F3 (RUN) Repeats and occurrence keys (task3.py / task3.out)
- Option 2 (S2 fb, pass 2): repeats 8313 inst = 994.0 ms = 10.50%: mathcalls.h 934.32 ms (9.87%, 7845 inst; readings k=2..8),
  mathcalls-narrow.h 34.48 ms (0.36%, 300; k=2..13), mathcalls-helper-functions.h 25.19 ms (0.27%, 168; k=2..4). All mathcalls*.h. CONFIRMED.
- Option 3 + groups* (S3G* fb): repeats 35,225 inst = 1037.4 ms = 10.38%: the same 994.0 ms of mathcalls*.h (9.95%) PLUS 26,912
  instances / 43.4 ms (0.43%) of other repeats: libc-header-start.h 20.1 ms (9090), <res>/stddef.h 14.7 (9163), wordsize.h 4.0
  (5196), stdarg.h 2.1, __stddef_null.h 2.0, long-double.h 0.4, timesize.h 0.2, assert.h, version.h. These are option-3 spans
  carrying #define/#undef lines; their k-th reading's macro list EQUALS reading k-1's (992 keys). So the design doc's "all 8,313
  repeats are bits/mathcalls*.h read under different macros" holds for option 2 only; under option 3 there are identical repeats.
- Readings per compile (config c58875 = redis-server, 125 compiles; mathcalls.h included by 30 of them): 25 compiles read the
  mathcalls.h locations 8 times, 5 compiles 3 times (23 locations read 3 or 8 times, 10 locations only in the 8-reading
  compiles, 6 only 3). mathcalls-narrow.h 13 readings (25 compiles), helper-functions 4 (30). Lua cfg (38): 3 readings.
- k>1 reading keys: 413 (S2) / 1405 (S3G*); in EVERY compile of a configuration the k-th reading has the same macro list,
  token hash and full pragma state (0 keys with >1 of any). CONFIRMED. Keys with >1 macro list are all k=1:
  commands.def (354 keys), connection.h, pthread.h, server.h, mstr.h, geohash.h (+ sys/select.h, bits/fcntl.h in S3G*).
- Each mathcalls k>1 reading's macro list differs from reading k-1's (413 of 413): that is what makes non-occ refuse k=1 as
  'macro' (stored = last reading of the previous compile).
- What lifts occ (pass-2 transitions, the only ones): S3G* fb -> fb occ: repeat->reused 10.38%, macro->reused 2.18%
  (mathcalls.h 2.05, helper 0.10, narrow 0.03) => 81.31 + 12.56 = 93.88%. S2 fb -> fb occ: repeat->reused 10.50%,
  macro->reused 2.30% => 67.44%. No uses/deps transitions: the re-parsed readings had no reusable dependents.
- Pass 1 (fill): no reuse in the first compile of any configuration (0 in all four runs); fill reuse 49.28% (S2 fb),
  61.42% (S2 fb occ), 74.91% (S3G* fb), 86.83% (S3G* fb occ): later compiles of a configuration reusing earlier ones; not suspicious.

### F4a (RUN) Every SV_UNDEFINED function on redis is defined, and (except hiredis dict.h) INSIDE THE SAME UNIT
- Debug plugin plug/BothnerTraceV2.so (copy of the current plugin + a stderr line per flagged FunctionDecl with the owner
  of its definition), run_undef.py on compiles 79,45,57,24,157,182,183 -> undef.out:
  keymeta.h unit [3770,8436) (616 tokens, the whole tail of the header from inside struct KeyMetaClassConf to #endif):
    getNumMeta, getModuleMetaBits: static inline prototypes at 7116/7170, definitions at 8184/8329 IN THE SAME FRAGMENT.
  strbuf.h [1590,4000): 9 static prototypes (strbuf_empty_length ... strbuf_ensure_null), all defined (static inline) in the same fragment.
  hiredis dict.c [2124,10647) (included by async.c): 4 static prototypes, defined in the same fragment.
  hiredis dict.h [4081,4625): 10 static prototypes (dictCreate ...), defined in dict.c's fragment [2124,10647) of the same compile.
  redismodule.h [98820,116577): RedisModule_Init static prototype, defined in the same fragment.
  vector-sets fastjson.c [1406,16158): jsonSkipValue, jsonParseValueToken: same fragment.
- So the plugin flags per bodiless DECLARATION (`!FD->doesThisDeclarationHaveABody()`), while the design 2.8 says "fragments that
  declare an internal or inline function without defining it". 82 of the 83 flagged instances define the function themselves;
  restoring them restores the definition, so the -Wundefined-internal/-inline hazard (function ends the compile undefined)
  cannot arise from them. Only dict.h (1 instance, async.c) relies on a later unit (dict.c) for the definition.

### F4b (RUN) Loss from 2.8's rule by flagged file (task4.py / task4.out): counterfactual = design with SV_UNDEFINED kept only
### on one file's units; "nothing flagged" reproduces " fb" exactly (2.1745x etc.); per-file losses add up exactly
- S3G* (option 3 + groups*): total 1145.25 ms = 11.46% of header time (pass 2), 1121.90 ms in the first build; 2.1745x -> 1.8753x.
  keymeta.h (75 inst): 1105.57 ms (11.07%; alone 1.8843x) = the unit itself 57.51 ms (66 inst; reused in 66 compiles without the
    rule) + dependents 1048.05 ms (896 inst, all reused->uses) in 66 compiles: server.h 933.8, stream.h 48.2, rdb.h 30.3,
    cluster.h 26.2, script.h 4.1, cluster_asm.h 2.6, functions.h 2.0, cluster_slot_stats.h 0.4 ms.
  redismodule.h (3 inst; tls.c, vset.c, vset_config.c): 37.86 ms = units 30.88 ms (2 inst) + dependents 6.99 ms (vector-sets expr.c).
  strbuf.h (2 inst, lua): 1.81 ms, the units themselves. dict.c, dict.h, fastjson.c: 0 (not reused even without the rule).
  => "all through keymeta.h" is 96.5% (1105.6 of 1145.3 ms); redismodule.h is 3.3%. Root attribution in the log
     (uses<-undefined keymeta.h 10.9%) vs counterfactual dependents 10.49% + itself 0.58%: consistent (first-failing-use attribution).
- S3 and S3G: 39.68 ms (0.40%) = redismodule.h 37.86 + strbuf.h 1.81; keymeta.h 0 (its span is refused for 'uses' anyway, root
  struct_FILE.h). S2 and S2G: 1.81 ms = strbuf.h only (keymeta.h's option-2 fragment starts inside struct KeyMetaClassConf
  after the #define lines -> nested).
- With F4a: refining SV_UNDEFINED to "no definition in the same unit" would unflag 82/83 instances (all but dict.h, which loses
  0 anyway) and give the fb numbers (2.17x) with no fallback needed on redis.

### F5 (RUN) Scoring sanity (task5.py / task5.out, misc.py / misc.out)
- For all 20 runs x 5 passes x 187 compiles: saved <= the compile's header time (option 2: sum of frag ns; option 3: + span
  define ns), saved_def <= saved, saved_def <= define time, saved_def = 0 in option-2 runs, why_ns['reused'] == saved, and
  sum(why_ns) == header time (empty units carry 0 ns). No violation. Max saved/header = 1.0 (S3G* fb occ tok, compile 183).
- Timing records: frag_ns vs sum of frag ns differs by -7..+46 ns (rounding), span == frag + sum(own_ns) within 2 ns.
- Below the floor (floor = main+main_dir+hdr_dir+pre+end of the timing run, LESS the replayed #define time for option 3;
  the raw floor is wrong for option 3 because the define time it contains is saved): SCALE 1: S3G*, S3G* old, S3G* fb,
  S3G* fb tok pass 2: crccombine.c (15.18 < 15.21 ms); S3G* fb occ (and occ tok) pass 2: crccombine.c (15.06 < 15.10) and
  hyperloglog.c (46.41 < 47.36), pass 1: hyperloglog.c. Nothing for option 2 or S3/S3G. SCALE 1.007: same compiles.
  Why: pred - floor' = (plain - span) + (unsaved fragment time) - 1.65 ms credit; plain wall - timing-run span is 0.64 ms
  (crccombine.c) and 0.24 ms (hyperloglog.c), the only two compiles under the 1.65 ms credit (median 7.32 ms).
  Fit over 187 compiles: plain - span = 7.64 ms - 14.26 ns/token x tokens (R^2 0.53, residual sd 0.87 ms): the trace's
  per-token cost inflates the timing span (~1% at 400k tokens, matching D7's +0.9%); these two are also -1.6/-2.6 ms residuals.
  Effect of clamping to the floor: <1 ms per compile, negligible on totals.
- Restored declarations per compile, pass 2 (median / max / total): S2 655/7142/240,953; S3 821/8175; S2G 775/7583;
  S3G 849/8221; S3G* 974/8710/325,776; S3G* fb 1013/10380/437,305; S2 fb occ 932/8855; S3G* fb occ 1162/12093/493,031;
  S2 fb tok 617/7071; S3G* fb tok 953/10380; S3G* fb occ tok 1121/12093; "old" == fixed. Passes 3-5 equal pass 2 in all runs.
  restored == sum of the reused units' binds in 187/187 compiles (S2, S3G* fb); restored/all binds median 0.468 (S2), 0.966 (S3G* fb).
- Fix 1 count (misc.out): exactly 1 unit in the build names a main-file declaration made before it: zmalloc.c's zmalloc.h:3234
  (zlibc_free), flags 11 -> refused as 'main' in S2 and S3G* fb, as before the fix.
