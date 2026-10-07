# Header cache: optimizations and open questions

Written 2026-10-07, to work from on 2026-10-08. Every optimization the
project has built or proposed, sorted into techniques and implementation
fixes, and every open question in one numbered list. Reasons and full
numbers live in `DECISIONS.md` (cited as §) and `NEXT.md`. Benchmark data:
`scratch/e2e/multi/results-2026-10-07/` (`multi4/` for the builds, `ceil/`
for the ceiling runs); scripts beside it (`replay_bench.py`,
`ceiling_run.py`, `ceiling_report.py`).

## Suggested order

1. Commit the three bug fixes (Q6). Everything since `177f5e00d` is
   uncommitted.
2. Decide the target and the finding (Q1, Q2). They change which numbers the
   paper reports and which work comes next.
3. Confirm the sorting below (Q4) and the order of overhead work (Q5).
4. Close the superseded items (Q28 to Q33); most are a yes.

## 1. Where things stand

A first build: an empty daemon, every compile of the project once, one at a
time, pinned to one core, plain clang and the cache interleaved file by
file. Speedup is plain time over cache time. Medians of three builds.

| Project | Compiles | Plain clang, full build | Cache, full build | Speedup, full build | Speedup, front end only |
|---|---|---|---|---|---|
| git | 567 | 174.14 s | 141.26 s | 1.23x | 1.43x |
| curl | 479 | 54.68 s | 44.12 s | 1.24x | 1.44x |
| postgres | 1,341 | 253.66 s | 230.64 s | 1.10x | 1.17x |
| cpython | 401 | 175.57 s | 158.50 s | 1.11x | 1.30x |
| nginx | 126 | 21.59 s | 19.62 s | 1.10x | 1.24x |
| redis-server | 117 | 31.83 s | 29.95 s | 1.06x | 1.20x |

Full build: each project's own flags, code generation included. Front end
only: `-fsyntax-only`. redis's row is §2.23's run alone on the machine. The
other front-end numbers come from builds that shared the machine with 17
others (Q24).

Correctness: no compile fails; every object is byte-identical to plain
clang's except three cpython objects, which differ in one debug-line column
each (Q7).

**The ceiling** (§2.25). If every hit cost nothing and every header were
skipped, a full build could be 1.33x (redis) to 1.84x (git) faster: headers
are 25% to 46% of a full build. The front end alone could be 3.3x to 7.7x
faster: headers are 70% to 87% of it. Your 2.5x to 3x fits the front end.

**The gain** = header time skipped − the cache's own time. The cache already
skips 65% to 94% of header time in full builds. For every second it skips,
it spends 0.47 s to 0.74 s of its own: looking up and loading copies,
replaying macros, writing copies. Most of the remaining speed is on that
side.

## 2. Optimizations: techniques and implementation

**How they are sorted.**

- **Technique:** changes what the cache decides, records or stores. It would
  carry over to another compiler or another file format, and it needs its
  own argument and its own measurement: a subsection of the paper each.
- **Implementation fix:** removes a cost that comes from how this cache is
  built -- its daemon protocol, or Clang's PCM reader and writer. Another
  implementation would not pay it. The paper covers these together, in one
  table.

### 2.1 Techniques

| # | Technique | Mechanism | Status | Measured effect | Which side of the gain |
|---|---|---|---|---|---|
| T1 | Dependencies on how identifiers expanded | For each identifier a header's parse considered for expansion, a copy records whether it was not a macro or which tokens it expanded to; a hit requires the same here. The whole macro table is never compared (§1.12) | Built 2026-09-18 | With T2: redis `anet.c` served 18 of 18 lookups, identical assembly (§1.5) | Hits and correctness |
| T2 | Dependencies face outward | A definedness test is recorded only if the state it saw came from outside the header and the headers inside it (§1.5) | Built 2026-09-30 | `anet.c`: rejections of the form "recorded defined, live undefined" 42 to 2 | Hits |
| T3 | An `#if` depends only on the tests its result needed | Clang's `#if` evaluator reports which `defined()` operators decided the value; the others are not recorded. `a \|\| b` with `a` true needs only `a` (§2.22) | Built 2026-10-06 | redis: rejections over `NULL` 210 to 0 | Hits |
| T4 | Outside declarations by name | A copy refers to a declaration its includer already had by its name and a fingerprint (layout, enumerator values, typedef alignment, initializers); the loading compile resolves the name to its own declaration (§2.18) | Built 2026-10-05 | Lets a copy serve compiles that parsed the includer's declarations as text; 72 compiles identical | Hits |
| T5 | The compile that misses writes the copy | At a miss the real compile parses the header as text, then writes the copy from its own state; no second compile, no build-system change (§2.17) | Built 2026-10-05 | Makes a first build possible; writing costs 0.15 s (nginx) to 22.96 s (postgres) per full build | Both |
| T6 | Fold everything | A copy holds the header and every header inside it. Inside a header being written, nothing is asked or served (§2.23) | Built 2026-10-06 | redis: hits per build 2,687 to 302; front end 1.20x, full build 1.06x (both were about 1.0x before) | The cache's own time |
| T7 | Ask only where a hit can pay | 77.0% of header parses cost less than one load; the other 23.0% hold 98.5% of header parse time. The daemon lists headers whose first parse was heavy; a compile never asks about the rest (§2.14) | Built 2026-09-29. A per-header cutoff built 2026-10-06 is off: no gain (§2.20) | Listing headers by a heavy first parse covers 90.0% of later parse time | The cache's own time |
| T8 | Handing macros back | A hit replays the header's `#define`s and `#undef`s (§2.16). Proposed option B: each macro's tokens in our metadata, definitions created directly at the hit, effects already in force skipped (§2.24) | Replay built 2026-09-29; B proposed | Replay is 0.68 s (redis) to 17.40 s (git, 5 ms per hit) of a full build | The cache's own time |
| T9 | Check at the `#define`, not at the include | Adapted from Bothner (2003). The daemon keeps every name any copy uses as a plain name; a compile notes when it `#define`s one; a hit checks only that short list instead of about 500 names (§4) | Proposed 2026-09-22, not approved | Short list: median 9 (FFmpeg) to 114 (qemu) names, once self-named macros like `#define stdin stdin` are left out | The cache's own time |
| T10 | Find the fitting copy without checking each | New. postgres checked and rejected 51,711 copies in 11,280 lookups, 4.6 per lookup. The daemon could index a header's copies by the macros that tell them apart and send only the one that fits | Idea, first raised here | Some of postgres's 18.96 s of lookups | The cache's own time |
| T11 | Linked copies | A copy holds its header's own lines and links to other copies, instead of folding them in (NEXT.md 6) | Not built: each linked PCM costs about 0.3 ms to open and slows name lookups | Ceiling with every header's first parse kept, linked against folded: full builds within 0.05x; front end redis 3.27x against 2.80x, cpython 4.49x against 3.90x | Hits, memory |
| T12 | A format built for loading | Your "beyond PCM": load declarations and types at a finer grain than PCM's reader (§2.24 option D) | Proposed, not designed | PCM's reader is 0.88 s of redis's 10.0 s front end: macro decoding 0.30 s, declarations and types 0.25 s, merging 0.02 s | The cache's own time |
| T13 | Relocating `__COUNTER__` | A copy built with the counter at a base no compile reaches; at a hit, each number is rewritten to the live counter plus its offset, as a loader relocates addresses (§2.7, §4) | Proposed 2026-09-29, deferred | Would serve qemu 5,377 of 5,924 includes declined now, the Linux kernel 10,145 of 11,319 | Hits |
| T14 | Reusing compiled header functions | A header's `static inline` functions are compiled and optimized again in every `.c` file that uses them; Zapcc caches generated code for this reason. Beyond a header cache | Idea | Not measured. The only way past 1.84x on full builds (§2.25) | Beyond the ceiling |

**Findings, not optimizations** (for the evaluation and discussion):

- **F1, the ceiling** (§2.25): how much of a build headers are, and how much
  a free cache could save.
- **F2, a file format against a compile server** (§2.24). Per hit, a server
  that keeps parsed headers in memory always wins: about 2.7 s of redis's
  10.0 s front end is the format's cost. Per build, it runs one server per
  parallel job, and they share nothing. The crossover in `-j` is not
  measured (Q2).

### 2.2 Implementation fixes

Savings are estimated on a one-thread first build of redis-server
(§2.21, §2.24), not measured.

| # | Fix | Removes | Estimated saving | Status |
|---|---|---|---|---|
| I1 | One index of names over the loaded PCMs (Clang's global module index as the model) | Each name searched for in every loaded PCM's table; most re-searches after a load | About 0.25 s of the front end with folding (1.0 s before) | Proposed (§2.24 A) |
| I2 | Check options once per compile; drop the record-name table | The same option comparisons for every PCM; 12 KB in every PCM, 13% of the stored bytes | Under 0.05 s, and 13% of the bytes | Proposed (§2.24 C) |
| I3 | Metadata in shared memory, replies carrying offsets; or no socket at all | Encoding, copying and decoding a copy's metadata on every query | About 0.3 s | Proposed; the no-socket form not approved (NEXT.md 3) |
| I4 | Remember header searches and declaration checks within a compile | The 48% of header searches and 49% of declaration checks that repeat | About 0.2 s | Proposed (§2.21) |
| I5 | Write a copy from its own source range | Four scans of the compile's whole state per copy written: 2.4 of its 4.0 ms | About 0.6 s | Proposed (§2.21) |
| I6 | Track dependencies only around headers that will be written | Most of the tracking | About 0.2 s | Proposed (§2.21) |
| I7 | Send a copy to the daemon without waiting for the acknowledgement | The wait | About 0.15 s, with I2 | Proposed (§2.21) |
| I8 | Replay: find each effect's name in its own PCM; skip effects already in force | Searching every loaded PCM per effect | Some of 0.78 s | Proposed; T8's option B would replace it |
| I9 | Check files once per build, in the daemon | Every compile checking the same files and redoing the same header searches | 0.39 s of a 30.3 s full build | Deferred by you (§2.19) |
| I10 | Overlap the lookup's round trip with resolving the `#include`; batch lookups | Tens of microseconds per `#include` | Small | Idea (§4) |
| I11 | Enter all of a loaded copy's names at load, as Clang does for C++ | Lazy searches at later lookups | Unknown: an experiment | Proposed (§2.24) |
| I12 | Cross-reference IDs for outside declarations | Resolving names at load | Unknown | Not started (NEXT.md 1) |

**What the fixes add up to** (§2.25's projection). I1 to I8 together with
T8's option B, cutting lookups by 50%, replay by 70%, writing by 70% and
slower parsing by 60%:

| Project | Full build, now | Full build, all fixes | Front end, now | Front end, all fixes | Front end, all fixes and copies that fit |
|---|---|---|---|---|---|
| redis | 1.05x | 1.13x | 1.19x | 1.56x | 1.92x |
| nginx | 1.10x | 1.25x | 1.24x | 1.75x | 2.07x |
| curl | 1.24x | 1.41x | 1.44x | 2.04x | 2.17x |
| git | 1.23x | 1.50x | 1.43x | 2.60x | 3.04x |
| postgres | 1.10x | 1.28x | 1.17x | 1.67x | 2.17x |
| cpython | 1.11x | 1.20x | 1.30x | 1.79x | 2.38x |

"Now" here is from the ceiling runs; redis's full build ran with six others
on the machine (1.05x; 1.06x alone). "Copies that fit": every header served
except its first parse in the build -- optimistic where macros really
differ, such as postgres's `FRONTEND`.

## 3. Open questions

### A. Direction (decide first)

1. **Q1. The target: front end, full build, or both?** Full builds cannot
   pass 1.84x through a header cache; the front end can reach 2x to 3x with
   the fixes above. Recommendation: report both, and set the 2.5x goal on
   the front end.
2. **Q2. The finding, and the crossover experiment.** You proposed:
   server-client cannot outperform monolithic. Measured: true per hit, not
   shown per build. The experiment changes the benchmark script only: send
   compile i to daemon i mod N (each daemon sees only its own copies, the
   hit rate of N servers), subtract the format's cost per hit, and compare
   against our cache at each `-j`. Approve?
3. **Q3. Keep PCM as the format?** Deferred 2026-10-06 (§4). PCM's reader is
   0.88 s of redis's 10.0 s front end; merging is only 0.02 s, so most of
   the cost is lookup and decoding, which I1 and T8 B attack without leaving
   PCM. T12 is the way out if they are not enough.
4. **Q4. Is the sorting right?** Three are borderline:
   - T8 option B is a technique if the paper argues how macros should come
     back. It is an implementation fix if it is only "PCM's macro tables are
     slow".
   - T12 is a technique only if it ends up as a new kind of format.
   - I9 resembles Clang's build-session validation, so it is sorted as
     implementation.
5. **Q5. The order of overhead work.** Largest measured costs first:
   - macro replay, T8 B: git 17.40 s;
   - writing, I5: postgres 22.96 s, cpython 7.87 s;
   - lookups, I1, I3, I4 and T10: postgres 18.96 s, git 10.35 s.

### B. Correctness decisions

6. **Q6. Commit and push the three bug fixes now?** A copy kept the
   writing compile's own files; a copy was written before its last
   function's body; a copy's variable definitions were not emitted
   (DECISIONS.md §3). The cpython column (Q7) goes in as a known
   limitation.
7. **Q7. The `-g` column loss.** In three cpython objects the debug-line
   column for `Py_REFCNT(op)` inside `Py_SET_REFCNT(...)` points at the outer
   macro. The cause: a macro expansion spelled in a file outside the folded
   copy keeps only the place it was used. Fix: keep such files as stand-ins
   with no contents, so expansions keep their spelling. A design change.
8. **Q8. `config.c`'s `-g` difference** (§4): `size_t` in debug information
   points at `glob.h`'s redeclaration. Proposed: resolve outside names when
   the copy loads, not lazily.
9. **Q9. Module names collide** (§4): 4 of 245 copies in the whole-redis
   build. Proposed: include the outside declarations' fingerprints in the
   name.
10. **Q10. Refuse to run in `-emit-module` compiles** (§4), so a copy from
    one can never be loaded elsewhere.
11. **Q11. The `pop_macro` hole** (§4): fix agreed (a popped macro becomes
    an expands-to dependency), not written. Write it?
12. **Q12. `__COUNTER__`.** Copies of headers that use it are declined.
    Whether any of the five projects uses it is not measured; that decides
    whether T13 matters for the paper's benchmarks.

### C. Proposals waiting for approval

13. **Q13. Macro option B** (T8). The largest single overhead in git.
14. **Q14. The eager-names experiment** (I11): enter every name of a loaded
    copy at load and measure.
15. **Q15. The check at the `#define`** (T9). Its value depends on Q26.
16. **Q16. List a header by a typical reading** (§4). The list uses each
    header's first reported parse time. A cold first compile listed
    `stddef.h` (580 us instead of 130 us), which was then served 2,043 times
    at about 1 ms a hit, though its text takes 32 us. Proposed: the median
    of its first few readings.
17. **Q17. Check files once per build, and queries without the socket**
    (I9, I3).
18. **Q18. Memory with folding.** `server.h` has 8 folded copies of 1.5 to
    2.4 MB each. The five projects' totals are not measured. If memory
    matters, linked copies (T11) come back.
19. **Q19. Index a header's copies by what tells them apart** (T10). Worth
    a proposal?

### D. Measurements not yet taken

20. **Q20. Parallel first builds of the cache** (`-j`). Only one-thread
    builds are measured. Two compiles missing the same header at once
    both write a copy.
21. **Q21. How much of a full build compiles header functions** (T14).
    This decides whether a full build can pass 1.84x at all.
22. **Q22. `-O0 -g` builds**, what a developer builds while editing. The
    header share is higher there, so the ceiling is too; not measured.
23. **Q23. Which macros reject copies in postgres.** "A macro's definedness
    differs" rejected 44,786 copies; 3,108 copies were written for 1,341
    compiles. Genuine differences, such as `FRONTEND`, or avoidable ones?
24. **Q24. Rerun the front-end builds alone.** They shared the machine with
    17 other builds.
25. **Q25. The projects' own test suites with the cache on** (§4), as the
    paper's correctness evidence beyond identical objects.
26. **Q26. What the roughly 500 not-a-macro checks per hit cost** (§4).
    This decides Q15.
27. **Q27. The cost of `-fmodules -fno-implicit-module-maps` itself** on the
    release build across the projects. §2.8 measured it on two files with a
    debug build. Both modes run with it, so it changes the baseline, not
    the comparison.

### E. Probably superseded: confirm and close

28. **Q28. §2.1, the separate sub-compile** (not approved): replaced by T5,
    the compile that misses writes the copy.
29. **Q29. §2.15's fold-by-parse-time mechanism** (generated module maps,
    merging of metadata): replaced by T6, fold everything, plus T7's ask list.
30. **Q30. Layout D and its two measurements** (NEXT.md, "The PCM seam"):
    fold everything stores one folded copy per header and include order.
    What is left of the layout question is memory (Q18).
31. **Q31. Producer-era items in §4**: a compile queuing a request it
    already rejected; a population request that always fails; the token
    tier's duplicate payload.
32. **Q32. Remove the producer, job recording and the populate tool**
    (NEXT.md 5), once Q28 is closed.
33. **Q33. A dynamic fold decision** (§4): with fold everything, cost no
    longer decides folding. What is left of it is Q16.
