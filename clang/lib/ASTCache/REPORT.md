# Header cache: correctness, optimizations and open questions

Written 2026-10-07 and updated the same evening, to work from on
2026-10-08. The correctness problems explained and ranked (section 2), every
optimization ranked by the time it could remove, measured (section 3),
sorted into techniques and implementation fixes (section 4), the proposed
switches for ablation (section 5), and every open question (section 6).
Reasons and full numbers live in `DECISIONS.md` (cited as §) and `NEXT.md`.
Benchmark data: `scratch/e2e/multi/results-2026-10-07/`; scripts beside it.

## Order of work (decided 2026-10-07)

User: "Correctness first, then we optimize based on the seriousness of the
problem"; "Let's do implementation over technique first so we keep
optimization independent on the technique side"; "save a commit per
technique, or a way to turn techniques on and off, for ablation studies".

1. The correctness problems, in section 2's order, one at a time, each
   approved before it is built.
2. The implementation fixes, in section 3.1's order.
3. The techniques, in section 3.2's order, each in a commit of its own and
   behind an off switch (section 5).

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
each (Q7). The three fixes behind this are pushed to the fork (`3716d1a71`).
Section 2 lists what the benchmarks do not reach.

**The ceiling** (§2.25). If every hit cost nothing and every header were
skipped, a full build could be 1.33x (redis) to 1.84x (git) faster: headers
are 25% to 46% of a full build. The front end alone could be 3.3x to 7.7x
faster: headers are 70% to 87% of it. Your 2.5x to 3x fits the front end.

**The gain** = header time skipped − the cache's own time. The cache already
skips 65% to 94% of header time in full builds. For every second it skips,
it spends 0.47 s to 0.74 s of its own: looking up and loading copies,
replaying macros, writing copies. Most of the remaining speed is on that
side.

## 2. Correctness problems, explained and ranked

Each was reproduced on 2026-10-07 with a few lines of C, a fresh daemon that
lists every header, and plain clang on the same file
(`scratch/e2e/qtests/`: one directory per problem, `harness.py` runs them). Ranked by how much they
change and how often they can happen.

| Rank | Problem | What changes | How often |
|---|---|---|---|
| 1 | Q34, Q35: a header's diagnostics are lost on a hit | Warnings disappear; under `-Werror` a compile plain clang fails succeeds | Any header with a warning |
| 2 | Q9: two copies get one name | Silent wrong code | Needs one unguarded header twice in a compile, with an outside declaration changed in between |
| 3 | Q11: `pop_macro` of a value pushed outside | Silent wrong code | No header in the six projects does it |
| 4 | Q10: the cache runs inside module builds | Clang crashed | Only builds that build Clang modules |
| 5 | Q8: outside names resolved late | Different but valid code; debug lines move | A redeclaration after the `#include` |
| 6 | Q7: a macro argument's column | One debug-line column | 3 of 401 cpython objects |
| 7 | Q12: `__COUNTER__` | Nothing: such headers are not cached | No header in the six projects uses it |

Checked and fine: parallel first builds. Eight compiles at a time sharing one
daemon (`par_check.py`): git, curl and postgres have no failures and
byte-identical objects; cpython differs only in the three Q7 columns and in
`getbuildinfo.c`, which embeds the time it was compiled.

### Q34 and Q35: a header's diagnostics are lost on a hit

**Mechanism.** Clang prints a warning while it parses the text that causes
it. A hit skips the header's text, so its warnings never print. Clang's own
modules behave the same way. Separately, the compile that reads a header as
text writes its copy when the header ends, whether or not that compile has
failed (Q35). A hard error marks the copy, and the reader refuses it later
("module file ... contains compiler errors", printed as an extra error). A
warning that `-Werror` turned into an error does not mark it.

**Reproduced.** `w.h`: `static inline int w(int x) { int unused_local; return x; }`.

| Flags | Plain clang on `b.c` | Served `b.c` |
|---|---|---|
| `-Wall` | warning: unused variable `unused_local` | no warning |
| `-Wall -Werror` | error, exit 1 | no diagnostic, exit 0 |

Under `-Werror`, the first compile fails but writes the copy, and every
later compile is served it and succeeds.

**Fixes.** Q35: write nothing after an error, `-Werror` ones included
(small). Q34: either never write a copy of a header that warned, or store the
header's diagnostics in the metadata and print them again on a hit, as
ccache replays a compile's output. Warnings depend on `-W` flags, so a stored
copy would also have to match them.

### Q9: two copies of a header get one name

**Mechanism.** A copy is named by a hash of its header, the compiler options
and its metadata, taken before the outside declarations it refers to are
added. Two copies that differ only in those declarations get one name. The
daemon keeps both. Inside one compile, the hook remembers loaded copies by
name, so an unguarded header such as `stddef.h` included twice reuses its
first copy; with a collision, the second hit gets the wrong copy.

**Reproduced.** Unguarded `h.h`: `extern __typeof__(f) *fp;`. `a.c` has
`int f();` before it, `b.c` has `int f(int);`: two copies, one name. `c.c`
includes `h.h` after `int f();` and again after `int f(int);`, then calls
`fp(1.0f)`. Plain clang: the second include redeclares `fp` as
`int (*)(int)` and the call passes the int 1. Served: the second hit reuses
the first copy, `fp` stays unprototyped, and the call passes the double 1.0:
the callee reads garbage. No diagnostic.

**Seen.** 4 of 245 copies in the whole-redis build share a name (§4); no
wrong code in the benchmarks.

**Fix.** Name a copy after its outside declarations are known (their
fingerprints in the hash). Small.

### Q11: a header pops a macro its includer pushed

**Mechanism.** `#pragma push_macro("X")` saves X's definition on a stack;
`#pragma pop_macro("X")` restores it. A header that pops X depends on what
its includer pushed, but a hit compares only the macro state at the
`#include`, never the stack. And `pop_macro` defines X without the callback
the cache records effects from, so the restored X is not replayed either.

**Reproduced.** `p.h`: `#pragma pop_macro("X")`, then
`static inline int px(void) { return X; }`. `a.c` pushes `X` = 1 and sets
`X` = 1; `b.c` pushes `X` = 5 and sets `X` = 1. Both have `X` = 1 at the
`#include`, so `b.c` is served `a.c`'s copy. `b.c` returns `px() * 100 + X`:
plain clang 505, served 140, with a new "ambiguous expansion" warning.

**Seen.** No header in the six projects pops a value pushed outside it.
`xxhash.h` pushes and pops its own, in a branch not compiled on x86.

**Fixes.** Decline a header that uses `push_macro` or `pop_macro` (smallest;
loses almost nothing), or record the stack as a dependency and a pop as an
effect.

### Q10: the cache runs inside compiles that build a module

**Mechanism.** A compile that builds a Clang module (`-emit-module`, or one
Clang starts to build a module implicitly) looks like any other compile to
the hook. It writes copies there and serves them to later module builds.
`__building_module(M)` in an `#if` is not recorded either.

**Reproduced.** A module whose one header tests `__building_module(M)`, built
three times through the cache: the second build wrote a copy, and the third
was served it and crashed (segmentation fault, "malformed or corrupted
precompiled file: 'too many submodules'"). An ordinary compile was not
served the module build's copy: the reader refused it.

**Fix.** The hook does nothing in a compile that builds a module. Small.

### Q8: an outside declaration is resolved when first needed

**Mechanism.** A copy refers to a declaration its includer already had, such
as `fatal` declared in the `.c` file before the `#include`, by name. The
loading compile resolves the name lazily, when the code using it is first
needed, often at the end of the translation unit. By then the name finds its
latest redeclaration, which may come after the `#include`. Plain clang bound
the reference where the header was parsed.

**Reproduced.** `b.c`: `void fatal(const char *);`, then `#include "h.h"`
(whose `static inline int check(int)` calls `fatal`), then
`_Noreturn void fatal(const char *);`. Served: the call is `noreturn` and the
code after it is unreachable. Plain clang: an ordinary call. In the same run,
a typedef redeclared after the header moved `size_t`'s debug line from 1 to
5, which is redis's `config.c` difference.

**Is it wrong?** The extra facts come from the program's own later
declarations, so the code stays valid for a correct program. But objects
differ from plain clang's and debug lines move.

**Fix.** Use, for each name, the declaration the hit's check already looked
up at the `#include` (the check finds every outside declaration a copy
needs, to compare fingerprints). That pins what plain clang would have
bound, and costs no lookup of its own: the lazy resolutions it replaces take
about 0.5 us each, at most 0.03 s per project.

### Q7: a column inside a macro argument

**Mechanism.** A copy drops the macro expansions spelled in files outside it
and keeps only where they were used. Debug information reports a token in a
macro's argument where the argument was written; with the macro taking the
argument dropped, that falls back to where the macro was used.

**Seen.** cpython's `Py_SET_REFCNT(op, Py_REFCNT(op) - 1)` in
`pycore_object.h:701`: column 23 in plain clang, 5 served. One byte in three
of 401 objects; code identical.

**Fix.** Keep a dropped file as a stand-in with no contents, so expansions
keep their spelling. A design change.

### Q12: `__COUNTER__`

Not a correctness problem now: a header that uses `__COUNTER__` is never
saved. None of the six projects' headers uses it; in `/usr/include` only
glib's `gmacros.h` does. It matters for qemu and the Linux kernel (§2.7).

## 3. Optimizations ranked by what they could remove

**How it was measured.** Each project's first build, its own compile
commands with code generation, through the cache one compile at a time,
with temporary timers around every step of the cache's work (the patch is
kept in `scratch/e2e/multi/proftmp/` and is not in the tree;
`prof_run.py`, `prof_report.py`). The timers added 7% to 37% to the steps
they timed; every number below is scaled back by the ratio of the cache's
own coarse timers (lookups, replay, writing) with and without them.
Seconds are summed over the six projects' first builds, whose plain-clang
total is 711.5 s. "Could remove" is the time the step takes now: an upper
bound on what the fix saves.

### 3.1 Implementation fixes

| Rank | Fix | What it removes | Could remove | Share of plain time |
|---|---|---|---|---|
| 1 | I8: skip replayed macro effects already in force | Finding each replayed macro's name, for effects that change nothing | 30.2 s | 4.3% |
| 2 | I5: write a copy from its own range | Scanning the whole compile's source locations, macros, identifiers, comments and declarations for each copy written | 19.4 s | 2.7% |
| 3 | I9: check files once per build | Header searches and file stamps every compile redoes | 17.0 s | 2.4% |
| 4 | I3: metadata in shared memory | Encoding, sending and decoding each saved copy's metadata at every query | 12.1 s | 1.7% |
| 5 | I1: one index of names over the loaded copies | Searching every loaded copy's name table for a name | 11.5 s | 1.6% |
| 6 | I7: send a copy without waiting | Waiting for the daemon to acknowledge each copy | 8.4 s | 1.2% |
| 7 | I12: cross-reference IDs | Looking outside declarations up by name | 1.9 s | 0.3% |
| 8 | I13: time headers only where needed | The hook timing every header for its ask list | 1.6 s | 0.2% |
| 9 | I4: remember searches within a compile | The searches a compile repeats (inside I9) | 1.1 s | 0.2% |
| 10 | I2: check options once, no record-name table | Option checks at every load; 12 KB per copy | 0.8 s | 0.1% |
| 11 | I6: track only headers being written | Tracking in headers no compile writes; most of what was timed was the timers | under 0.6 s | under 0.1% |
| -- | I11: enter a copy's names at load | Measured, both ways in one run: -1.2% to +1.6%, no gain. Drop | none | -- |

Notes on the top three:

- **I8.** One replayed effect in 16 was sampled: 1,040,788 of 1,040,789
  were already in force. Once a copy loads, its own macros are visible, and
  re-applying them changes nothing; the time is spent finding each name
  among the loaded copies. The rare effect that does change something -- an
  `#undef` of a macro the includer defined, or a name another loaded copy
  also defines -- has to be found by a test cheaper than the lookup.
- **I5.** Of what the writer visits for each copy, 98% to 99.6% of the
  source-location entries and 82% to 98% of the macros belong to other
  headers. A copy's own range is known when the header ends.
- **I9** includes I4.

Together, without double counting, the fixes could remove about 103 s.
The cache's six first builds take 624.1 s now (1.14x over plain clang's
711.5 s); without those 103 s, about 521 s (1.37x).

### 3.2 Techniques

| Rank | Technique | What it removes | Could remove | Share of plain time |
|---|---|---|---|---|
| 1 | Better fit (no technique chosen) | Headers still parsed above the first-parse floor: postgres 25.9 s, cpython 12.0 s. Why they miss is Q23 | 49.0 s | 6.9% |
| 2 | T8: macros from metadata (option B) | All macro replay and decoding; about 4.5 s beyond I8 | 34.7 s | 4.9% |
| 3 | T12: a format built for loading | Decoding declarations and types; any format still decodes some | 9.6 s | 1.4% |
| 4 | T14: reusing compiled header functions | Back-end work on a header function after its first compile in the build | 9.2 s | 1.3% |
| 5 | T11: linked copies | First-parse floor, folded minus linked, before each linked copy's own load cost | 6.5 s | 0.9% |
| 6 | T9: check at the `#define` | The not-a-macro checks | 5.2 s | 0.7% |
| 7 | T10: find the fitting copy without checking each | Checks of copies that are rejected | 2.5 s | 0.4% |
| 8 | T13: relocating `__COUNTER__` | Nothing in these six projects | none | -- |

T14 was measured with `-ftime-trace` on plain compiles: a function counts as
a header's when it was parsed while a header was open, and its back-end time
is every trace event naming it (`t14_run.py`). It is small in C: most of a
header function's cost lands in its callers after inlining, where it cannot
be reused.

## 4. Optimizations: techniques and implementation

**How they are sorted.**

- **Technique:** changes what the cache decides, records or stores. It would
  carry over to another compiler or another file format, and it needs its
  own argument and its own measurement: a subsection of the paper each.
- **Implementation fix:** removes a cost that comes from how this cache is
  built -- its daemon protocol, or Clang's PCM reader and writer. Another
  implementation would not pay it. The paper covers these together, in one
  table.

### 4.1 Techniques

| # | Technique | Mechanism | Status | Measured effect | Which side of the gain |
|---|---|---|---|---|---|
| T1 | Dependencies on how identifiers expanded | For each identifier a header's parse considered for expansion, a copy records whether it was not a macro or which tokens it expanded to; a hit requires the same here. The whole macro table is never compared (§1.12) | Built 2026-09-18 | With T2: redis `anet.c` served 18 of 18 lookups, identical assembly (§1.5) | Hits and correctness |
| T2 | Dependencies face outward | A definedness test is recorded only if the state it saw came from outside the header and the headers inside it (§1.5) | Built 2026-09-30 | `anet.c`: rejections of the form "recorded defined, live undefined" 42 to 2 | Hits |
| T3 | An `#if` depends only on the tests its result needed | Clang's `#if` evaluator reports which `defined()` operators decided the value; the others are not recorded. `a \|\| b` with `a` true needs only `a` (§2.22) | Built 2026-10-06 | redis: rejections over `NULL` 210 to 0 | Hits |
| T4 | Outside declarations by name | A copy refers to a declaration its includer already had by its name and a fingerprint (layout, enumerator values, typedef alignment, initializers); the loading compile resolves the name to its own declaration (§2.18) | Built 2026-10-05 | Lets a copy serve compiles that parsed the includer's declarations as text; 72 compiles identical | Hits |
| T5 | The compile that misses writes the copy | At a miss the real compile parses the header as text, then writes the copy from its own state; no second compile, no build-system change (§2.17) | Built 2026-10-05 | Makes a first build possible; writing costs 0.15 s (nginx) to 22.96 s (postgres) per full build | Both |
| T6 | Fold everything | A copy holds the header and every header inside it. Inside a header being written, nothing is asked or served (§2.23) | Built 2026-10-06 | redis: hits per build 2,687 to 302; front end 1.20x, full build 1.06x (both were about 1.0x before) | The cache's own time |
| T7 | Ask only where a hit can pay | 77.0% of header parses cost less than one load; the other 23.0% hold 98.5% of header parse time. The daemon lists headers whose first parse was heavy; a compile never asks about the rest (§2.14) | Built 2026-09-29. A per-header cutoff built 2026-10-06 is off: no gain (§2.20) | Listing headers by a heavy first parse covers 90.0% of later parse time | The cache's own time |
| T8 | Handing macros back | A hit replays the header's `#define`s and `#undef`s (§2.16). Proposed option B: each macro's tokens in our metadata, definitions created directly at the hit, effects already in force skipped (§2.24) | Replay built 2026-09-29; B proposed | All macro work: 34.7 s over six first builds, 4.9% (section 3.2); 0.68 s (redis) to 17.40 s (git) | The cache's own time |
| T9 | Check at the `#define`, not at the include | Adapted from Bothner (2003). The daemon keeps every name any copy uses as a plain name; a compile notes when it `#define`s one; a hit checks only that short list instead of about 500 names (§4) | Proposed 2026-09-22, not approved | The checks it replaces: 5.2 s over six first builds, 0.7%. Short list: median 9 (FFmpeg) to 114 (qemu) names, once self-named macros like `#define stdin stdin` are left out | The cache's own time |
| T10 | Find the fitting copy without checking each | New. postgres checked and rejected 51,711 copies in 11,280 lookups, 4.6 per lookup. The daemon could index a header's copies by the macros that tell them apart and send only the one that fits | Idea, first raised here | Checks of rejected copies: 2.5 s over six first builds, 0.4%, most in postgres (1.8 s) | The cache's own time |
| T11 | Linked copies | A copy holds its header's own lines and links to other copies, instead of folding them in (NEXT.md 6) | Not built: each linked PCM costs about 0.3 ms to open and slows name lookups | First-parse floor, folded minus linked: 6.5 s over six first builds, 0.9%, before each linked copy's own load cost | Hits, memory |
| T12 | A format built for loading | Your "beyond PCM": load declarations and types at a finer grain than PCM's reader (§2.24 option D) | Proposed, not designed | Decoding declarations and types: 9.6 s over six first builds, 1.4%, an upper bound | The cache's own time |
| T13 | Relocating `__COUNTER__` | A copy built with the counter at a base no compile reaches; at a hit, each number is rewritten to the live counter plus its offset, as a loader relocates addresses (§2.7, §4) | Proposed 2026-09-29, deferred | None in the six projects; would serve qemu 5,377 of 5,924 includes declined now, the Linux kernel 10,145 of 11,319 | Hits |
| T14 | Reusing compiled header functions | A header's `static inline` functions are compiled and optimized again in every `.c` file that uses them; Zapcc caches generated code for this reason. Beyond a header cache | Idea | 9.2 s over six first builds, 1.3% (section 3.2) | Beyond the ceiling |

**Findings, not optimizations** (for the evaluation and discussion):

- **F1, the ceiling** (§2.25): how much of a build headers are, and how much
  a free cache could save.
- **F2, a file format against a compile server** (§2.24). Per hit, a server
  that keeps parsed headers in memory always wins: about 2.7 s of redis's
  10.0 s front end is the format's cost. Per build, it runs one server per
  parallel job, and they share nothing. The crossover in `-j` is not
  measured (Q2).

### 4.2 Implementation fixes

What each could remove is measured in section 3.1; the estimates on redis
that stood here (§2.21, §2.24) are superseded.

| # | Fix | Removes | Could remove (section 3.1) | Status |
|---|---|---|---|---|
| I1 | One index of names over the loaded PCMs (Clang's global module index as the model) | Each name searched for in every loaded PCM's table; most re-searches after a load | 11.5 s | Proposed (§2.24 A) |
| I2 | Check options once per compile; drop the record-name table | The same option comparisons for every PCM; 12 KB in every PCM, 13% of the stored bytes | 0.8 s, and 13% of the bytes | Proposed (§2.24 C) |
| I3 | Metadata in shared memory, replies carrying offsets; or no socket at all | Encoding, copying and decoding a copy's metadata on every query | 12.1 s | Proposed; the no-socket form not approved (NEXT.md 3) |
| I4 | Remember header searches and declaration checks within a compile | The 48% of header searches and 49% of declaration checks that repeat | 1.1 s | Proposed (§2.21) |
| I5 | Write a copy from its own source range | Four scans of the compile's whole state per copy written: 2.4 of its 4.0 ms | 19.4 s | Proposed (§2.21) |
| I6 | Track dependencies only around headers that will be written | Most of the tracking | under 0.6 s | Proposed (§2.21) |
| I7 | Send a copy to the daemon without waiting for the acknowledgement | The wait | 8.4 s | Proposed (§2.21) |
| I8 | Replay: find each effect's name in its own PCM; skip effects already in force | Searching every loaded PCM per effect | 30.2 s | Proposed; T8's option B would replace it |
| I9 | Check files once per build, in the daemon | Every compile checking the same files and redoing the same header searches | 17.0 s | Deferred by you (§2.19) |
| I10 | Overlap the lookup's round trip with resolving the `#include`; batch lookups | Tens of microseconds per `#include` | Small | Idea (§4) |
| I11 | Enter all of a loaded copy's names at load, as Clang does for C++ | Lazy searches at later lookups | None: measured -1.2% to +1.6% | Measured 2026-10-07; drop |
| I12 | Cross-reference IDs for outside declarations | Resolving names at load | 1.9 s | Not started (NEXT.md 1) |
| I13 | Time headers only where needed | The hook timing every header, for the ask list and the stats | 1.6 s | New, 2026-10-07 |

**What the fixes add up to**: section 3.1, measured. §2.25's projection,
which assumed cuts per kind of work, is superseded.

## 5. Switches for ablation (proposal, not built)

One variable, `CLANG_HEADER_CACHE_OFF`, listing techniques to turn off;
each off state is the behavior before that technique was built, so the paper
can measure each one's share:

| Name | Technique | Off means | Exists |
|---|---|---|---|
| `outward` | T2 | Record every definedness test, wherever its state came from | New |
| `if-result` | T3 | Record every `defined()` an `#if` evaluated | New |
| `by-name` | T4 | Copy outside declarations into the copy | As `CLANG_HEADER_CACHE_OUTSIDE=copy` |
| `fold` | T6 | Ask and serve inside a header being written; a header with a served header inside is not written | New |
| `ask-list` | T7 | Ask about every header | As the daemon's `-ask-cutoff-us 0` |

T1, T5 and the replay of macro effects have no off state: without them the
cache is wrong or writes nothing, so the paper reports them as the base.
Every new technique (T8's option B, T9, T10, ...) lands in a commit of its
own, with a name in the same variable, on by default once approved.

## 6. Open questions

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
5. **Q5. The order of overhead work.** Answered by section 3: measured,
   implementation fixes first.

### B. Correctness decisions

Explained and ranked in section 2; each fix is approved one at a time.

6. **Q6. Commit and push the three bug fixes.** Done: `3716d1a71`, pushed
   to the fork, after checking what they assume (the commit message lists
   it).
7. **Q7. A column inside a macro argument** (section 2, rank 6). Fix is a
   design change: keep dropped files as stand-ins.
8. **Q8. Outside names resolved late** (rank 5). Reproduced in code, not
   only debug information. Fix: use the declarations the hit's check found.
9. **Q9. Two copies get one name** (rank 2). Reproduced as wrong code. Fix:
   name a copy after its outside declarations are known.
10. **Q10. Module builds** (rank 4). Reproduced as a crash. Fix: the hook
    does nothing in a compile that builds a module.
11. **Q11. `pop_macro`** (rank 3). Reproduced as wrong code. Fix: decline
    a header that uses `push_macro` or `pop_macro`, or record the stack.
12. **Q12. `__COUNTER__`.** Not a problem in the six projects: none of their
    headers uses it.
34. **Q34. A served header's warnings are lost** (rank 1, new). Fix: never
    save a header that warned, or store its diagnostics and print them
    again at a hit.
35. **Q35. A compile with errors still writes copies** (rank 1, new). Fix:
    write nothing after an error. Small; fixes the `-Werror` outcome.

### C. Proposals waiting for approval

13. **Q13. Macro option B** (T8). The largest single overhead in git.
14. **Q14. The eager-names experiment** (I11). Done: no gain (section 3.1).
    Close.
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

20. **Q20. Parallel first builds of the cache** (`-j`). Correctness checked
    at `-j8` (section 2): no failures, objects identical. Speed at `-j` not
    measured yet; the check's timing shared cores with other runs.
21. **Q21. How much of a full build compiles header functions** (T14).
    Done: 9.2 s over six first builds, 1.3% (section 3.2). A full build will
    not pass the header ceiling this way.
22. **Q22. `-O0 -g` builds**, what a developer builds while editing. The
    header share is higher there, so the ceiling is too; not measured.
23. **Q23. Which macros reject copies in postgres.** "A macro's definedness
    differs" rejected 44,786 copies; 3,108 copies were written for 1,341
    compiles. Genuine differences, such as `FRONTEND`, or avoidable ones?
24. **Q24. Rerun the front-end builds alone.** They shared the machine with
    17 other builds.
25. **Q25. The projects' own test suites with the cache on** (§4), as the
    paper's correctness evidence beyond identical objects.
26. **Q26. What the not-a-macro checks cost.** Done: 5.2 s over six first
    builds, 0.7% (section 3.2, T9).
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
