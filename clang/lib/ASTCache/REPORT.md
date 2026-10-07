# Header cache: correctness, optimizations and open questions

Written 2026-10-07 and updated the same evening, to work from on
2026-10-08; updated again after your answers (Q8 to Q11, Q34, I8). The correctness problems explained and ranked (section 2), every
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
each (Q7). The three fixes behind this are pushed to the fork (`3716d1a71`),
and so are the fixes for Q35 and Q9 (section 2). Section 2 lists what the
benchmarks do not reach.

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

Each was reproduced with a few lines of C, a fresh daemon that lists every
header, and plain clang on the same file (`scratch/e2e/qtests/`: one
directory per problem, `harness.py` runs them). In each test `a.c` is
compiled first and writes the copy, and `b.c` is served it. Ranked by how
much they change and how often they can happen.

| Rank | Problem | What changes | How often | Status |
|---|---|---|---|---|
| 1 | Q34, Q35: a header's diagnostics are lost on a hit | Warnings disappear; under `-Werror` a compile plain clang fails succeeds | Any header with a warning | Q35 fixed 2026-10-07. Q34 designed, waiting for approval |
| 2 | Q9: two copies of a header share a name | Wrong code, no diagnostic | One unguarded header twice in a compile, an outside declaration changed in between | Fixed 2026-10-07 |
| 3 | Q11: `push_macro` and `pop_macro` | Wrong code, no diagnostic, in three of the four ways headers use them | No header compiled in the six projects uses them | Fix proposed |
| 4 | Q10: the cache runs inside compiles that build a Clang module | Clang crashed | Only builds that use Clang modules | Fix proposed |
| 5 | Q8: a name a copy refers to is looked up later than plain clang looks it up | Different but valid code; debug lines move | A declaration made again after the `#include` | Fix proposed |
| 6 | Q7: a macro argument's column | One debug-line column | 3 of 401 cpython objects | Design change |
| 7 | Q12: `__COUNTER__` | Nothing: such headers are not cached | No header in the six projects uses it | -- |
| -- | Q36 (new): a copy assumes the names it declares are new | A lost warning; for a variable, a compile error | Not measured | Open |

Checked and fine: parallel first builds, 16 compiles at a time sharing one
daemon (`par_check.py`), with the fixes below: no failures, and every object
byte-identical to plain clang's except cpython's Q7 columns and
`getbuildinfo.c`, which embeds the time it was compiled.

### Q34 and Q35: a header's diagnostics are lost on a hit

**Mechanism.** Clang prints a warning while it parses the text that causes
it. A hit skips the header's text, so its warnings never print. Clang's own
modules behave the same way.

**Reproduced** (`qtests/qw`). `w.h`: `static inline int w(int x) { int unused_local; return x; }`.

| Flags | Plain clang on `b.c` | `b.c` served `a.c`'s copy |
|---|---|---|
| `-Wall` | warning: unused variable `unused_local` | no warning |
| `-Wall -Werror` | error, exit 1 | no diagnostic, exit 0 |

**Q35, fixed 2026-10-07.** A compile that had a real error still wrote
copies. Clang marks a module written after an error as holding errors, and a
later compile offered it refused it with an error of its own, printed on top
of the header's (`qtests/qe`). Now nothing is written after a real error. A
warning that `-Werror` made an error is not one: Clang itself does not count
it ("warnings which have been upgraded to errors do not prevent
compilation"), and the header's declarations are what they would have been.
Unit test: `ExportTest.ACompileWithAnErrorWritesNoPart`.

**Q34, design.** Your decision (2026-10-07): store the warnings, "for only
flags that require printing out the error".

1. **Collect.** In the compile that reads a header as text and writes its
   copy, each warning Clang reports while the header is open is saved with
   the copy: which warning, where, its text and its notes.
2. **Print again.** At a hit, each saved warning is reported again through
   the compile's own flags: `-Werror` makes it an error and fails the
   compile, as plain clang does; `-w` hides it.
3. **Match only on the flags that decide what prints.** A copy fits only a
   compile in which the same warnings are on at that `#include`: the
   command line's `-W` flags and any `#pragma GCC diagnostic` in force
   there. `-Werror`, `-Wno-error=...` and formatting flags are not compared,
   because step 2 applies them. Each of the six projects compiles with one
   set of warning flags, except postgres (6 of 1,341 compiles use another)
   and cpython (1 of 401); 7 `.c` files set a warning pragma before a later
   `#include` (curl 4, cpython 3).
4. A header that leaves a warning pragma in force for its includer is not
   written, as a header that leaves `#pragma pack` pushed is not.

Cost: a copy with no warnings stores nothing; a hit compares one number.

### Q9: two copies of a header share a name (fixed 2026-10-07)

**Mechanism.** A copy is named by a hash of its header, the compiler options
and its metadata, taken before the outside declarations it refers to are
known, so two copies that differ only in those share a name. Inside one
compile the hook reuses a loaded copy by name, so an unguarded header
included twice took its first copy again.

**Reproduced** (`qtests/q9`). Unguarded `h.h`: `extern __typeof__(f) *fp;`.
`a.c` has `int f();` before it, `b.c` has `int f(int);`: two copies, one
name. `c.c` includes `h.h` after `int f();` and again after `int f(int);`,
then calls `fp(1.0f)`. Plain clang: the second include declares `fp` again
as `int (*)(int)`, and the call passes the int 1. Served: the second hit
took the first copy again, `fp` kept no prototype, and the call passed the
double 1.0, so the callee reads garbage. No diagnostic.

**Fix.** A name already loaded in the compile is reused only for the same
copy (the same bytes); another copy under that name is passed over, and the
header is read as text. Loading the second copy as well is not an option:
Clang merges two copies' declarations only when they are the same, so there
would be two `fp`'s, and a use of `fp` is "ambiguous" (tried: naming copies
after their outside declarations led there). Plain clang reads the second
include as text and makes its `fp` a redeclaration of the first, and so
does the cache now. A header included again under other macros, as expat
includes `xmltok_impl.c` three times, has copies of other names and is
still served.

**Checked.** `c.c`'s object is identical to plain clang's. Unit tests:
`ExportTest.ASecondCopyOfAHeaderIsNotTakenForTheFirst`,
`ExportTest.AHeaderIncludedUnderTwoMacroStatesIsServedBothTimes`. Six first
builds, 16 compiles at a time: no failures, and the rule passed over no
copy. A first version that allowed one copy of a header per compile read
63 fitting copies as text in the same builds (cpython 59, expat's
`xmltok_impl.c` among them; git 3; postgres 1).

### Q11: `push_macro` and `pop_macro`

**What they do.** `#pragma push_macro("X")` saves X's current definition on
a stack kept for X; `#pragma pop_macro("X")` puts the saved definition back.
The usual use: a header needs a name its includer may have defined as a
macro, so it saves it, replaces it, and restores it at its end.

```c
// s.h
#pragma push_macro("X")   // save the includer's X
#undef X
#define X 2               // the header's own X
static inline int sx(void) { return X; }   // compiled as return 2
#pragma pop_macro("X")    // the includer's X is back
```

**What a served header must do.** What the text does: inside the header X is
2; after it, X is whatever it was before.

**Tested: the four ways headers use them** (`qtests/q11*`).

| Use | Test | Plain clang | Served | Same? |
|---|---|---|---|---|
| A header saves, changes and restores X; the includer defines X. The usual use | `q11s`: `b.c` has `#define X 1`, includes `s.h`, returns `sx() * 10 + X` | 21 | 60 | No |
| The same; the includer has no X | `q11u` | X undefined after | X undefined after | Yes |
| One header saves and changes X, another restores it | `q11p`: `begin.h`, `end.h`; `b.c` has X = 5 | 25 | 60 | No |
| A header restores what its includer saved | `q11`: `p.h` pops; `a.c` saved 1, `b.c` saved 5 | 505 | 140 | No |

**Why.** A copy keeps, for each macro the header touched, the state the
header left it in, as a definition the copy holds. After `pop_macro`, that
state is the includer's own definition: `#define X 1`, written in `a.c`. The
copy does not hold `a.c`, so the token `1` loses its place, and the served
compile reads garbage where its text was: X expands to 40 in all three
wrong rows (60 = 2 × 10 + 40; 140 = 1 × 100 + 40). The last two rows have a
second problem: what `pop_macro` restores comes from a stack its includer
filled, and a hit never compares that stack.

**Fix proposed.**

1. A macro that ends the header as it began (the same definition, or
   undefined both times) is no effect of the header: nothing is replayed for
   it, and the copy holds no definition of it. This fixes the usual use and
   costs nothing at a hit.
2. A copy never holds a definition written outside it.
3. Rows 3 and 4, a push left on the stack at the header's end or a pop of
   what was pushed before the header: either decline the header, as the
   cache already declines one that leaves `#pragma pack` pushed, or record
   pushes and pops as two more kinds of effect, and the definition a header
   pops from outside as a dependency, compared at the `#include` like any
   other macro. Either costs nothing for headers that do not use the
   pragmas.

**Seen.** No header compiled in the six projects uses them. `xxhash.h` does,
in a branch not compiled on x86, and so does readline.

### Q10: the cache runs inside compiles that build a Clang module

**What a Clang module is.** Clang has a feature of its own called modules,
separate from this cache. A library lists its headers in a file called a
module map. With `-fmodules`, the first time a compile includes one of those
headers, Clang compiles all of them once, in a compile of their own -- a
*module build* -- and saves the result as a `.pcm` file. Later includes of
those headers load the `.pcm` instead of reading the text. Our copies use
the same file format, but they are a different thing: a copy holds one
header as one compile parsed it; a Clang module holds a whole library and is
made by a compile of its own.

**Who uses them in C.** Builds that pass `-fmodules` and have module maps:
mostly Apple platforms, whose system headers come with module maps, and
Objective-C projects. Clang's own headers (`stddef.h`, `stdarg.h`, ...) come
with a module map too, so any compile with `-fmodules` builds those as
modules unless `-fno-implicit-module-maps` is given.

**What goes wrong.** The hook runs in every compile Clang runs, module builds
included, and treats a module build like any other compile: it saves copies
of the library's headers and serves them to later module builds. A module
build that is served a copy writes a Clang module built from our copy
instead of from the headers' text, which Clang's module writer was not made
for.

**Reproduced** (`qtests/q10`). A library of one header, built as a Clang
module three times through the cache (`clang -cc1 -emit-module`): the first
build timed the header, the second saved a copy of it, and the third was
served that copy and crashed (segmentation fault; "malformed or corrupted
precompiled file: 'too many submodules'"). An ordinary compile including
the header was not served the module build's copy.

**Not seen in our runs.** The scripts pass `-fno-implicit-module-maps`, and
none of the six projects uses Clang modules.

**Fix proposed.** The hook does nothing in a module build. A module build
runs once per library, so nothing is lost.

### Q8: a name a copy refers to is looked up later than plain clang looks it up

**What `_Noreturn` means.** In C11, `_Noreturn` on a function declaration
promises that the function never returns to its caller, as `exit()` never
does. The compiler relies on the promise: after a call to such a function,
it deletes the code that would run next.

**C lets a function be declared again, and a later declaration can add a
fact.** The test (`qtests/q8`):

```c
// b.c
void fatal(const char *msg);            // line 2: an ordinary function
#include "h.h"                          // line 3
_Noreturn void fatal(const char *msg);  // line 4: the same function, now promised never to return
int use_b(int x) { return check(x) + 1; }

// h.h
static inline int check(int x) {
  if (x < 0)
    fatal("negative");
  return x * 2;
}
```

**Plain clang** reads `h.h` at line 3. At `fatal("negative")` it connects
the call to the `fatal` it knows then, line 2's. The machine code for
`check` calls `fatal` and then carries on.

**A served compile.** `h.h`'s copy cannot hold `fatal`'s declaration, which
belongs to `b.c`, so it holds the name `fatal` and finds it in the compile
that loads it. It does so when it first needs `check`'s body: when `b.c`
generates machine code, after line 4. So it finds line 4's `_Noreturn`
declaration, and the machine code for `check` calls `fatal` and stops there:
the compiler deleted what came after the call.

**Is it wrong?** Not for a correct program: if `fatal` returned, `b.c` would
have broken its own promise, which C leaves undefined anyway. But the object
differs from plain clang's. The same thing moves debug information: in the
same test, `size_t` is declared again after the `#include` (lines 1 and 5),
and a served compile's debug information names line 5 where plain clang's
names line 1. That is how redis's `config.c` differed from plain clang.

**Fix proposed.** To check that a copy fits, the cache already looks up
every name the copy refers to, at the `#include` (here `fatal` at line 3,
finding line 2's declaration). Keep those results and connect the copy's
names to them instead of looking the names up again later. The later lookups
it replaces cost about 0.5 us each, at most 0.03 s per project.

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

### Q36: a copy assumes the names it declares are new (found 2026-10-07)

**Mechanism.** A copy's own declarations were new where it was written. A
compile served it may already have a declaration of the same name, from its
`.c` file or another header. Clang merges the two when they are the same,
the usual case. When they differ in a way C allows, plain clang makes the
header's a redeclaration of the earlier one, and may warn; a served compile
does neither.

**Reproduced.** `x.h`: `int g();` and `extern int (*fp)();`. `y.h`:
`int g(int);` and `extern int (*fp)(int);`. `a.c` includes only `y.h` and
writes its copy; `b.c` includes `x.h`, then `y.h`, and is served `y.h`'s
copy.

| Test | Plain clang on `b.c` | Served |
|---|---|---|
| `qtests/q9b`, the function `g` only | warning: `g` without a prototype conflicts with a later declaration | no warning; code identical |
| `qtests/q9c`, the variable `fp` too | compiles | error: reference to `fp` is ambiguous |

**How often.** No failure in the six projects. It needs two declarations of
one name that differ but are compatible, one of them inside a copy.

**Fix (idea).** Record the names a copy declares and check at a hit that the
compile has no declaration of them yet. Each such lookup searches every
loaded copy now, so this is affordable only after I1 (one name index).

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
  Where replay's time goes, six first builds, untimed:

  | Replay | Seconds |
  |---|---|
  | All of it: 19,002 hits, about 876 effects each | 34.65 |
  | Finding each effect's name | 29.18 |
  | Updating the macro table | 1.06 |
  | The rest: the loop, refreshing names, finding the copy's macro | 4.41 |

  Your proposal (2026-10-07): replay as a small loop that switches on a
  compact list and writes the macro table directly, each definition decoded
  when its name is first used. The loop exists already
  (`replayMacroEffects`); what costs is finding each name, because Clang
  builds a name's entry from every loaded copy the first time anything asks
  for it. Two ways not to ask: (a) at export, mark the effects that loading
  the copy already applies, and replay only the rest; (b) your design, T8's
  option B: the definitions in our metadata, written into the table through
  a lookup of the compile's own table only. Both remove nearly all of the
  34.65 s. (b) also turns `push_macro` and `pop_macro` (Q11) into two more
  cases of the loop, and is the larger change.
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
8. **Q8. A name looked up later than plain clang looks it up** (rank 5).
   Explained again with the test. Fix proposed: keep the declarations the
   hit's check found at the `#include`. Approve?
9. **Q9. Two copies share a name** (rank 2). Fixed 2026-10-07: another copy
   under a name already loaded is passed over and the header read as text.
   Naming copies after their outside declarations was tried first: it
   turned the wrong code into a compile error (two `fp`'s).
10. **Q10. Module builds** (rank 4). Explained again: what a Clang module
    is. Fix proposed: the hook does nothing in a module build. Approve?
11. **Q11. `push_macro` and `pop_macro`** (rank 3). The usual use -- a
    header saves, changes and restores a macro -- is wrong code too (new).
    Fix proposed: a macro that ends the header as it began is no effect,
    and a copy never holds a definition written outside it. For the split
    uses: decline, or record pushes and pops? Approve 1 and 2, and choose
    for 3.
12. **Q12. `__COUNTER__`.** Not a problem in the six projects: none of their
    headers uses it.
34. **Q34. A served header's warnings are lost** (rank 1). Your decision:
    store them. Design in section 2, matching only on which warnings are
    on. Approve?
35. **Q35. A compile with errors still writes copies** (rank 1). Fixed
    2026-10-07: nothing is written after a real error.
36. **Q36. A copy assumes the names it declares are new** (new). A lost
    warning, or for a variable a compile error. Measure how often, or wait
    for I1?

### C. Proposals waiting for approval

13. **Q13. Macro option B** (T8), your replay design (section 3.1, I8).
    Build (a) now as I8 and your design later as T8, or your design now?
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
