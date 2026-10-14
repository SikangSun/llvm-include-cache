# Header cache: correctness, optimizations and open questions

Written 2026-10-07 and updated the same evening, to work from on
2026-10-08; updated again after your answers (Q8 to Q11, Q34, I8), and on 2026-10-08
after building Q8, Q10, Q34 and I8; on 2026-10-11 after building Q11
and setting Q36 aside; on 2026-10-14 after building Q7, I5 and I9. The
correctness problems explained and ranked (section 2), every
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
and so are the fixes for Q35, Q9, Q10, Q8 and Q34 (section 2) and the
first implementation fix, I8 (section 3.1). Section 2 lists what the
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
| 1 | Q34, Q35: a header's diagnostics are lost on a hit | Warnings disappear; under `-Werror` a compile plain clang fails succeeds | Any header with a warning | Fixed 2026-10-07 (Q35) and 2026-10-08 (Q34) |
| 2 | Q9: two copies of a header share a name | Wrong code, no diagnostic | One unguarded header twice in a compile, an outside declaration changed in between | Fixed 2026-10-07 |
| 3 | Q11: `push_macro` and `pop_macro` | Wrong code, no diagnostic, in three of the four ways headers use them | No header compiled in the six projects uses them | Fixed 2026-10-11, your rule |
| 4 | Q10: the cache runs inside compiles that build a Clang module or a precompiled header | Clang crashed; a compile using the precompiled header failed | Only builds that use Clang modules or precompiled headers | Fixed 2026-10-08 |
| 5 | Q8: a name a copy refers to is looked up later than plain clang looks it up | Different but valid code; debug lines move | A declaration made again after the `#include` | Fixed 2026-10-08 |
| 6 | Q7: a macro argument's column | One debug-line column | 3 of 401 cpython objects | Fixed 2026-10-14, no cost |
| 7 | Q12: `__COUNTER__` | Nothing: such headers are not cached | No header in the six projects uses it | -- |
| -- | Q36 (new): a copy assumes the names it declares are new | A lost warning; for a variable, a compile error | None in the six projects | Set aside 2026-10-11: only declarations without prototypes, removed in C23 |

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

**Q34, fixed 2026-10-08.** Your decision: store the error strings, and
print them where the flags require it.

1. **Saved.** The compile that reads a header as text and writes its copy
   saves each diagnostic Clang prints about the header: its text, its kind,
   its place in the header, its notes, ranges and fix-its.
2. **Printed again.** At a hit the saved diagnostics are printed again, as
   the compile that wrote the copy printed them: warnings as warnings,
   warnings `-Werror` made errors as errors, which fail the compile.
3. **Only where the flags require the same.** A copy is served only where
   the warning flags (`-W...`, `-w`, `-pedantic`) and the warning pragmas in
   force (`#pragma GCC diagnostic ...`) are the ones it was written under.
   Under other flags the header would print other warnings, so the copy is
   passed over and the header read as text. Each of the six projects
   compiles with one set of warning flags, except postgres (6 of 1,341
   compiles) and cpython (1 of 401).
4. **Pragmas a header leaves.** git's `git-compat-util.h` turns off
   `-Wsign-compare` for the rest of the compile with an unpaired
   `#pragma GCC diagnostic ignored`. Its copy records that pragma, and a hit
   applies it at the `#include`, as the text would. A header that leaves a
   pragma pushed, or prints a diagnostic about a place outside it, is not
   written.

| Check (2026-10-08) | Plain clang | Served |
|---|---|---|
| `qtests/qw`, `-Wall` | warning: unused variable `unused_local` | the same warning; stderr identical, "In file included from" line included |
| `qtests/qw`, `-Wall -Werror` | error, exit 1 | the same error, exit 1 |
| Six first builds, 16 compiles at a time | -- | no failures; objects identical except cpython's known Q7 columns and `getbuildinfo.c` |

Unit tests: `ExportTest.AServedHeaderPrintsItsWarningAgain`,
`UnderWerrorAServedHeaderWithAWarningFailsTheCompile`,
`ACopyIsNotServedUnderOtherWarningFlags`,
`NotServedWhereAWarningPragmaIsInForce`,
`AHeaderLeavingAWarningPragmaInForceIsServedWithIt`,
`AHeaderLeavingAWarningPragmaPushedIsNotWritten`.

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

Explained again 2026-10-08, by another model, on request.

**Fixed 2026-10-11, by your rule:** what a header's pop takes from below it
is a dependency; the pushes and pops that reach outside it are replayed; a
macro the header leaves as it found it is no effect. The exporter follows
each push and pop (two new PPCallbacks); the copy's metadata lists, per
name, the definitions its pops took and how many pushes it left
(`MacroStackEffect`, metadata version 9); a hit checks the first against
the top of the loading compile's stack and replays the pops and pushes
before making the copy's macros visible; a copy no longer holds a
definition a pop brought back from outside it; and a use of the name after
such a pop is no longer checked against its definition at the `#include`,
since the pop's own dependency covers it. q11s, q11u, q11p and q11 now
compile to plain clang's code, served or read, and so do q11p and q11 with
b.c pushing a.c's value, where end.h and p.h are served and their pops
replayed. The traces below are of the build before the fix.

**Result.** In three of four tests, b.c is served the copy and returns a different number than plain clang: q11s 60 (plain clang: 21), q11p 60 (25), q11 140 (505); q11u agrees. Cause 1: the copy holds a definition of X written in a.c, and b.c reads that definition as 40; in q11s, X is `1` before `#include "s.h"` at b.c:2 and 40 after. Cause 2, in q11p and q11: the push is in one file, the pop is in another, and the copy holds neither, so even a readable definition would make X `1` (a.c's) after the `#include` in b.c, where plain clang has `5` (b.c's own).

#### 1. What the two pragmas do

`#pragma push_macro("X")` puts X's current definition on a stack kept for X. `#pragma pop_macro("X")` takes the top definition off that stack and makes it X's definition. Example, a file ex.c:

| Line | Code | X before → after | What the line does |
|---|---|---|---|
| ex.c:1 | `#define X 1` | undefined → 1 | defines X |
| ex.c:2 | `#pragma push_macro("X")` | 1 → 1 | puts `1` on X's stack; X is still 1 |
| ex.c:3 | `#undef X` | 1 → undefined | undefines X |
| ex.c:4 | `#define X 2` | undefined → 2 | defines X |
| ex.c:5 | `#pragma pop_macro("X")` | 2 → 1 | takes `1` off X's stack and makes it X's definition |
| ex.c:6 | `int v = X;` | 1 → 1 | v is 1 |

The stack is kept while clang reads one .c file and every header it includes. So a push in one file can meet its pop in another (q11p, q11).

#### 2. Terms

- **Plain clang**: clang run on b.c with the cache off. Its answer is the correct one.
- **a.c's compile**: clang run on a.c with the cache on, first. The cache reads each header's text and writes one copy per header.
- **b.c's compile**: clang run on b.c with the cache on, next. At an `#include`, the cache loads the header's copy instead of reading the header's text. Then b.c is **served** the copy.
- **Copy** of one header (s.h, begin.h, end.h or p.h): the declarations with every macro already expanded (sx() is stored as `return 2;`), plus, for each macro that the header's text defines or pops, the definition the macro has when the header's text ends.
- **Macro list**: inside the copy, the macros that the header's text `#define`s or `#undef`s. When b.c's compile loads the copy, the cache **replays** the list: it defines each macro on the list as the copy's definition, or undefines the macro.
- **Metadata**: inside the copy, the assumptions the copy needs, such as "X is `1` at the `#include`". The cache compares the metadata with b.c before serving b.c.
- **Notation**: `s.h:7` is line 7 of s.h. `1 (a.c:1)` is X defined as 1 by the line a.c:1. `40 (copy)` is X defined as 40 by a loaded copy. "X before → after" is X's state before and after the line runs.

#### 3. q11s: s.h pushes X, defines its own X, pops X; b.c defines X

**Plain clang on b.c.**

| Line | Code | X before → after | What the line does |
|---|---|---|---|
| b.c:1 | `#define X 1` | undefined → 1 (b.c:1) | defines X |
| b.c:2 | `#include "s.h"` | 1 → 1 | clang enters s.h |
| s.h:1-2 | `#ifndef S_H` `#define S_H` | 1 → 1 | include guard; X is not involved |
| s.h:3 | `#pragma push_macro("X")` | 1 → 1 | puts `1 (b.c:1)` on X's stack |
| s.h:4 | `#undef X` | 1 → undefined | undefines X |
| s.h:5 | `#define X 2` | undefined → 2 (s.h:5) | defines X |
| s.h:6 | `static inline int sx(void) { return X; }` | 2 → 2 | X expands to 2: sx() is `return 2;` |
| s.h:7 | `#pragma pop_macro("X")` | 2 (s.h:5) → 1 (b.c:1) | takes `1 (b.c:1)` off X's stack and makes it X's definition |
| s.h:8 | `#endif` | 1 → 1 | end of s.h |
| b.c:3 | `int b(void) { return sx() * 10 + X; }` | 1 → 1 | sx() is 2, X is 1: 2 * 10 + 1 = **21** |

**The cache: a.c's compile writes the copy.** The lines of a.c are the lines of b.c (the function is named a). So the table above applies with a.c:1 in place of b.c:1. At s.h:7, X goes from `2 (s.h:5)` to `1 (a.c:1)`. At the end of s.h, a.c's compile writes:

```
copy of s.h
  declarations    : sx(), whose body is `return 2;`
  macro list      : S_H, X   (both defined when s.h ends)
  definition of X : `1`   <- written at a.c:1, outside s.h
```

The copy holds s.h's text only, not a.c's. So the copy cannot store where the token `1` was written, and the token's text cannot be read back.

**The cache: b.c's compile loads the copy.**

| Line | Code | X before → after | What happens |
|---|---|---|---|
| b.c:1 | `#define X 1` | undefined → 1 (b.c:1) | defines X |
| b.c:2 | `#include "s.h"` | 1 (b.c:1) → 40 (copy) | loads the copy of s.h and replays the macro list: X is defined as the copy's definition, whose token reads as 40 |
| b.c:3 | `int b(void) { return sx() * 10 + X; }` | 40 (copy) → 40 (copy) | sx() is 2 (stored in the copy), X is 40: 2 * 10 + 40 = **60** |

**Result.** 60, against 21 for plain clang. Cause 1.

#### 4. q11u: the same s.h; b.c has no X

**Plain clang on b.c.**

| Line | Code | X before → after | What the line does |
|---|---|---|---|
| b.c:1 | `#include "s.h"` | undefined → undefined | clang enters s.h |
| s.h:1-2 | `#ifndef S_H` `#define S_H` | undefined → undefined | include guard |
| s.h:3 | `#pragma push_macro("X")` | undefined → undefined | puts "undefined" on X's stack |
| s.h:4 | `#undef X` | undefined → undefined | nothing to undefine |
| s.h:5 | `#define X 2` | undefined → 2 (s.h:5) | defines X |
| s.h:6 | `static inline int sx(void) { return X; }` | 2 → 2 | sx() is `return 2;` |
| s.h:7 | `#pragma pop_macro("X")` | 2 (s.h:5) → undefined | takes "undefined" off X's stack and makes it X's state |
| s.h:8 | `#endif` | undefined → undefined | end of s.h |
| b.c:2-4 | `#ifdef X` `int x_after = 1;` `#endif` | undefined → undefined | `#ifdef X` is false: b.c:3 is skipped |
| b.c:5 | `int b(void) { return sx(); }` | undefined → undefined | returns 2 |

**The cache: a.c's compile writes the copy.** The lines of a.c are the lines of b.c (the function is named a). At s.h:7, X goes from `2 (s.h:5)` to undefined. At the end of s.h, a.c's compile writes:

```
copy of s.h
  declarations    : sx(), whose body is `return 2;`
  macro list      : S_H defined, X undefined   (when s.h ends)
  definition of X : none   (X is undefined: no token to read back)
```

**The cache: b.c's compile loads the copy.** At b.c:1, the copy says X is undefined at the end of s.h. X is undefined before b.c:1 and after b.c:1. At b.c:2, `#ifdef X` is false, as in plain clang. At b.c:5, sx() is 2.

**Result.** 2 and X undefined after the `#include`, in both. Same.

#### 5. q11p: begin.h pushes, end.h pops

**Plain clang on b.c.**

| Line | Code | X before → after | What the line does |
|---|---|---|---|
| b.c:1 | `#define X 5` | undefined → 5 (b.c:1) | defines X |
| b.c:2 | `#include "begin.h"` | 5 → 5 | clang enters begin.h |
| begin.h:1 | `#pragma push_macro("X")` | 5 → 5 | puts `5 (b.c:1)` on X's stack |
| begin.h:2 | `#undef X` | 5 → undefined | undefines X |
| begin.h:3 | `#define X 2` | undefined → 2 (begin.h:3) | defines X |
| begin.h:4 | `static inline int inside_begin(void) { return 7; }` | 2 → 2 | X is not involved |
| b.c:3 | `static int inside = X;` | 2 → 2 | inside is 2 |
| b.c:4 | `#include "end.h"` | 2 → 2 | clang enters end.h |
| end.h:1 | `#pragma pop_macro("X")` | 2 (begin.h:3) → 5 (b.c:1) | takes `5 (b.c:1)` off X's stack and makes it X's definition. The push was in begin.h, another file |
| end.h:2 | `static inline int inside_end(void) { return 8; }` | 5 → 5 | X is not involved |
| b.c:5 | `int b(void) { return inside * 10 + X; }` | 5 → 5 | inside is 2, X is 5: 2 * 10 + 5 = **25** |

**The cache: a.c's compile writes two copies.** a.c differs from b.c at a.c:1, `#define X 1`, and in the function's name. The table leaves out a.c:2-5, begin.h:4 and end.h:2: none of them defines, undefines, pushes or pops X.

| Line | Code | X before → after | What the line does |
|---|---|---|---|
| a.c:1 | `#define X 1` | undefined → 1 (a.c:1) | defines X |
| begin.h:1 | `#pragma push_macro("X")` | 1 → 1 | puts `1 (a.c:1)` on X's stack |
| begin.h:2 | `#undef X` | 1 → undefined | undefines X |
| begin.h:3 | `#define X 2` | undefined → 2 (begin.h:3) | defines X |
| end.h:1 | `#pragma pop_macro("X")` | 2 (begin.h:3) → 1 (a.c:1) | takes `1 (a.c:1)` off X's stack and makes it X's definition |

a.c's compile writes the copy of begin.h after begin.h:4, and the copy of end.h after end.h:2:

```
copy of begin.h
  declarations    : inside_begin()
  macro list      : X   (defined when begin.h ends)
  definition of X : `2`   <- written at begin.h:3, inside begin.h: readable
  not in the copy : the push at begin.h:1

copy of end.h
  declarations    : inside_end()
  macro list      : empty   (end.h has no #define or #undef)
  definition of X : `1`   <- written at a.c:1, outside end.h
  not in the copy : the pop at end.h:1
```

**The cache: b.c's compile loads both copies.**

| Line | Code | X before → after | What happens |
|---|---|---|---|
| b.c:1 | `#define X 5` | undefined → 5 (b.c:1) | defines X |
| b.c:2 | `#include "begin.h"` | 5 (b.c:1) → 2 (copy) | loads the copy of begin.h and replays its macro list: X is defined as `2`. The push at begin.h:1 does not run: X's stack stays empty |
| b.c:3 | `static int inside = X;` | 2 (copy) → 2 (copy) | inside is 2 |
| b.c:4 | `#include "end.h"` | 2 (copy) → 40 (copy) | loads the copy of end.h. The copy's definition of X is the token from a.c:1, which reads as 40. The pop at end.h:1 does not run |
| b.c:5 | `int b(void) { return inside * 10 + X; }` | 40 (copy) → 40 (copy) | inside is 2, X is 40: 2 * 10 + 40 = **60**. Clang also prints `warning: ambiguous expansion of macro 'X'` here |

X is not on the macro list of end.h's copy, because `pop_macro` is neither a `#define` nor a `#undef`. Yet the load at b.c:4 still takes X from 2 to 40, and clang finds more than one definition of X at b.c:5 and prints the warning.

**Result.** 60, against 25 for plain clang. Cause 1: the token from a.c:1 reads as 40, as in q11s. Cause 2: plain clang's pop at end.h:1 takes `5 (b.c:1)`, which b.c's push at begin.h:1 put on X's stack. In b.c's compile, neither begin.h:1 nor end.h:1 runs, so X's stack is empty at b.c:4. A readable token would still make X `1` (a.c's), not `5` (b.c's).

#### 6. q11: b.c and a.c push; p.h pops

**Plain clang on b.c.**

| Line | Code | X before → after | What the line does |
|---|---|---|---|
| b.c:1 | `#define X 5` | undefined → 5 (b.c:1) | defines X |
| b.c:2 | `#pragma push_macro("X")` | 5 → 5 | puts `5 (b.c:1)` on X's stack |
| b.c:3 | `#undef X` | 5 → undefined | undefines X |
| b.c:4 | `#define X 1` | undefined → 1 (b.c:4) | defines X |
| b.c:5 | `#include "p.h"` | 1 → 1 | clang enters p.h |
| p.h:1-2 | `#ifndef P_H` `#define P_H` | 1 → 1 | include guard |
| p.h:3 | `#pragma pop_macro("X")` | 1 (b.c:4) → 5 (b.c:1) | takes `5 (b.c:1)` off X's stack and makes it X's definition. The push was in b.c, another file |
| p.h:4 | `static inline int px(void) { return X; }` | 5 → 5 | X expands to 5: px() is `return 5;` |
| p.h:5 | `#endif` | 5 → 5 | end of p.h |
| b.c:6 | `int b(void) { return px() * 100 + X; }` | 5 → 5 | px() is 5, X is 5: 5 * 100 + 5 = **505** |

**The cache: a.c's compile writes the copy.** a.c differs from b.c at a.c:1 (`#define X 1`) and at a.c:6 (`int a(void) { return px(); }`). The table leaves out p.h:1-2, p.h:5 and a.c:6: none of them defines, undefines, pushes or pops X.

| Line | Code | X before → after | What the line does |
|---|---|---|---|
| a.c:1 | `#define X 1` | undefined → 1 (a.c:1) | defines X |
| a.c:2 | `#pragma push_macro("X")` | 1 → 1 | puts `1 (a.c:1)` on X's stack |
| a.c:3 | `#undef X` | 1 → undefined | undefines X |
| a.c:4 | `#define X 1` | undefined → 1 (a.c:4) | defines X: a different definition from a.c:1, with the same token `1` |
| a.c:5 | `#include "p.h"` | 1 → 1 | X is `1` here, so the copy's metadata will say "X is `1` at the `#include`" |
| p.h:3 | `#pragma pop_macro("X")` | 1 (a.c:4) → 1 (a.c:1) | takes `1 (a.c:1)` off X's stack and makes it X's definition |
| p.h:4 | `static inline int px(void) { return X; }` | 1 → 1 | px() is `return 1;` |

a.c's compile writes the copy of p.h after p.h:5:

```
copy of p.h
  declarations    : px(), whose body is `return 1;`
  macro list      : P_H   (defined when p.h ends; X is not on the list)
  definition of X : `1`   <- written at a.c:1, outside p.h
  metadata        : X is `1` at the #include
  not in the copy : the pop at p.h:3
```

**The cache: b.c's compile loads the copy.**

| Line | Code | X before → after | What happens |
|---|---|---|---|
| b.c:1-4 | as in the plain table | undefined → 1 (b.c:4) | X's stack holds `5 (b.c:1)` |
| b.c:5 | `#include "p.h"` | 1 (b.c:4) → 40 (copy) | The cache compares the metadata with b.c: X is `1` here, as the copy needs. The check passes, so b.c's compile loads the copy. The copy's definition of X is the token from a.c:1, which reads as 40. The pop at p.h:3 does not run |
| b.c:6 | `int b(void) { return px() * 100 + X; }` | 40 (copy) → 40 (copy) | px() is 1 (stored in the copy), X is 40: 1 * 100 + 40 = **140**. Clang also prints the warning here |

**Result.** 140, against 505 for plain clang. Cause 1: the token from a.c:1 reads as 40, as in q11s. Cause 2: the check compares only X's value at the `#include` (`1` at a.c:5, `1` at b.c:5). The check never compares the stacks: the top of X's stack is `1 (a.c:1)` in a.c and `5 (b.c:1)` in b.c. Plain clang's pop at p.h:3 takes `5`, so px() is 5. The copy holds px() as `return 1;`.

#### 7. All four results

| Test | Setup | Plain clang: b.c's return value | Served the copy: b.c's return value | Same? |
|---|---|---|---|---|
| q11s | s.h:3 pushes X, s.h:5 defines X as 2, s.h:7 pops; b.c:1 defines X as 1 | 21 | 60 | No |
| q11u | the same s.h; b.c has no X | 2, X undefined after b.c:1 | 2, X undefined after b.c:1 | Yes |
| q11p | begin.h:1 pushes X, end.h:1 pops; b.c:1 defines X as 5 | 25 | 60, plus a new warning | No |
| q11 | b.c:2 pushes X (a.c:2 does too), p.h:3 pops; b.c:1 defines X as 5 | 505 | 140, plus a new warning | No |

Re-run on 2026-10-08 on the current build: same numbers. A test function `int v(void) { return X; }` placed right after the `#include` returns 40 when served the copy (q11s, q11p, q11) and 1, 5, 5 under plain clang.

**How common.** No header compiled in the six benchmark projects (redis, nginx, curl, git, postgres, cpython) uses `push_macro` or `pop_macro`. Elsewhere the pair is common: a header that needs a name such as `min` or `max` for itself pushes a.c's macro of that name, defines its own, and pops at its end, as s.h does.

#### 8. Fixes (proposed; nothing is built)

| Fix | What the cache would hold or check | Copy of | Result in the tests |
|---|---|---|---|
| **1. Hold nothing for a macro that ends where it began** (cause 1, the usual use) | When a.c's compile reaches the end of s.h, it compares X's definition with X's definition before s.h:1. If both are the same definition, the copy holds nothing for X: no macro-list entry, no definition | s.h | q11s: X is `1 (a.c:1)` before s.h:1 and after s.h:8, so nothing is held. After b.c:2, X stays `1 (b.c:1)`, and b() returns 21, like plain clang. q11u: undefined at both ends, nothing held. Cost at a hit: none |
| **2. Never hold a definition written outside the file** (cause 1, the split uses) | After end.h:2 and after p.h:5, X's definition is `1 (a.c:1)`, written in a.c. The copy would not hold it | end.h, p.h | X no longer reads 40, and the warning goes. b.c still does not get its own `5`: cause 2 remains |
| **3a. Refuse** (cause 2) | The cache writes no copy of a file whose push is not popped in it, or whose pop takes a definition pushed outside it. The cache already declines a header that leaves `#pragma pack` pushed | begin.h, end.h, p.h | b.c reads their text: 25 and 505, like plain clang. Cost: these files are never served |
| **3b. Hold the steps, check the stack** (cause 2) | The copy holds each push and pop as a step to replay. For a pop that takes a definition pushed outside its file, the copy also holds a check at the `#include`: "the definition the pop takes is `1`". Needs macro replay rebuilt as a small loop over steps, where push and pop are two more kinds of step | begin.h (steps: push X, define X as `2`); end.h and p.h (step: pop X, with the check) | In b.c the pop would take `5 (b.c:1)`, not `1`: the check fails, b.c reads the text of end.h and p.h, and returns 25 and 505. When the definitions match, b.c is served the copy and the replayed pop takes b.c's own definition off b.c's stack |

**Recommendation.** Approve 1 and 2. For 3, build (a) now and (b) later, when macro replay is rebuilt as that loop. Reason: (a) follows the existing refusal for `#pragma pack`, and no header in the six benchmark projects uses these pragmas, so refusing loses no hits there.

**Note on fixes 1 and 2.** `pop_macro` calls no #define or #undef callback,
so in `q11p`'s end.h and `q11`'s p.h, X is not on the copy's macro list:
the garbage reaches b.c through the copy's own definition of X becoming
visible when the copy loads, not through the replay. Both fixes must remove
X's definition from the copy itself, not only from the list.

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

**Fixed 2026-10-08.** The hook does nothing in a compile that writes a
precompiled file: a module build, or a precompiled header. A precompiled
header had the same problem: built through the cache, it referred to the
copy it was served, which lives only in the daemon's memory, and every
compile using it stopped at "module file '/__hc_memory/0.pcm' not found"
(`qtests/q10pch`). Checked: three module builds and three precompiled
header builds through the cache succeed, and a compile using the header
compiles.

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

**Fixed 2026-10-08.** To check that a copy fits, the cache already looks up
every name the copy refers to, at the `#include` (here `fatal` at line 3,
finding line 2's declaration). The reader now resolves the copy's references
to those declarations (`ASTReader::pinNameReference`) instead of looking the
names up again later. Checked: `qtests/q8`'s object is identical to plain
clang's; unit test `ExportTest.ACallInACopyFindsTheDeclarationBeforeTheInclude`.

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

**Fixed 2026-10-14, more simply.** The writer turned a location inside a
dropped expansion into the macro's use (`getExpansionLoc`). It now turns it
into the place debug information and diagnostics report for it
(`getFileLoc`): the macro's use, or, inside a macro argument, where the
argument was written. `qtests/q7` (`SET(o, GET(o) - 1)`, with SET and GET
defined in a header outside the copy) now compiles to plain clang's code
with `-g`; so do all six projects' objects, but cpython's
`getbuildinfo.c`, which embeds its build time. A note pointing into such an
argument moves to plain clang's column too (`qtests/q7d`: h.h:4:9, was
h.h:4:4). Writing costs the same: 260.6 against 259.5 ms for the 26 copies
`Objects/object.c` writes, 232.5 against 232.2 ms for `typeobject.c`'s 22,
medians of 7 interleaved.

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

**Set aside 2026-10-11, your decision.** Both failing tests need a function
type without a prototype (`int g();`, `int (*fp)();`): deprecated in every C
standard, and removed in C23, where `int g();` means `int g(void)` and plain
clang rejects q9c itself (`conflicting types for 'g'`). The compatible pairs
C still allows are served correctly: `extern int arr[];` in x.h and
`extern int arr[10];` in y.h (`qtests/q36arr`) compile to plain clang's code,
since Clang's loader links an array of unknown size to a sized one.

**Fix (idea).** Record the names a copy declares at file scope, with their
types, and check at a hit, before loading, that the compile has no
declaration of them, or one of the identical type.

**Cost, corrected 2026-10-09.** The estimate below assumed each check is a
new search through every loaded copy, at the rate measured in replay. The
fit check already does that search: every name the header's text uses,
the names it declares included, is looked up to confirm it is not a macro
(CacheClient.cpp, the not-a-macro names). After that lookup the name's
entry holds every declaration of the name, from the compile and from the
loaded copies, so the fix adds one test per name on that entry. Names a
macro builds by token pasting are not in that list and would still need a
search. The fix is next, with its cost measured. The estimate, kept for
the record (git's first build, 2026-10-08, every copy dumped):

| git first build | Value |
|---|---|
| Hits | 3,421 |
| Declarations a served copy holds, per hit | 3,687 |
| Of them at file scope (functions, types, variables, enumerators), the names to check | about 1,300 |
| Cost of one lookup, measured in replay (29.18 s over 16.65 million) | 1.75 us |
| Checks per git first build | about 7.8 s, 4.5% of plain clang's 174 s |

At that rate the six first builds would have paid about 43 s; the
searches the rate assumed are already made by the fit check.

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
| 1 | I8: skip replayed macro effects already in force | Finding each replayed macro's name, for effects that change nothing | 30.2 s; built 2026-10-08, removed 35.1 s | 4.3% |
| 2 | I5: write a copy from its own range | Scanning the whole compile's source locations, macros, identifiers, comments and declarations for each copy written | 19.4 s; built 2026-10-14, removed 5.2 s | 2.7% |
| 3 | I9: check files once per build | Header searches and file stamps every compile redoes | 17.0 s; built 2026-10-14 (searches only), removed 6.3 s | 2.4% |
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
- **I8, built 2026-10-08 (option (a), your go-ahead).** At a hit an effect
  is skipped when the compile has never seen its name and no earlier copy
  touched it; the name's first use finds the copy's definition anyway. A name
  the compile has seen is still replayed: loading a copy marks every name
  already seen as out of date, and two readers of a macro do not bring one up
  to date (the check that skips an #include whose guard macro is defined, and
  `#pragma push_macro`). Measured: six first builds, medians of three, the
  modes interleaved file by file, the six projects at once, each on a core
  of its own:

  | Project | Plain clang | Cache, every effect replayed | Cache with I8 | Saved by I8 |
  |---|---|---|---|---|
  | redis | 34.46 s | 32.67 s | 32.13 s | 0.54 s |
  | nginx | 21.31 s | 19.44 s | 18.36 s | 1.08 s |
  | curl | 52.22 s | 42.49 s | 40.55 s | 1.94 s |
  | git | 167.40 s | 140.76 s | 120.24 s | 20.53 s |
  | postgres | 266.16 s | 244.08 s | 235.87 s | 8.22 s |
  | cpython | 183.26 s | 166.54 s | 163.78 s | 2.77 s |
  | All six | 724.80 s | 645.98 s | 610.91 s | 35.07 s |

  Predicted: nearly all of replay's 34.65 s. 75% to 93% of effects are
  skipped. `CLANG_HEADER_CACHE_REPLAY_ALL=1` turns I8 off, for measuring.
- **I5.** Of what the writer visits for each copy, 98% to 99.6% of the
  source-location entries and 82% to 98% of the macros belong to other
  headers. A copy's own range is known when the header ends.
- **I9** includes I4.
- **I5 and I9, built 2026-10-14, measured.** Six first builds, the six
  projects at once, each on cores of its own, four modes interleaved file by
  file: plain clang; the cache before I5 and I9; with I5; with I5 and I9 (a
  build ID per build). Three builds each; savings are means of the per-build
  differences, since the modes ran side by side:

  | | Plain clang | Before | I5 | I5 and I9 |
  |---|---|---|---|---|
  | Six first builds, mean of 3 | 728.53 s | 605.59 s | 600.37 s | 594.06 s |
  | Speedup over plain clang | -- | 1.20x | 1.21x | 1.23x |
  | Writing copies (the cache's own timer) | -- | 38.04 s | 33.48 s | 33.93 s |
  | Redoing a copy's header searches | -- | 9.64 s | 9.68 s | 1.48 s |
  | Checking candidates, all steps | -- | 20.81 s | 20.88 s | 12.69 s |

  I5 removed 5.22 s (3.16 to 7.42 s per build), I9 6.31 s (5.14 to 8.62
  s). Every compile succeeded; every object matched plain clang's.

  Both are well below the estimates above. The profile's own timers
  inflated short, frequent steps: curl's header searches took 2.47 s under
  them and 1.19 to 1.44 s without. Treat the other rows of this table as
  upper bounds too, and measure each fix.

  *I5* gives the writer the copy's own run of source-location entries (one
  unbroken run, since Clang numbers entries as it makes them), the compile's
  memory buffers (kept wherever they lie, found once per compile), the macro
  names the header's directives named, and the last top-level declaration
  before the header. Every copy written in both ways, 799 copies over 40
  compiles of each project, was byte-identical.

  *I9* keeps, with each stored copy in the daemon, what its #include
  searches found in the current build, under a key: the include path, the
  name the header was found by and the directory it was found in, and the
  names the compile already holds any of the copy's files by (a search
  starts from those). A compile that finds a known outcome skips the
  searches; one that does not makes them and sends the outcome with its next
  message, so nothing waits. The assumption: during a build no header
  appears in or disappears from the include path. Every compile still checks
  the size and modification time of each file a copy holds. The build ID is
  `CLANG_HEADER_CACHE_BUILD`; without one, outcomes are kept only within a
  compile.

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
| I5 | Write a copy from its own source range | Four scans of the compile's whole state per copy written: 2.4 of its 4.0 ms | 19.4 s | Built 2026-10-14: 5.2 s |
| I6 | Track dependencies only around headers that will be written | Most of the tracking | under 0.6 s | Proposed (§2.21) |
| I7 | Send a copy to the daemon without waiting for the acknowledgement | The wait | 8.4 s | Proposed (§2.21) |
| I8 | Replay: find each effect's name in its own PCM; skip effects already in force | Searching every loaded PCM per effect | 30.2 s | Proposed; T8's option B would replace it |
| I9 | Check files once per build, in the daemon | Every compile checking the same files and redoing the same header searches | 17.0 s | Built 2026-10-14, the searches; each compile still checks the files: 6.3 s |
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
7. **Q7. A column inside a macro argument** (section 2, rank 6). Fixed
   2026-10-14, at no measured cost.
8. **Q8. A name looked up later than plain clang looks it up** (rank 5).
   Fixed 2026-10-08: the copy's names resolve to the declarations the hit's
   check found at the `#include`.
9. **Q9. Two copies share a name** (rank 2). Fixed 2026-10-07: another copy
   under a name already loaded is passed over and the header read as text.
   Naming copies after their outside declarations was tried first: it
   turned the wrong code into a compile error (two `fp`'s).
10. **Q10. Module builds** (rank 4). Fixed 2026-10-08: the hook does
    nothing in a compile that writes a module or a precompiled header.
11. **Q11. `push_macro` and `pop_macro`** (rank 3). Fixed 2026-10-11 by
    your rule: what a pop takes from below the header is a dependency, the
    pushes and pops that reach outside it are replayed, and a macro the
    header leaves as it found it is no effect.
12. **Q12. `__COUNTER__`.** Not a problem in the six projects: none of their
    headers uses it.
34. **Q34. A served header's warnings are lost** (rank 1). Fixed
    2026-10-08: saved with the copy, printed again at a hit, served only
    under the same warning flags and pragmas.
35. **Q35. A compile with errors still writes copies** (rank 1). Fixed
    2026-10-07: nothing is written after a real error.
36. **Q36. A copy assumes the names it declares are new** (new). Set
    aside 2026-10-11: it needs declarations without prototypes, removed in
    C23.

### C. Proposals waiting for approval

13. **Q13. Macro option B** (T8), your replay design (section 3.1, I8).
    (a) built as I8, 2026-10-08: 35.1 s of the six first builds' 646.0 s.
    Your design stays T8, for later.
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
