# Header cache — decision record

Why the code in `clang/lib/ASTCache`, `clang/lib/CacheObserver`, and the
observation hooks in Lex/Parse/Sema are shaped the way they are. Each entry
records what was decided, the reason, and what was rejected — the reasons are
the part that doesn't survive in the code.

Last updated 2026-09-30.

---

## 1. Observation

### 1.1 `PPCallbacks`, not `Preprocessor::setTokenWatcher`

`setTokenWatcher` is a single slot, and `syntax::TokenCollector` already holds
it. `addPPCallbacks` chains, so any number of observers receive the full event
stream without contending. Both cache tiers observe through it.

### 1.2 `TransitiveClosureTracker` lives in Lex, not Basic

It was written into `clang/Basic` first. `PPCallbacks::FileChangeReason` is a
nested type of a Lex-layer class, so Basic cannot name it. Moved before the
first build.

### 1.3 A new callback for undefined identifiers, not a changed signature

`#if SOME_UNDEFINED > 5` reaches no existing callback: the expression evaluator
substitutes `0` without going through macro expansion, so nothing names the
identifier. The preprocessor does track it internally
(`DefinedTracker::IncludedUndefinedIds`), but does not surface it.

Two ways to expose it: extend `PPCallbacks::If`/`Elif` with the identifier
list, or add a callback. Extending the signatures would touch 17 in-tree
overrides for a feature none of them want. Added
`PPCallbacks::UndefinedIdentifierInDirective` instead, fired from
`EvaluateValue`'s undefined-identifier arm and guarded by `ValueLive` so a
branch that was never evaluated records nothing.

### 1.4 A stack of per-header frames, not one flat set per translation unit

The first version tracked one include-closure for the whole translation unit
and asked, per macro, whether its definition fell inside it. That answers the
wrong question. Headers nest, so one macro expansion has to be asked about
once *per enclosing header*, and can answer differently at each: a macro
defined in `a.h` and used in the `b.h` it includes is a genuine dependency of
`b.h` and not a dependency of `a.h`, whose own text pins it.

The flat version also classified the main `.c` file as internal to everything,
which silently suppressed real dependencies. Replaced with a frame per entered
file, each with its own closure and its own record.

### 1.5 Conditionals are recorded with no provenance filter

Syntactic dependencies are filtered (1.12) — a macro whose `#define` sits
inside the frame's own closure is not recorded. Conditionals are not filtered, even
though the same refinement would apply.

Over-recording is always safe: it can only make validation re-check something
that was in fact invariant. Under-recording is never safe. The filter for
conditionals is also harder to state correctly, because an *undefined* macro
has no `#define` location to test provenance against. Deferred as an
efficiency question, not a correctness one.

**Superseded 2026-09-30 (user: "Dependency should be strictly outward
facing").** Built. A definedness test is recorded for a header only if the
state it saw came from outside that header's closure. The state comes from
the active `#define`, or, for an undefined name, from the `#undef` that
removed it; a name never defined, a `-D` and a builtin count as outside.
The first test in a header decides, as for syntactic dependencies. The same
rule covers include-guard and `#pragma once` skips of a file the header
itself read earlier. On redis's `anet.c` (one `.c` file, its own daemon),
rejections of the form "recorded defined, live undefined" fell from 42 to 2;
the two left are `stddef.h` testing `__need_size_t`, which its includer sets
-- outward facing, as they should be.

That exposed a comparison bug: a saved copy stores a macro's body tokens
without their text, so `64` loaded with `__WORDSIZE` from the served copy of
`fmacros.h` read as "a number" and never equalled the `64` another copy had
recorded from text. Literal text is now read from where the token was
written (`serializeMacroTokens`, both sides), and text that cannot be read
neither records nor matches. With both: `anet.c` 18 hits of 18 lookups, no
candidate rejected, assembly identical to an uncached compile. Every header
`anet.c` includes directly was served, so nothing inside them was asked for.

### 1.6 Poison, don't guess

A frame that encounters something the recorder cannot describe soundly is
marked and emits no record at all. This covers C++ template and deduction-guide
queries and any symbol query with no usable source location.

A partial record is worse than none: it has *fewer* constraints than the
compile it came from, so it matches more compiles than it should. The
asymmetry is the whole safety argument — an untracked situation has to fall
out of the cache, never through it.

### 1.7 Both `Elifdef`/`Elifndef` overloads are overridden

Each has two: one carrying the macro name (branch evaluated) and one carrying
only a range (an earlier branch already won). Overriding one hides the other
by ordinary C++ name hiding, silently losing the callback with no diagnostic.
The range-only form deliberately records nothing — that branch was never
tested.

### 1.8 Sema entry points are wrapped, not edited at each return

`getTypeName` has 24 return points across 321 lines; `ClassifyName` has 22
across ~390. Each function keeps its public signature and gains a thin wrapper
around a renamed `…Impl` holding the original body, reporting once at the
wrapper's single return. `isTemplateName` and `isDeductionGuideName` are
wrapped the same way for uniformity.

All observer work happens inside `if (NameClassObserver)`, so an ordinary
compile pays one null check per call.

### 1.9 The four Sema entry points are the complete set

`clang/docs/InternalsManual.rst:1252` states the parser's discipline: whenever
it expects a `tok::identifier` or `tok::coloncolon` it calls
`TryAnnotateTypeOrScopeToken` or `TryAnnotateCXXScopeToken`. All
external-state-dependent ambiguity in C/C++ is rooted in what an identifier
currently names, so routing every identifier through those entry points is what
makes the list complete rather than a sample. A full call-site trace of
`clang/lib/Parse` confirms four: `getTypeName`, `ClassifyName`,
`isTemplateName`, `isDeductionGuideName`.

Two are deliberately not hooked. `isCurrentClassName` reduces to "is this the
identifier of the class currently open", which the header's own text fixes.
`isTagName` and `DiagnoseUnknownTypeName` are error-recovery paths, reached
only once ordinary classification has already failed; a header whose population
reaches either is excluded from caching rather than having its recovery
decisions modelled.

### 1.10 Symbol records store the resolved type's identity

Recording "this word named a type" is not enough. Two translation units can
both answer yes with different types, and the stored parse holds whichever one
built it. The record stores the canonical spelling of what it resolved to.

### 1.11 C only, for now

C's ambiguity surface is exactly two of the four hooks — `ClassifyName` (via
`TryAnnotateName`, which is not `CPlusPlus`-guarded) and `getTypeName`. The
parser never asks the other two in C: `isTemplateName` sits after the C
early-exit in `TryAnnotateTypeOrScopeTokenAfterScopeSpec` (`Parser.cpp:2062`)
or in C++-only parsing, and `isDeductionGuideName` needs C++17
(`ParseDecl.cpp:3881`) or C++-only parsing. C++ overload sets are a
substantially harder problem and are deferred; reaching a C++-only query
poisons the frame (1.6) rather than recording something unvalidated.

Not crucial to the C scope (marked 2026-09-25), built and kept: the
`isTemplateName`/`isDeductionGuideName` wrappers, their two observer
methods, and the `Template` value of `SymbolClassification`. Nothing the C
scope depends on waits for them, and no C work should be spent on them.

### 1.12 Syntactic dependencies: every identifier considered for expansion

The dependency kinds are organised by the stage that produces them (2026-09-17).
Preprocessor: conditional, with inclusion as the case Clang skips the I/O for,
and pragma, both state and stack. Lexer: *syntactic*. Parser: symbolic. A
syntactic dependency is an identifier the preprocessor considered for macro
expansion while processing the header, with the macro state it had the first
time -- *not a macro*, or *expands to exactly these tokens*. The old value
dependency (a name an outside `#define` made a macro) and the implicit one (a
name the includer could make a macro) are its two states, not two categories.
Approved with the corrections below on 2026-09-18 and built as
`SyntacticDependency`, replacing `ValueDependency`.

It is not exotic and cannot be declined. Counting each project's macro names
against the 1,922 symbols libc exports: git 150, postgres 99, FFmpeg 52, php-src
50, redis 42, openssl 35, qemu 31, curl 27, sqlite 23, cpython 17, nginx 0.
Object-like definitions account for 286 of those and function-like for 275, so
the pattern is not a fringe of the function-override case. Keywords get
redefined too -- qemu `block/iscsi.c:56` defines `inline` as
`__attribute__((gnu_inline))`, includes one header, and undefines it on the next
line, which means the same header has two valid parses in one build; sqlite's
public `sqlite3.h:315` defines `double` as `sqlite3_int64` under
`SQLITE_OMIT_FLOATING_POINT`.

Collection. The lexers report each identifier where it reaches the expansion
decision, just before `HandleIdentifier` would be consulted:
`Lexer::LexIdentifierContinue` for text read from a file, `TokenLexer::Lex` for
tokens coming out of a macro -- a `##` result included -- and
`Lexer::LexDependencyDirectiveToken` for clang-scan-deps' minimized sources.
A fourth site is not in a lexer: `Preprocessor::HandleMacroExpandedIdentifier`
expands a macro whose whole replacement is one token without a `TokenLexer`,
after `isTrivialSingleTokenExpansion` has checked that the token is not itself
a macro. That check is an expansion decision, so the token is reported there.
Found by a failing test, not by reading: `#define ALLOC grab_impl` is the
commonest macro shape there is, and `grab_impl` was going unrecorded.
They report only while expansion is enabled, so a macro's own name in
`#define`, the operand of `#ifdef` or `defined()`, and the operands of `##` are
not candidates; `JOIN(mal,loc)` records `malloc` and neither `mal` nor `loc`.
The receiver is one `ExpansionCandidateObserver` pointer on `Preprocessor`, not
a `PPCallbacks` method: an ordinary compile always has callbacks attached, and a
new callback would add a chain of virtual calls to every identifier of every
compile, where an unset pointer costs one test.

First sighting decides. In each open frame, the first time a name is seen: not
a macro records NotAMacro; a macro defined outside that frame's closure records
ExpandsTo with its replacement tokens; a macro defined inside records nothing.
A header that uses `BUF` and then defines it depended on `BUF` not being a
macro at that use; one that defines `BUF` first does not depend on the includer
at all. One false rejection remains: a header that `#undef`s an outside macro
and then uses the name records NotAMacro, and is rejected wherever the includer
defines it. That costs hits, not correctness.

Transport and check. The NotAMacro names -- several hundred for a system
header -- are moved by the daemon into the shared region as a NUL-separated
list, which `CacheEntryRef` addresses by offset and length; the rest of the
record travels in the reply. The client interns each name and calls
`Preprocessor::isMacroDefined`, which reads `IdentifierInfo::hasMacroDefinition`
first and consults module visibility only when that bit is set: the bit alone
is also set for a module's macro that is not visible, and every compile here
runs with modules (2.8). An entry whose list cannot be read is rejected, and
the daemon does not store an entry whose list it could not store.

Rejected: walking the live macro table, which holds 4,486 entries by the end of
redis `server.h` against a header's ~500 names, and needs an index built per
include in either direction. Rejected: a sorted array of name hashes, which
would let the client reject without interning -- interning costs lookups in a
table Clang maintains regardless, and loading the copy repeats them anyway.

Rejected after first being written here: hooking
`Preprocessor::LookUpIdentifierInfo`. It misses a name that comes out of an
outside macro's body. With `#define ALLOC grab_impl` outside the header,
`grab_impl` was looked up when that `#define` was read; an includer that later
defines `grab_impl` changes the header unseen, while the check on `ALLOC`'s
tokens still passes. Rejected at the same time: deciding at hit time whether a
colliding macro's definition lies inside the recorded closure. Recording the
state at first sighting settles that during population, and removes the false
rejection of a header that defines a name before using it.

---

## 2. What gets stored and where

### 2.1 A separate sub-compile of the header, not a slice of the real compile

**Reversed 2026-09-30 (user): exporting from the real compile is the
default.** The section below argues for the separate compile; it was never
approved, and the user had said on 2026-09-02 that the project goes without
it. What exporting needs, from the code: `ASTWriter` writes every top-level
declaration the compile did not load from a module file
(`ASTWriter.cpp:5648`), and every declaration those reference. Run on the
real compile of anet.c it would write all of stdio.h, every other header read
so far, and anet.c's own declarations -- the context problem again, without
the hidden submodule. Writing one header needs (1) each declaration's header,
taken from its location, and (2) a way to refer to another header's
declarations, which a module file can do only by importing the module file
that holds them. So each header becomes its own PCM importing the PCMs of
the headers it uses -- layout C, written by one compile. That also answers
this section's include-guard objection: whether bar.h came before foo.h or
inside it, foo.h's PCM imports bar.h's. And it answers the conflicts in
section 3: `fd_set` lives once, in sys/select.h's PCM, and a copy names the
exact PCM it was built against. Design to be written up for approval.

Tested 2026-10-01 with a throwaway test: a compile called
`ASTWriter::WriteAST(&Sema, ..., /*WritingModule=*/nullptr, ...)` from the
`FileChanged` exit of `a.h`, kept compiling to the end without error, and a
second compile loaded the 241 KB result with `-include-pch` and used `a.h`'s
struct and macro -- and the first `.c` file's own `MAIN_FLAG` and `main_t`.
So the writer exports "the state so far" as it is, mid-compile, with no
separate compile; what it writes is all of that state, visible on load, not
one header. The only end-of-compile assumption found is an assertion that no
C++ local instantiations are pending, which C never has.

The token tier populates by observing the real compile, because tokens slice
positionally: a token belongs to exactly one file, and
`TokenBuffer::spelledTokens(FID)` hands back one file's worth.

Declarations do not slice that way, but the obstacle is narrower than
"declarations reference things outside the header" — such references are
ordinary dependencies, captured by the closure. Template instantiation is also
not the obstacle: the artifact carries the pattern and each translation unit
instantiates at its own point of use, exactly as modules and PCH already
behave.

The obstacle is include guards. In a translation unit that includes `bar.h`
before `foo.h`, `foo.h`'s own `#include "bar.h"` is a no-op, so `bar.h`'s
declarations sit outside the slice while `foo.h`'s reference them. Reverse the
order and `bar.h` lands inside. The same header yields a different slice
depending on include order, so the slice is not a function of the header and
its dependencies alone.

A piggyback design could work by recording which slices a slice references and
requiring those loaded first — but that is an import graph, which is to say
implicitly modularizing every header during every ordinary compile.
`Decl::setLocalOwningModule` is set as declarations are created, driven by the
compile knowing which module it is building, so per-header ownership cannot be
reconstructed afterwards; an ordinary compile would have to carry it
throughout. One redundant parse is cheaper.

**Corrected 2026-09-11.** This section used to say the cost was a scope
limit: that a header had to compile on its own, and one relying on its includer
would be declined. That was wrong. Every header in a working build compiles —
otherwise the build stops — so every one is cacheable. Needing something from
the includer (redis's `anet.h` uses `uint32_t` and never includes
`<stdint.h>`) is a dependency, recorded like any other. What was missing was
context: the producer compiled each header with nothing in front of it. Given
the context the real build had, `anet.h` is saved and its record carries 38
symbol dependencies. See 2.9.

The import-graph objection above applies to the piggyback design only. It is
an objection to building an import graph *during an ordinary compile*, which
never knows it is building a module. A separate producer compile does know, so
saved copies could import each other the way ordinary Clang modules do: each
holding only its own header's declarations and naming the saved copies it
needs loaded first. That would shrink saved copies — today `server.h`'s
contains a full copy of `stdio.h`, which is why it is 528 KB — but it is not
needed for correctness, since duplicate declarations from two saved copies are
merged on load as they are for any two modules. Deferred as an optimization.

**Not approved.** On 2026-09-02 the user wrote "now that we are going without
the subcompilation route", and no later message approved going back. The
separate compile was nevertheless built and recorded here as decided. Open
again as of 2026-09-15; see the first entry of section 3.

### 2.2 Module shape, not PCH shape

A `.pch` and a `.pcm` are the same format written by the same `ASTWriter`;
`GeneratePCHAction` and `GenerateModuleAction` both create a `PCHGenerator`.
What differs is when the file may be loaded. A PCH is a translation-unit
prefix, loaded before any source is read. A header sits in the middle of a
file, so only the module path can serve it.

This is what forces the pragma problem (2.4): `WritePackPragmaOptions` returns
early when `WritingModule` is set, so the module path deliberately carries no
pragma state in either direction.

### 2.3 The dependency record travels in the protocol, not inside the stored result

A `ModuleFileExtension` block that stores the record inside the artifact was
written, tested, and then removed. It is the wrong placement: the daemon
returns every candidate entry for a filename and the client picks among them,
so a record inside the artifact would force mapping and opening every candidate
just to read it — the second round trip the protocol exists to avoid.

The record is small (a handful of names and token sequences); the artifact is a
whole parsed header. They travel separately.

`DependencySerialization` is kept — a record still has to go over a socket.

### 2.4 Pragma state: incoming is a dependency, outgoing is an effect

`#pragma pack(1)` before an `#include` changes the layout of every record in
the header, via `AlignPackStack.CurrentValue` in
`Sema::AddAlignmentAttributesForRecord`. No macro is tested and none expanded,
so all three dependency categories record nothing and two compiles that must
not share an entry produce identical keys.

The two facets are different in kind and were initially conflated (both were
treated as preconditions, which is sound but declines the common case):

- **Incoming** is an input, so it belongs in the record.
- **Outgoing** is something the header does to its includer. Included
  textually, a header that sets pack without popping leaves the includer
  packed; loading a parsed result does not replay that.

Clang already draws this exact line for a warning: `SemaPPCallbacks::FileChanged`
snapshots the pack value on entering a file and compares it on leaving
(`warn_pragma_pack_modified_after_include`).

Scope: pragmas whose state is a small value — `pack` (a single `uint32_t` via
`AlignPackInfo::getRawEncoding`), `float_control`, the segment pragmas,
`ms_struct`, `init_seg`. `#pragma clang attribute push` holds a `ParsedAttr*`
from the includer's own parse and `#pragma GCC visibility` holds a stack whose
type is private to `SemaAttr.cpp`; neither is comparable or replayable, so a
header reached under either is declined. So is one that pops more levels than
it pushed, since that reads the includer's stack rather than a current value.

### 2.5 The outgoing facet names a category, not just a pragma fix

Everything a header *does* to its includer is a place where loading a parsed
result can differ from including the text, and none of it is a dependency, so
no amount of dependency tracking would catch it. Known members: pragma state,
macro definitions, include-guard marking. Macros are believed handled — module
loading exports a module's macros and makes them visible at the load point —
but this has not been verified and should be before it is relied on.

### 2.6 A whole-macro-table digest is right for the token tier and wrong for the AST tier

The token tier ships an MD5 of the active macro state per file and matches it
by value. The AST tier records only the macros that actually mattered, so an
entry survives a translation unit whose unrelated macro state differs. A digest
would almost never match across translation units.

`llvm::hash_combine` is deliberately not used for the digest: it is seeded per
process, so it is not reproducible across separate compiler invocations, which
a persistent cache requires.

### 2.7 `__LINE__` and `__FILE__` are not excluded

A header that expands a translation-unit-varying builtin cannot be cached —
by AST time the macro is gone and only the value remains, so there is nothing
left to check, and `__COUNTER__` alone would produce a distinct entry per
translation unit.

The list is narrower than it first looks. `__LINE__` expands to the presumed
line *within the current file* (`PPMacroExpansion.cpp:1637`), so a use inside a
header yields the header's own line number in every translation unit;
`__FILE__` likewise names the current file. Both are invariant and were wrongly
listed as exclusions at first. The genuinely varying ones are `__COUNTER__`,
`__INCLUDE_LEVEL__`, `__BASE_FILE__`, and the wall-clock trio `__DATE__`,
`__TIME__`, `__TIMESTAMP__`.

**Update 2026-09-11.** The exclusion above was never implemented, and a test
shows the result is worse than a miss. A header containing

    int counter_a = __COUNTER__;
    int counter_b = __COUNTER__;

is saved with *no* dependency recorded for `__COUNTER__`, so a saved copy
holding 0 and 1 would be served to a compile whose counter is already at 7.
Decided instead of declining: record where each such value lands in the parsed
header, and after loading, rewrite those values from the loading compile's own
state. `__COUNTER__` must also advance the loading compile's counter by the
number of uses in the header, or a later `__COUNTER__` collides. Not yet
implemented.

**Update 2026-09-11 (2).** Until the backfill exists, the producer declines a
header that expands any of the six inside its own text: a miss, never a wrong
answer. Expansions in the context do not count; the context's declarations
stay hidden from the loading compile, so its counter values never reach it.
The backfill is still the plan.

**Update 2026-09-29.** Two corrections.

The context's counter did reach the loading compile. A module file records the
counter its producer ended with (`PP_COUNTER_VALUE`, `ASTWriter.cpp:2593`), and
the reader's listener sets the live counter to it on every load
(`PCHValidator::ReadCounter`). A copy saved after a context that used the
counter twice set a compile that had reached 5 back to 2, so later "unique"
names could repeat. Fixed: the server restores the counter after the load,
which is right because the saved part itself used none. Pinned by
`HitTest.AHitLeavesTheCounterWhereTheHeaderFoundIt`.

The exclusion is not rare. Searching header text for the six names found them
only inside `#define`s, which was read as "no header runs one". But a header
runs the counter whenever its own text uses a macro that expands to it: glib's
`G_LIKELY` (`_g_boolean_var_N`), the kernel's `READ_ONCE` and `BUILD_BUG_ON`
(`__compiletime_assert_N`). Measured by preprocessing every `.c` file of six
benchmark projects with their build flags and each of the six replaced by a
marker word: of the headers a `.c` file includes directly, none are declined in
redis, nginx, git or FFmpeg; 5,945 of 28,528 in qemu (21%), among them
`qemu/osdep.h` in 3,340 of 3,855 files; and 11,323 of 32,094 in the Linux
kernel (35%), among them `module.h`, `kernel.h`, `mm.h` and `slab.h`. Only
`__COUNTER__` appeared; the other five ran in no header.

**Decided 2026-09-30 (user): the counter is a plain dependency and an
effect.** A header whose text runs `__COUNTER__` is saved, not declined. Its
metadata records the counter's value where the header starts and how many
values the header used. A saved copy is reused only when the live counter has
that starting value, and after the hit the live counter is set to start plus
count, as reading the header would have left it. Exact by construction. The
other five builtins stay declined; none ran in any measured header.

Relocation -- rewriting the copy's counter numbers on load (section 4) -- is
deferred. A number that fed a computation cannot be rewritten after Clang
computed with it, which the user judged a real gap. Revisit only if the first
round of benchmarks shows the exact-start rule costs too much reuse; measured
beforehand, it keeps 5,203 of qemu's 5,377 reusable includes and 5,549 of the
Linux kernel's 10,145.

### 2.8 Every compile runs with `-fmodules -fno-implicit-module-maps`

Decided 2026-09-11. Applies to both the cached build and the uncached
baseline, so the two are compared under the same flags.

A saved copy is a module, and only a compile with modules on can load one.
`Modules` is `NotCompatible` in `LangOptions.def`, so the AST reader rejects a
mismatch outright. Tested with a saved copy built under the loading compile's
own flags:

    error: modules semantics was enabled in precompiled file 'M.pcm'
           but is currently disabled

The reason for the check: inside a module, every declaration is labelled with
the module that owns it, and stays hidden until that module is imported. With
modules off, an `#include` is purely textual and has no import step, so
nothing could ever reveal those declarations. The reader refuses the file
before reading any of them.

`-fmodules` alone would also make Clang look for `module.modulemap` files next
to headers and turn every header those files cover into a module — Clang ships
one for its own builtin headers — which changes the build being measured.
`-fno-implicit-module-maps` stops that. On a file including `<stddef.h>` and
`<stdio.h>`: zero modules built, every header stays textual.

Cost of the mode itself (`-fsyntax-only`, best of three, debug build of
clang): redis `object.c` 3025 → 3030 ms, `server.c` 4705 → 4759 ms. redis
`sds.c` and `object.c` compile unchanged under it.

The alternative, patching the reader to skip the check, would leave module
ownership labels on declarations in a compile with no machinery to interpret
them. Not attempted.

### 2.9 Context: what the producer compiles in front of a header

Decided 2026-09-11.

The producer compiles a header after the files that the compile which missed on
it had already included, in order, with that compile's own flags. Nothing
about the context is chosen by policy: every population request comes from an
actual miss, so the context is whatever that compile really had. A saved copy
built in some other context is not wrong either — its record describes that
context, and it simply never matches here. Stored variants converge on the
ones the build actually uses.

Macro state needs no special handling. redis's `fmacros.h` defines
`_GNU_SOURCE`, which changes what `<string.h>` declares. `fmacros.h` is in the
list, so `_GNU_SOURCE` is defined when `<string.h>` is compiled, and the record
for `<string.h>` says so.

**The context is compiled into the saved copy but kept hidden.** The saved
copy is a module with two submodules:

    module __hc_<hash> {
      explicit module context { header "<wrapper including each context file>" }
      module target { header "<wrapper including the header>" export * }
    }

A loading compile makes only `target` visible. This is a soundness
requirement. A textual `#include "foo.h"` adds foo.h's declarations to what is
visible and nothing else. If the context's declarations became visible too,
loading foo.h's saved copy into a file that never included `<stdio.h>` would
declare everything in `<stdio.h>` there. foo.h's own references into its
context — a type foo.h uses, a macro foo.h tests, a guard that suppressed one
of foo.h's `#include`s — are covered by foo.h's record, which the loading
compile checks against its own state.

**Only headers included directly by a `.c` file are populated.** A header
reached from inside another header is saved as part of that other header's
copy. The only way to reproduce "partway through `a.h`" would be to include
all of `a.h`, and if `a.h` includes `b.h`, that compiles `b.h` inside the
hidden context submodule and leaves the visible one empty. The producer
detects this and declines. (An earlier experiment showed `b.h`'s declarations
do end up in such a copy. True — but in the hidden part, which is exactly what
makes the copy unusable.)

**Superseded 2026-09-30 (user: "headers in .h file should also be served").**
A header included from inside another header is now built and served too.
Its context is the lines each file along the include chain ran before the
next #include: the .c file's lines before `#include "a.h"`, then a.h's lines
before `#include "b.h"`. A line is an #include (replayed the way the compile
found the file) or a #define or #undef the file's own text made, so glibc's
`bits/stdio_lim.h` sees `_STDIO_H` from stdio.h's text. a.h itself is never
replayed, and what a.h declared above the #include is missing; a b.h that
needs it fails to compile in the producer and is declined. The .c file's own
#defines now come along as well, which fixes the `_GNU_SOURCE` miss in
section 4. The compile that reads a header records this request when
`CLANG_HEADER_CACHE_JOBS` names a directory (job file version 3); the driver
flags are added by the populate tool's `-Xcc`, since cc1 does not see them.
The hit path no longer limits serving to the .c file's own #includes.

First end-to-end result (redis, debug clang, one daemon): `anet.c` alone made
161 lookups with 76 hits, and its assembly matched an uncached compile. With
five more files on the same daemon, `sds.c` matched; `object.c`, `zmalloc.c`,
`ae.c` and `util.c` failed to compile. See section 3, "Copies built after
different .c files conflict".

The context goes in through a generated wrapper header rather than `-include`
flags. `-include` lists are among the preprocessor options the reader
validates on load (`checkPreprocessorOptions`); keeping context out of them
means the saved copy's options are exactly the compile's own.

**Each `#include` is replayed the way the compile found its file.** For
`#include <...>` the compiler searches an ordered list of directories. On this
machine:

    1. .../build_dev/lib/clang/22/include
    2. /usr/local/include
    3. /usr/include/x86_64-linux-gnu
    4. /usr/include

`#include <stdint.h>` takes the first match, Clang's `stdint.h` in directory 1,
and remembers it came from directory 1. Clang's `stdint.h` then says
`#include_next <stdint.h>`: search again starting after directory 1. That finds
glibc's `/usr/include/stdint.h`, which declares `uint32_t`.

The first version replayed `#include "/abs/.../include/stdint.h"`. A file
opened by path was found in no directory, so its `#include_next` searches from
directory 1, finds Clang's `stdint.h` again (guarded, so empty), and glibc's
is never read. The `anet.h` smoke test was declined with "unknown type name
'uint32_t'".

A resolved path says which file; `#include_next` also needs where in the list
it was found, and the compiler only learns that by searching. So each entry of
a request records two things (`IncludeRef`):

- **the text as written** (`<stdint.h>`, `"hiredis.h"`), used to replay the
  include, so the producer searches the same list (the request carries the
  same `-I` flags);
- **the file opened**, used to check the replay: if the replay opens a
  different file, the producer declines.

A file the compile found without searching -- next to the `.c` file (Clang
leaves the found-in directory unset there, `HeaderSearch.cpp:1035`), or by an
absolute path -- is replayed by its absolute path, which is also found without
searching, so `#include_next` inside it starts from the top in both compiles.

The one thing no list of files reproduces is a declaration written directly in
the `.c` file above the `#include`:

    typedef int T;
    #include "h.h"      // h.h uses T

`T` is in no header, so the producer's compile fails, nothing is stored, and
that header misses in that context. About 3% of `.c` files have anything at
all before their first `#include` (redis 4/126, sqlite 3/125, curl 0/44). A
miss, never a wrong answer. Since nothing is stored, every later compile
re-queues the same failing request; having the daemon remember the failure is
a later fix.

Each saved copy is built under a module name hashed from its header, flags,
and context (`__hc_` + 16 hex digits of xxh3). Two modules with the same name
cannot both be loaded into one compile, so the single fixed name used before
would have made loading two saved copies together impossible.

**The three generated files are stored inside the saved copy.** The producer
writes them into a temporary directory and deletes that directory when it is
done, but the saved copy records them as the files its declarations were
reached through. A compile that later touches a header the saved copy also
contains -- the normal case, since the context is exactly what the `.c` file
had already included -- goes to the saved copy for that header, ends up
needing a wrapper on disk, and dies with "malformed or corrupted precompiled
file: could not find file '.../__hc_context.h'". Each of the three is passed
as `-fmodules-embed-file=`, which marks the file transient
(`FrontendAction.cpp:988`) so the writer stores its text in the saved copy; on
load, a transient input file missing from disk becomes a virtual file built
from that text (`ASTReader.cpp:2849`). Cost on redis `anet.h` with
`<stdint.h>` as context: 116,744 -> 117,036 bytes, +292 bytes or +0.25%, which
is the 418 bytes of generated text minus what the bitstream packs away.
Embedding every input file instead (`-fmodules-embed-all-files`) would store
the text of `<stdint.h>` and everything under it: 170,640 bytes, +46%, for no
gain.

### 2.10 Population runs on a thread during the compile; what is left at exit goes to a detached process

A compile that misses queues a `ProduceRequest` for a producer thread in the
same process. The thread starts at the first miss, produces saved copies while
the compile continues, and pushes each to the daemon as it finishes. When the
compile is done (`cc1_main.cpp`, after `ExecuteCompilerInvocation`), the
requests the thread has not finished -- including the one it is partway
through, which restarts from scratch -- are written to a job file and handed
to a detached `clang-header-cache-populate -job <file>`. The compile never
waits for population.

Why a thread: the first saved copy exists sooner. Measured on redis (best of
5): producing `server.h` takes 2.6 s; `object.c` compiles in about 3.0 s. With
population only at exit, `server.h`'s copy exists about 5.6 s after `object.c`
starts; with the thread, about 2.7 s. Every compile that reaches
`#include "server.h"` in between hits instead of missing, and 61 of redis's
126 `.c` files begin with that line.

Why the handoff is still needed: one production can take nearly as long as
the whole compile. Producing `functions.h` (context: `server.h`) takes 2.6 s,
although `functions.h` itself parses in about 10 ms, because the producer
re-parses the context. `object.c` has six direct includes, so on an empty
cache its queue holds roughly 15 s of work against a 3 s compile (estimated
from two measured productions). The thread gets further when the compile
spends longer after its last `#include` -- code generation and optimization
run after parsing.

Accepted costs:

- A crash in the producer thread kills the user's compile; a crash in a
  detached process would not. `llvm::CrashRecoveryContext` is a partial guard.
- The thread must be stopped before the process exits: cc1 exits without
  freeing memory, and a `std::thread` still running at exit aborts the process.
- CPU: under `make -jN` every core is busy, so a production takes CPU from
  other compiles whether it runs on the thread or in the detached process.

Considered: population only in a detached process at exit (the earlier plan;
the first copy arrives later); a detached process started at the first miss
and fed through a pipe (early first copy and crash isolation, no handoff).
The pipe version is the fallback if producer crashes in the thread turn out to
matter in the evaluation.

### 2.11 Duplicate productions are not deduplicated, for now

Two compiles that miss on the same header with the same flags and context each
produce an identical saved copy. The module name -- a hash of header, flags,
and context -- covers everything the producer's output depends on except file
contents, which do not change during a build. In a cold `make -jN` of redis,
each of the 61 files beginning with `#include "server.h"` that starts before
the first copy is saved spends 2.6 s producing the same copy.

Considered and rejected: the daemon marks a header "in progress" when the
first request arrives, and other compiles wait for the result instead of
parsing the header themselves.

1. Waiting costs more than parsing. Producing is parse the context + parse the
   header + write the file; the waiting compile's own work is only parsing the
   header. A compile waiting on the copy of `functions.h` could wait 2.6 s to
   save 10 ms.
2. The copy may not fit the waiting compile: different flags, a different
   context, a `#define` or declaration in its `.c` file above the `#include`,
   or the production declines.

Deferred: the useful half. The daemon records "production in flight for
key K"; a compile that misses with the same key parses the header normally
and does not queue a second production. No compile waits, and because the key
determines the output, dropping the duplicate needs no guess about fit. It
needs a new protocol message (a claim on K, answered "go ahead" or "in
flight"), and a correct first population comes first.

Corrected 2026-09-29: the daemon used to treat two pushes with byte-equal
records as one production and keep only the first. The compiler options are
not in the record, so a copy built with `-fshort-enums` shut out every later
copy of the same header built without it -- the reader refuses the first for
such compiles, and the second was never stored. A push now carries the module
name, which hashes the options, and a duplicate must match it too.

### 2.12 Assumption: no source file changes during a complete build

The evaluation builds each benchmark from a clean tree and runs its tests. No
source file or header changes while that build runs, and none changes between
the build that saved a copy and a build that loads it. After editing any
source, restart the daemon so the cache starts empty.

Clang checks this anyway. Every saved copy lists the files it was built from
with their sizes and modification times, and loading a copy whose header was
edited since stops the compile with "fatal error: file '...' has been modified
since the module file '...' was built". Under the assumption that never
happens. Handling edits -- treating an out-of-date copy as a miss (the
reader's `ARR_OutOfDate` mode) and replacing the stale copy in the daemon -- is
out of scope.

### 2.13 The hit path: what is settled

- **Turned on by an environment variable**, `CLANG_HEADER_CACHE=<daemon
  socket>`. Works with any build system without editing its flags. A compile
  with the variable unset behaves exactly as before.
- **Saved copies load from memory.** The bytes stay in the daemon's shared
  memory. `ModuleManager::addInMemoryBuffer` registers them under a name no
  file has (`/__hc_memory/<module>.pcm`), and `CompilerInstance::loadModuleFile`
  loads them: `ModuleManager::addModule` takes registered bytes before trying
  the disk (`ModuleManager.cpp:182`). `LoadTest` does this partway through a
  compile, after the compile parsed the context header itself, and the loaded
  declarations then work with the compile's own types.
- **What Clang already provides**, checked by hand with copies in the
  producer's layout: a loaded `draw(struct point *)` accepts the compile's own
  `struct point`; two loaded copies whose contexts share a header agree on its
  types; the context stays hidden (`u32` without `types.h` gives "missing
  '#include'"); a second `#include` of the header is skipped, for both an
  include guard and `#pragma once`; the `.c` file can define functions the copy
  declares.
- **How the hit path is wired.** `HeaderCacheHook` (`clang/Lex`) is a
  one-method interface, owned by the `Preprocessor`, asked at the top of
  `case Enter:` in `HandleHeaderIncludeOrImport` -- before the include-depth
  check and before a FileID is created. A non-null answer takes the same two
  lines as `case Import:`: make the module visible, return `ModuleImport`, so
  the parser and Sema learn of it through the ordinary annotation token. The
  implementation (`HeaderCacheServer.cpp`) asks only for `#include`s written
  in the main file, only at a declaration boundary, and only once Sema
  exists. It is installed from `cc1_main.cpp` through
  `CompilerInstance::setPreprocessorCreatedCallback`, because `clangASTCache`
  depends on `clangFrontend` and the dependency cannot also run the other
  way.
- **Lookups use the producer's spelling of the path.** The producer stores
  entries under an absolute, dot-free path; the hit path turns the compile's
  name for the file into the same form (`FileManager::makeAbsolutePath`, then
  `remove_dots`) before asking. Symlinks are not resolved on either side, so
  two spellings through different symlinks still miss.
- **`CLANG_HEADER_CACHE_STATS` prints one line when the main file ends:**
  hits, lookups, candidates rejected, and `#include`s skipped because they were
  not at a declaration boundary. The region path is not configurable from the
  environment; the compile always maps the default region, so the daemon must
  use it.
- **First hit on a real benchmark file.** redis `anet.c`, compiled from its own
  directory with `-std=gnu11 -O2`, after producing `anet.h` in the context of
  `anet.c`'s 16 includes before it: `1 hits / 18 lookups` (19 direct includes;
  `<string.h>` had already been read through another header, so Clang skipped
  it before the hook is asked). The object file's disassembly is identical to
  an uncached compile's, all 1,517 lines.

---

### 2.14 The compiler asks only about headers the shared region lists

Decided 2026-09-18. The daemon keeps a list of headers worth asking about in the
shared region. At an `#include`, the compiler checks the list in memory; a header
not on it is parsed as usual, with no socket traffic at all.

Asking costs more than most headers do. One Unix-socket round trip on this
machine is 12-15 us before any decoding, and one PCM load about 226 us, while
the median header parses its own text in 44 us (257,262 parses across redis,
nginx, git, FFmpeg and qemu). 77.0% of parses cost less than one load, yet the
other 23.0% hold 98.5% of header parse time. For the light majority both answers
lose: a hit loads slower than parsing, and a miss pays the round trip and parses
anyway.

The list names headers, not headers to skip. A header the daemon has not listed
has no saved copy worth serving, so asking about it could only return a miss; a
fresh build therefore parses everything locally and pays nothing extra. The list
is small: 51 headers for nginx up to 778 for qemu, 35 KB of paths at most.

Membership comes from observation. A compile that parses a header times it with
two clock reads between its `FileChanged` events -- how `SemaPPCallbacks`
(`Sema.cpp:180`) times files for `-ftime-trace` -- and reports the times at the
end of the compile, off the critical path. One observation predicts well: 93.8%
of a header's later parses land on the same side of the load line as its first
(91.4% weighted by time), and listing every header whose first parse was heavy
covers 90.0% of later parse time.

The list only decides whether to ask. Every candidate an answer returns still
goes through the full record check, so a stale list costs time and never
correctness.

Publication keeps the region append-only: the daemon appends each new version
of the list and then atomically stores its offset in a fixed slot, and a reader
loads that slot with acquire ordering. Older versions stay readable to anyone
still holding their offset. The daemon republishes only when membership changes.

Rejected: predicting weight from file size, which `stat` gives for free at a
miss. Its rank correlation with parse time is 0.51, and every threshold either
lists mostly light headers (1 KB: 71.9% light) or misses most of the heavy time
(16 KB: 54.4% captured). Rejected: a list of headers to parse locally, which
still pays the round trip for every header not yet on it. Rejected: a copy of
the list synced over the socket, which needs a sync step and can lag; the region
is already mapped by every client.

Not decided: putting each listed header's candidates (record and stored-result
offsets) in the region too, which would remove the socket from lookups entirely.

Built 2026-09-29, with two choices the user made. A header joins the list only
when a compile reports reading it at or above the cutoff; a copy stored by
`clang-header-cache-populate` is not asked about until then, and the daemon's
`-list-stored` option lists stored headers anyway, for that tool and for tests.
And the time is from entering the header to leaving it, the headers it includes
on the way counted in, since a saved copy holds all of them. The daemon keeps
each header's first reported time, so membership never flips back. On a debug
build of clang every header parses slower than the 226 us cutoff and is
listed; the cutoff only filters on a release build.

### 2.15 Folding: a fixed parse-time cutoff decides which headers get their own PCM

Decided 2026-09-25: whether a header is folded into its parent's PCM or gets
a PCM of its own depends on how long the header takes to process, compared
against one fixed number. Not on its line count, and not on how many parents
it has -- a header with very many parents is the one case worth a second look,
and the trade-off there is to be measured, not assumed.

Folding happens when the parent's PCM is built; two existing PCM files cannot
be merged. The producer already folds everything, since it compiles a header
as text and every header that one includes is parsed into the same PCM. So
the work is the other direction: keeping a header out of its parent's PCM.

The cutoff is the cost of one PCM load, 226 us (release clang-18; 2.14). A
header whose processing costs less folds into its parent; one that costs more
gets its own PCM, which the parent refers to. This is the same line 2.14 draws
for the ask-list, so one number decides both: a header worth asking about is a
header worth its own PCM. Across 257,262 parses in five projects, 77.0% fall
below it and fold, and the 23.0% above it hold 98.5% of parse time.

The time compared is the header's own text plus everything folded into it, so
it is computed bottom-up: a child below the line adds its time to its parent's.
It comes from the timing 2.14 already takes -- two clock reads between
`FileChanged` events -- and the first observation stands in for later ones
(93.8% land on the same side of the line). The cutoff has to be read on the
same kind of build that took the timings; a debug clang parses several
times slower, so its timings would put far more headers above the line and
fold far fewer.

Still proposed, not approved: how the producer keeps a header out (list it as
its own module in the generated module map and pass its PCM with
`-fmodule-file=`); falling back to folding when the child has no PCM yet or its
record does not match at the include; copying into the parent's record the
part of the child's record the parent's own text does not settle, since a
referenced child's text never passes the tracker; and always folding a header
that is included more than once under different macros (Clang's `stddef.h`
with `__need_size_t`), since a PCM holds one version of it.

Deferred as an optimization: a dynamic cutoff (section 4).

### 2.16 A hit replays what the header did to the macro table

Built 2026-09-29, as the macro half of the effects the user listed on
2026-09-17. The producer records each macro the target's text defined or
undefined, once, with its state when the target ended. After a load the
client makes the module visible and then appends one local directive per
name at the `#include`: `#undef`, or `#define` with the loaded module's own
definition (`Preprocessor::getModuleMacro`), not a copy re-lexed from text.

Module macro visibility alone was not enough, in two ways a test pinned down.
A module's `#undef` of a macro its importer defined does not hide the
importer's definition, so `lim.h`'s `#undef LIMIT` never took effect. And two
loaded modules that define one name differently leave it ambiguous, where a
textual include lets the later one win. A local directive overrides every
visible module macro, which gives both answers a textual include gives.

The effects travel like the NotAMacro names: the daemon moves them into the
region and a reply carries their offset, since a header that pulls in a system
header touches thousands of macros and they are needed only on a hit.

What this does not cover: a `#pragma push_macro` the header leaves unpopped,
and `#pragma pop_macro` inside the header, which defines a macro without a
`MacroDefined` callback (the `pop_macro` hole, section 4).

Include-once state needed nothing new. The loaded module file already carries
each header's `#pragma once`, `#import` and included marks, and Clang applies
them when it reads the header info. A guarded header's second `#include` is
skipped because replay defines its guard macro.

### 2.17 Export only the header's own part, from the real compile (proposed 2026-10-02; first version built 2026-10-05)

**Proposed, awaiting approval.** The user: "given the sizes, reduced BMI mode
is a must". This replaces the separate compile of 2.1 and the context of 2.9.

**What a saved copy holds.** At the end of header H in the real compile,
write a PCM with H's own part: the declarations H's text produced, the
macros H defined, and copies of the outside declarations those declarations
point at. Nothing else the compile holds.

**Why.** Measured on redis with release clang 18, as PCH sizes, the compile's
whole state right after an `#include` against what that `#include` added:
`anet.c` after `anet.h`, 737 KB against 7 KB; `util.c` after `util.h`,
1,697 KB against 45 KB; `object.c` after `server.h`, its first include,
1,929 KB against 1,717 KB. Over 45 includes that are not first in their file,
the added part is a median 2.1% of the whole
(`scratch/statesize/statesize.py`).

**How the writer does it.**

1. *H's region.* Source-location offsets are handed out in order, so every
   location created between entering H and leaving H, in H or in a file first
   entered under it, falls in one range. A declaration is H's when its
   expansion location falls in that range.
2. *Seeds.* `ASTWriter` starts from every top-level declaration not loaded
   from a module file (`ASTWriter.cpp:5648`) and then writes everything those
   reference (`GetDeclRef` queues it). Starting from H's declarations instead
   writes H's part plus exactly the outside declarations it points at.
3. *Lookup tables.* Written after the declarations, keeping only names whose
   declarations were written: what C++20 reduced BMI already does
   (`isLookupResultNotInteresting`, `ASTWriter.cpp:4631` and `:4691`).
4. *Not reduced BMI as it stands.* Reduced BMI also drops the bodies of
   non-inline functions and some variable initializers (`CanElideDeclDef`,
   `ASTWriterDecl.cpp:429`), and never seeds `static` declarations. A C
   header's `static` functions need both, so the change is a sibling mode on
   the same machinery: an optional seed predicate on `ASTWriter`, no eliding.
5. *Macros.* For a C++20 header unit the writer already writes each macro's
   final value as a module macro (`ASTWriter.cpp`, `isHeaderUnit()` in
   `WritePreprocessor`). Here: the same, for each macro whose `#define` lies in
   H's region. `#undef`s stay in the metadata's effects, as now.
6. *A module.* Only a module loads partway through another compile, so the
   writer gets a `Module` made for H, as for a header unit. The declarations
   stay unowned; the reader treats an unowned declaration as visible
   (`ASTReaderDecl.cpp:647`).
7. *When.* Not at H's `FileChanged` exit: the parser reads one token ahead,
   so the last declaration of H may not be finished there. At the next
   top-level declaration boundary after H ends.

**What the metadata adds.**

- *Outside declarations become dependencies.* Each written declaration
  outside H's region (`anet.h`'s `uint32_t` typedef) is recorded with its
  name and a fingerprint: Clang's ODR hash for a struct, enum or function,
  the canonical type for a typedef. A hit requires this compile to have a
  visible declaration with that name and fingerprint. A different `fd_set` is
  then a miss, not `object.c`'s merge error. Because the check passes only
  where the outside declarations already exist, their being visible on load
  changes nothing. It also covers names an expression uses, such as a
  `memcpy` call in an inline function, which no symbol hook sees.
- *Include resolution* (section 3, `-I`): each `#include` in H's region with
  its spelling and the file found.

**What goes away:** the separate compile, the context and its hidden
submodule, job files, and with them the conflicts of section 3.

**Costs and risks.**

- Writing is on the compile's own path: the AST cannot be read on another
  thread while parsing goes on. Done only on a miss, only for a header on the
  ask-list. Not measured.
- A header N inside H gets its own copy, and H's copy holds N's declarations
  again; loaded together, the two merge. Folding (2.15) could later make H's
  copy import N's.
- Not verified: writing a module from a compile that is not building one, and
  loading it partway through another compile.

**Plan.** (1) A throwaway experiment for the last risk: export `anet.h`'s own
part from a real `anet.c` compile, load it into a second `anet.c` compile,
compare assembly, report the PCM's size. (2) The writer's seed predicate and
macro filter, with tests. (3) The two metadata additions. (4) Export at a
miss in place of job recording.

**Approved 2026-10-02** (user: "If reduced export doesn't break semantics
then it's fine. You can start testing").

**Experiment, 2026-10-02.** `ASTWriter::setPartFilter` (a starting-set
predicate and a macro predicate; lookup tables and C name chains keep only
what was written; nothing elided) and `CLANG_HEADER_CACHE_EXPORT` /
`CLANG_HEADER_CACHE_LOAD` in `HeaderCacheServer.cpp`. The export is written
at the header's `FileChanged` exit, so only headers that end with a
prototype or a macro were tried. Each redis `.c` file was compiled once to
export the header, again serving the header from the export, and compared
with a plain compile (debug clang, `-O2 -S`):

| File : header | Assembly | Copy | Source locations | Declarations | Macros | Export |
|---|---|---|---|---|---|---|
| `anet.c` : `anet.h` | identical | 107 KB | 40 KB | 7 KB | 0.4 KB | 21 ms |
| `sds.c` : `sds.h` | identical | 147 KB | 39 KB | 27 KB | 6 KB | 30 ms |
| `util.c` : `util.h` | identical | 875 KB | 595 KB | 30 KB | 2 KB | 111 ms |
| `zmalloc.c` : `zmalloc.h` | identical | 172 KB | 51 KB | 29 KB | 4 KB | 34 ms |
| `ae.c` : `ae.h` | identical | 203 KB | 14 KB | 48 KB | 29 KB | 38 ms |
| `adlist.c` : `adlist.h` | identical | 57 KB | 18 KB | 4 KB | 1 KB | 10 ms |
| `dict.c` : `dict.h` | identical | 124 KB | 38 KB | 19 KB | 3 KB | 24 ms |

So a module written from an ordinary compile loads partway through another
compile and gives the same program. The declarations and macros are the
small part the measurement predicted. Most of each copy is tables that cover
the whole compile so far, which the filter does not touch: the source
manager block (every file and macro expansion before the header; `util.c`
includes `math.h` first, whose macros expand thousands of times), the
source-location offsets, the input-file list, and the header-search table.
Split three ways (`scratch/partexport/breakdown.py`): the header's part
(declarations, macros, and the name and index tables over them), the tables
describing the whole compile so far (source manager block, source-location
offsets, input files, header-search table), and a fixed format overhead
(mostly the bitstream's block-info block):

| Header | Part | Whole compile | Fixed |
|---|---|---|---|
| `anet.h` | 20 KB | 72 KB | 15 KB |
| `sds.h` | 63 KB | 69 KB | 15 KB |
| `util.h` | 62 KB | 797 KB | 15 KB |
| `zmalloc.h` | 69 KB | 87 KB | 15 KB |
| `ae.h` | 164 KB | 22 KB | 16 KB |
| `adlist.h` | 12 KB | 31 KB | 14 KB |
| `dict.h` | 46 KB | 64 KB | 14 KB |

`ae.h` is `ae.c`'s first `#include`, so it is the first to include
`monotonic.h`, `<stdint.h>`, `<unistd.h>` and the glibc headers under them,
and all of those are its part; for the same reason little came before it, so
its whole-compile tables are small. The declarations-and-macros column of the
first table leaves out the name table, which for `ae.h` is 56 KB. The writer already drops
unused module maps from the same tables and shifts every location it writes
to match (`computeNonAffectingInputFiles`); trimming them to the header's
region is the next step. Also still to do: the included-file marks, which the
writer saves for every file the compile read, must be limited to the region,
or a `#pragma once` file the loading compile never read would be skipped.

**Trimming, 2026-10-02** (user: "yes, if you can, without breaking
semantics and correctness"). Almost all of the source-location table was
macro-expansion entries: 2,636 of `anet.h`'s 2,811 entries (34 of 40 KB) and
45,369 of `util.h`'s (582 of 595 KB); file entries were 2-3 KB. So a part now
drops every macro-expansion entry outside the region and keeps every file
entry, through the same mechanism that drops unused module maps
(`computeNonAffectingInputFiles`): the dropped entries leave the table and
every offset after them shifts down. A location inside a dropped expansion
is written as the place that macro was used (`getAdjustedLocation`). That is
the file, line and column a presumed location already reports for it, and
so what debug information and a diagnostic's first line show; only the
"expanded from macro" notes of a diagnostic about an outside declaration are
lost. Such an expansion was used outside the region too, so that place is
in a file, and files are kept. Diagnostic-pragma state recorded inside a
dropped expansion is dropped with it.

`-g` exposed one more difference, in file names, not locations: the writer
saves every file name as an absolute path, so a compile that loaded the part
named `ae.h` `/home/.../src/ae.h` in its debug information where a compile
that read the file says `./ae.h`. A part now saves each name as the compile
opened it. A consequence for the design: a part holding `./ae.h` is right
only for a compile that also opens `ae.h` as `./ae.h`, so the hit check must
compare the name each recorded lookup finds, not only the real path.

Rerun, seven headers, `-O2 -S` and `-O2 -g -S`: all 14 compiles gave
assembly identical to a plain compile's.

| Header | Copy | Part | Whole-compile tables | Fixed | Export |
|---|---|---|---|---|---|
| `anet.h` | 62 KB | 20 KB | 27 KB (was 72) | 15 KB | 17 ms |
| `sds.h` | 107 KB | 63 KB | 29 KB (was 69) | 15 KB | 28 ms |
| `util.h` | 119 KB | 62 KB | 42 KB (was 797) | 15 KB | 62 ms |
| `zmalloc.h` | 115 KB | 69 KB | 30 KB (was 87) | 15 KB | 32 ms |
| `ae.h` | 203 KB | 164 KB | 22 KB (was 22) | 16 KB | 39 ms |
| `adlist.h` | 37 KB | 12 KB | 12 KB (was 31) | 14 KB | 9 ms |
| `dict.h` | 80 KB | 45 KB | 20 KB (was 64) | 14 KB | 22 ms |

What remains in the whole-compile tables is one entry per file in the input
list, the header-search table and the file entries, for every file read so
far. Dropping the files the part never refers to needs the set of locations
the part writes before the writer starts, which it computes as it goes; not
attempted.

**First debugging version, built 2026-10-05** (user: "yes let's build ...
I want to finish the first debugging version this week").

- *When.* A header on the ask-list, at a declaration boundary, that no
  stored copy fits, is exported (`PartExporter`, `HeaderCacheServer::tryServe`
  calling `exportNext`). Its region is the offsets handed out between entering
  and leaving it. The part is written at the first top-level declaration the
  parser finishes after the header ends, or at the end of the translation
  unit. That notification comes from an AST consumer registered as a
  frontend plugin (`header-cache-export`), which every compile gets and which
  does nothing in a compile without an exporter.
- *Recording.* The dependency tracker runs in the real compile, but keeps a
  frame only for the headers being exported (`trackOnly`); with no frame open
  every hook returns at once. The exporter takes the pragma snapshots, the
  macro effects and the `__COUNTER__`-family decline the producer used to.
- *Writing.* `setPartFilter` as in the experiment, plus a fourth predicate:
  the header-search table keeps only files entered in the region, so a
  loading compile marks as included only what reading the header would have.
  A part never names a declaration from a loaded copy: those are left out of
  the lookup tables and C name chains (one can be in a lookup result when a
  copy was loaded after the header ended, through the parser's lookahead).
- *Metadata.* `DeclarationDependency`: every written declaration from
  outside the region, with a fingerprint (`declarationFingerprint`: kind,
  canonical type, asm label, and the ODR hash of a definition). At a hit the
  name is looked up at file scope, and the live declaration must have the same
  fingerprint, or a definition where the part carries only a declaration.
  `IncludeDependency::Name` and `HeaderDependencySet::HeaderName`: a part
  holds its files under the names its compile opened them by, so a hit
  requires the same names (only for parts; a producer copy's names are its
  own). The module name hashes the stored path, the compile's module hash
  (`CompilerInvocation::getModuleHash`) and the metadata, so the same part from
  two compiles is one entry. Encoding version 6.
- *Serving.* A part is its module's top level; `load` takes the top module
  when there is no `target` submodule.
- *A limit.* A part imports nothing, so it cannot refer to a declaration from
  a copy this compile loaded. A compile that has loaded a copy before a header
  ends does not export that header ("a saved copy was loaded before the
  header"). In practice the first hit ends a compile's exports: on `anet.c`, a
  compile that exported as it went soon hit its own part of the unguarded
  `stddef.h`, and declined 171 of the rest. So there are three modes
  (`CLANG_HEADER_CACHE_EXPORT_PARTS`): unset, serve and export misses until
  the first load; `0`, serve only; `populate`, serve nothing and export every
  listed header no stored copy fits. Parts that import the copies they refer
  to (the import graph of 2.1) would remove the limit; not built.

Six redis files on one daemon (`scratch/e2e/export_e2e.py`, debug clang,
`-O2 -S`), every listed header (`-ask-cutoff-us 0`): pass 1 times the
headers, pass 2 populates, pass 3 serves. All 18 compiles gave assembly
identical to a plain compile's, the populating ones included. `object.c`,
which stopped with conflicts between copies built after different `.c`
files (section 3), compiles. The daemon held 492 parts of 278 headers,
31 MB.

| File | Pass 3 hits / lookups |
|---|---|
| `anet.c` | 21 / 23 |
| `sds.c` | 16 / 20 |
| `zmalloc.c` | 22 / 26 |
| `util.c` | 34 / 40 |
| `ae.c` | 12 / 12 |
| `object.c` | 6 / 6 |

A lookup is one per header the compile reached; a header inside a served
one is never asked about. Rejections count candidates, and most were other
variants of the same header (`__need_size_t`, `__USE_GNU`); 23 were outside
declarations that differed (`_IO_FILE`, `stack_t`, `vprintf`), which a load
would have turned into a merge error.

## 3. Things that turned out to be wrong

Kept because the reasoning that produced them is likely to recur.

- **Two misses were blamed on missing replay.** NEXT.md said a second
  `#include` of a `#pragma once` header would not be skipped after a hit; a
  test showed the module file's header info already marks it included. And
  section 4 blamed redis's `_BSD_SOURCE` miss on unreplayed macros; the
  rejection log (`CLANG_HEADER_CACHE_STATS=2`) showed it is 1.5 instead:
  `fmacros.h` defines `_BSD_SOURCE` and then includes `<features.h>`, which
  tests it, so the saved copy of `fmacros.h` requires the flag to be set
  before `fmacros.h` sets it.

- **PCM was adopted without stating what it cannot do.** 2.2 compared PCH
  and PCM only on when a file may be loaded. The property that decides fit
  went unstated: a PCM can refer outside itself only to other PCMs, never to
  declarations the loading compile parsed as text. Our model is a header plus
  references to whatever its includer already has -- a dependency. To fit, each
  saved copy carried its whole context in a hidden submodule. Measured on qemu
  `hw/arm/virt.c` (2026-09-14, release clang-18): 68 PCMs, 460 MiB, 128 s of
  compile time to produce, and loading them fails (clang-18: "`CacheType` must
  be defined before it is used"; our clang 22: assertion at
  `IdentifierResolver.cpp:85`). One PCM for the whole file: 9.1 MiB, 0.84 s,
  identical machine code. This should have been stated before 2.2 was adopted.
  Direction as of 2026-09-15: treat Clang's serialization code as a library to
  reuse parts of, not its module format as our storage format.

- **The conditional check read stale macro state after a hit.** A hit loads
  a module, and every identifier that module mentions and the compile had
  already seen is marked out of date (`ASTReader.cpp:4941`); its macro bit
  stays as it was until something updates it. Lexing does
  (`HandleIdentifier`), but `Preprocessor::isMacroDefined` does not. So
  `main.c` that tested `FEATURE`, then took `a.h` -- which defines it -- from
  the cache, then reached `b.h`, matched `b.h`'s record "FEATURE undefined" and
  was served the wrong parse. Found 2026-09-18 while adding the syntactic
  check, which had the same hole; both now refresh the identifier first
  (`Preprocessor::refreshIdentifier`), and `HitTest` fails for each check
  with the refresh removed.

- **"Sound by construction" was false.** The provenance check (a macro is a
  dependency only if its definition lies outside the header's closure) was
  presented as making the design sound by construction. Ten adversarial
  attacks produced ten silently-corrupting counterexamples. The design is
  sound only where the recorder actually observes the relevant event, which is
  an empirical property, not a structural one.

- **The dependency set is fail-open.** Records are built by observing
  callbacks, so anything unobserved is an *omission*, and an omitted
  dependency makes an entry match *more* compiles, not fewer. Every gap found
  so far — `#if UNDEFINED > 5`, skipped `#include`s, `#elifdef`, incoming
  pragma state, the whole outgoing-effect category — was found by constructing
  a counterexample by hand. There is no systematic method and no test that
  reports "the record is missing something."

- **The plan described a server that was never written.** It cited
  `ListeningSocket` as part of the token tier's implementation. Only the client
  half exists: `SocketCachePayloadSink::send` writes a length-prefixed payload
  and never reads. `deserialize` is written but has no caller. The token tier
  can record and ship; nothing reads any of it back.

- **Context was replayed by absolute path, which broke `#include_next`.** A
  resolved path names the file but not where in the search list it was found.
  Found by the `anet.h` smoke test; fixed in 2.9. The first proposed fix
  replaced the path with the spelling; the right fix keeps both, the path to
  check the replay against.

- **The unloadable-copy failure was blamed on the wrong check.** The first
  diagnosis said the reader validates every input file when a saved copy is
  loaded. That validation is skipped for explicitly loaded module files
  (`ASTReader.cpp:3202`). The failure comes from a later read, and which read
  is still unpinned: a compile that includes a header the saved copy also
  contains fails, one that never touches such a header does not. Embedding
  ended the dependence on those files being on disk, so the exact path stopped
  mattering.

- **The declaration-boundary check rejected almost every `#include`.**
  `InDeclGroup` was held for the whole of `ParseDeclGroup`, including while
  the terminating `;` was consumed. The parser reads one token ahead, so
  consuming that `;` lexes the next token -- and when the next line is an
  `#include`, the preprocessor processes it right then, with the flag still
  set. Every `#include` that follows a declaration read as being inside it;
  so did one after a function definition, whose closing `}` is also consumed
  inside `ParseDeclGroup`. Nothing had ever called the check, so nothing had
  noticed. Found by the first hit-path test: `use.h` was never even looked
  up. Fixed by restoring the flag before the `;` and before a function body.

- **The first redis run missed every lookup.** The producer stores absolute
  paths; redis compiles `anet.c` from its own directory, so the compile named
  the header `anet.h`. The daemon's own count gave it away -- `lookups 18 (0
  with entries)`. The unit tests had passed because they used absolute paths
  throughout. Fixed by normalising the lookup name; `HitTest` now covers a
  relatively named header.

- **The stats line was printed from a destructor that never runs.** cc1 runs
  with `-disable-free`, so the preprocessor that owns the hit path is never
  destroyed. The line is now printed when the main file ends.

- **"25,027 tests passing" was Clang's own regression suite**, not evidence
  about this work. At the time the cache had six tests of its own.

- **Cacheability percentages measured the producer, not the headers.** 42% of
  curl's and 71% of redis's headers were reported cacheable, with the rest
  presented as not self-contained. The declines were the producer compiling
  each header with nothing in front of it. See 2.1 and 2.9.

- **The producer read its results after they were freed.** It consulted the
  dependency tracker and the Sema observer after `ExecuteAction` returned, by
  which point both are destroyed. It worked by luck. Results are now copied
  out while the compile is still running.

---

### Copies built after different .c files conflict (found 2026-09-30)

Every saved copy carries its context in a hidden submodule. Hidden from name
lookup, but not from declaration merging: when two loaded copies, or a copy
and the live compile, declare the same entity differently, Clang reports an
error. Two cases on redis, once copies from several .c files shared a daemon:

- `ae.c`, `util.c`: `fd_set::__fds_bits` from one copy's context is not
  present in `fd_set` from another copy's context. sys/select.h names the
  member `fds_bits` or `__fds_bits` depending on `__USE_XOPEN`, and the two
  copies were built after different .c files' feature macros.
- `object.c`, `zmalloc.c`: "conflicting asm label" on stdio.h's `fscanf`,
  which `__REDIRECT`s to a different symbol depending on feature macros.

A copy's metadata covers only its target part, so nothing checks that the
context's declarations agree with the compile loading it. This is the
duplicate-merging cost that layout D names and that stopped Google's
pre-parsed headers (N3426: "merging symbols is technically difficult").
These failures stop the compile with an error. Not fixed; needs a decision
(NEXT.md). In a later run `object.c` also loaded a copy of `server.h` whose
load reported the `fd_set` error, then read `server.h` as text, and every
enumerator in it was a redefinition.

Traced 2026-09-30 to a real pair. `sds.c` starts with `<stdio.h>` and
`<stdlib.h>`; inside `stdlib.h` glibc reaches `bits/types/clock_t.h`, nine
lines declaring `clock_t`. Its copy, built while compiling `sds.c`, replayed
38 lines in front, all of `<stdio.h>` first, without `_GNU_SOURCE`, so the
copy declares `fscanf` as `__isoc99_fscanf`. `ae.c` has `_GNU_SOURCE`, loads
that copy (clock_t.h's own text never tests it), then reads `stdio.h`, which
declares `fscanf` as `__isoc23_fscanf`: "conflicting asm label". clock_t.h
needed none of the replayed lines; it includes `bits/types.h` itself.

The user does not accept the replayed #includes ("I don't buy context"): a
copy should hold only its header and what the header includes, with
everything from outside a dependency. Measured by rebuilding every header
`sds.c` and `ae.c` read with only the recorded `#define` lines in front: 159
of 170 build for `sds.c` (114 with the replay), 99 of 110 for `ae.c` (67),
and the mean copy shrinks from 235 KB to 40 KB and from 360 KB to 47 KB. Of
the 11 that fail per file, a few need a macro an earlier header defined
(`__BEGIN_DECLS`, from `sys/cdefs.h`) and the rest need a type from another
header (`anet.h`'s `uint32_t`, `__dev_t`, `size_t`). Proposed, awaiting the
user: in front of a header, a `#define` for every macro defined at its
#include and nothing else; a header that still needs an outside type is not
saved for now.

Also found and fixed on the way:
- A client attached to the default region path whatever region its daemon
  wrote, and read names and saved copies at offsets meant for another file.
  The daemon now answers a hello with its region path and instance id; the
  client attaches to that file and refuses one whose id differs.
- An unguarded header served twice (stddef.h) loaded the same module twice,
  a fatal error. A copy already loaded in the compile is reused.
- Two rejection paths gave no reason; they now say which list was damaged.

### `__FILE__` is not the same in every compile (found 2026-10-01)

2.7 says `__FILE__` names the current file, so a use inside a header is the
same in every translation unit. Wrong: it is the path the compile used to
open the file. redis `ae.c` includes `ae_epoll.c`, found beside `ae.c`, so a
normal compile from `src/` spells it `./ae_epoll.c`. The job recorder replays
an include found beside its includer by absolute path (the producer's wrapper
sits in another directory), so the saved copy holds
`/home/.../redis/src/ae_epoll.c`. With all six redis files on one daemon,
`ae.c` compiled cleanly and its assembly differed from an uncached compile in
exactly that string: a different program, not an error.

Approved 2026-10-02 and built: `FileNameDependency`. The tracker records the
text each `__FILE__` and `__FILE_NAME__` produced, and which file it was
expanded in: the header itself, or the file a recorded `#include` found
(next entry). A hit redoes those lookups, takes the name each found, and
rejects the copy if any text differs. A `#line` in a file's own text names
the file, so a use after one is not recorded. (The first version worked
names out only for files found beside the header; the include-path fix
replaced that with the recorded lookups the same day.)

Two gaps remain. A file the compile already read under another name keeps
the first name; the check asks the source manager for it, but a copy that is
served does not register its files, so a later textual `#include` of the same
file under a third name gets that name where a textual compile would have
used the first. And nothing checks that this compile's include path finds the
same files the copy's did; see the next entry. Both need a decision.

The fix makes `ae.c` correct, not faster: the producer still opens
`ae_epoll.c` by absolute path, so its copy never matches a compile that says
`./ae_epoll.c`. Opening it by the compile's own name would need the
producer's wrapper to sit in the includer's directory. Not proposed yet.


### The include path is not checked (found 2026-10-02)

The lookup key is the absolute path of the header the `#include` names, so a
hit always loads a copy of that same file. The files that header includes
are a different matter: the producer found them through its own `-I` list,
and nothing compares that list, or what it found, with the compile loading
the copy. The module reader checks only the module cache path
(`PCHValidator::ReadHeaderSearchOptions`); it does not override
`ReadHeaderSearchPaths`.

qemu shows the case. qemu compiles some files once per target CPU, with
`-I../target/arm` in one compile and `-I../target/i386` in another (20 source
files of its build appear with more than one target directory).
`include/system/kvm.h` says `#ifdef COMPILING_PER_TARGET` /
`#include "cpu.h"`, which finds `target/arm/cpu.h` in one compile and
`target/i386/cpu.h` in the other. A copy of `kvm.h` built in the arm compile
holds arm's `cpu.h`, and its metadata says nothing about which `cpu.h`.

The user (2026-10-02): if the PCM does not check this, the cache must, and
what it needs belongs in the metadata on the server. Two ways, proposed:

- **Store the include path** with each copy and require the loading compile's
  to be identical. One comparison per hit, but it refuses every copy across
  qemu's targets, even for the many headers that find the same files in
  every target.
- **Store each `#include`'s result** -- the text as written and the file
  found -- for every `#include` in the header's region, entered or skipped,
  and at a hit redo each lookup through the compile's own `HeaderSearch` and
  compare. Exact: only an `#include` that finds a different file rejects.
  About one cached lookup per `#include`; `stdio.h` enters 34 other files,
  plus the `#include`s its guards skip.
  `#include_next` needs the directory each file was found in, which the
  lookup returns. Comparing the name found, not only the file, also settles
  `__FILE__` for files the include path found.

Recommended: the second. Approved 2026-10-02 and built:
`IncludeDependency` records every `#include`, `#include_next`,
`__has_include` and `__has_include_next` in the header's region, entered or
skipped, with the file found (by real path) and the entry whose file holds
the directive. At a hit, `redoLookups` (`CacheClient.cpp`) repeats each one
through the compile's `HeaderSearch`, from the header's own name and search
directory (the hook now passes the directory), and rejects on the first
that finds a different file. `FileNameDependency` now names the entry that
found its file instead of a path under the header's directory, so
`__FILE__` is checked for every file, include path or not. With
`-fms-compatibility`, a quoted lookup also searches beside every open file,
which one includer cannot reproduce, so such a frame is poisoned. Test: a
copy built with `-Ia` is served under `-Ia` and not under `-Ib`, where
`<cfg.h>` finds another file.

## 4. Open

- The `pop_macro` hole: a macro's textual definition location is not trustworthy
  as provenance after `#pragma push_macro`/`pop_macro`. Fix agreed (record the
  popped macro as an ExpandsTo syntactic dependency), not written.
- Token tier ships a duplicate payload when a header is re-included.
- A compile keeps producing a request whose saved copy it has already
  rejected. redis `setcpuaffinity.c` line 27 is `#define _GNU_SOURCE`, before
  `#include <stdlib.h>`. The producer replays only `#include` lines, so it
  compiles `<stdlib.h>` without `_GNU_SOURCE`, and the record says
  `_GNU_SOURCE` was not defined. The compile has it defined, rejects the saved
  copy, and queues the same request again, which produces the same bytes.
  Skip considered and deferred (2026-09-14): the lookup reply names each
  rejected candidate's module, and a compile does not queue a request whose
  `moduleNameFor` equals one of them. Safe because the same request always
  produces the same bytes (2.11) and a stored entry is never rewritten after
  it is written. The fix for the example itself is to replay the `.c` file's
  own `#define`/`#undef` lines in the context -- a separate design, not yet
  proposed.
- `__COUNTER__` and the other translation-unit-varying builtins: declined for
  now (2.7). Rewriting values on load cannot handle the commonest use,
  `CAT(tmp_, __COUNTER__)`, which puts the value into a declaration's name.
  The alternative is a dependency and an effect: the counter's value where
  the header starts must match, and the live counter is advanced by the
  number the header used. Recommended 2026-09-29 to build neither, on a search
  that missed macros which expand to the counter; withdrawn the same day (2.7,
  update 2026-09-29).

  Where the numbers land, measured 2026-09-29 over every `.c` file of qemu and
  the Linux kernel (the counter replaced by a marker word; `-Wundef` for
  `#if`): never in an `#if`; in names in all of qemu's uses and most of the
  kernel's (glib's `_g_boolean_var_N`, `__compiletime_assert_N`,
  `__UNIQUE_ID_x_N`); in inline-asm labels and their `"i"` operands, from the
  kernel's `WARN_ON` via `instrumentation_begin()`; and in error-message and
  `WARN` strings that quote an expanded name. None feeds a computation: no
  bare number reaches an enum value, an array size or a constant.

  Proposed 2026-09-29, at the user's push for a real algorithm: relocate the
  numbers on load, as a loader relocates addresses. The producer starts the
  counter at a base no compile reaches (1,000,000,000), so each number it hands
  out is a ten-digit value, and records where each one landed; a spare
  `Token` flag bit (three are free) carried through `##` and `#` makes that
  exact. The metadata keeps the base and the count. On a hit the loader
  rewrites each number to the live counter plus its offset -- in identifiers
  through the existing deserialization listener and
  `ASTReader::SetIdentifierInfo`, which `DecodeIdentifierInfo` honours before
  returning; in asm and string literals through a small reader hook -- then
  advances the live counter by the count. Dependencies naming a relocated
  identifier are checked under its rewritten spelling. A number that fed a
  computation, a directive or an `#if` cannot be rewritten; such a copy keeps
  the exact-start dependency above. Reuse, counting only the counter: qemu
  5,377 of 5,924 declined includes, the Linux kernel 10,145 of 11,319 -- every
  one but each header's first build. Names alone give qemu the same and the
  kernel 6,422; an exact start gives 5,203 and 5,549. Deferred 2026-09-30
  (2.7): the exact-start dependency is the default, and relocation is worked
  out only if the first round of benchmarks shows poor reuse.
- A population request whose context cannot be reproduced fails every time it
  is retried; the daemon should remember the failure.
- Overlap the lookup round trip with the work around it. `receiveMessage`
  blocks by design and a Unix-socket round trip costs tens of microseconds,
  once per `#include`. `HandleIncludeDirective` resolves the filename, then
  stats the filesystem across every `-I` path, then checks the guard, and only
  then decides what to do. Sending the request once the filename resolves and
  reading the reply at the decision point hides the round trip behind work the
  compiler performs regardless. There is nothing to precompute while waiting:
  `hasMacroDefinition` is a bit Clang already maintains (1.12).
- Batch the lookups: one request naming many headers, one reply, one round
  trip. Partly superseded by 2.14, which removes the round trip for every
  header the region does not list. The compiler cannot know its own includes in advance, since conditionals
  gate them, but the daemon can -- it has already seen which headers a given
  main file asked for, and in what order, on an earlier build, so predicting
  from that history needs no compiler change. Pairs with the client-side mirror
  deferred earlier.
- A dynamic fold decision, replacing 2.15's fixed cutoff. Measure both sides
  in real compiles -- each header's parse time and each PCM's load time, which
  grows with the number of PCMs already loaded -- and fold where parsing is
  cheaper. The user and a labmate disagree on what the criterion should be;
  measured times on both sides settle it. Deferred 2026-09-25 as hard to build
  now.
- Fold small headers and single-parent headers into their includer's saved
  copy instead of giving them their own. Superseded as a rule by 2.15, which
  decides by processing time, not by size or parent count; the measurements
  below still stand. Each extra PCM costs about 250 us to
  load (release clang-18), rising to 400 us at 300 PCMs because in C every
  identifier lookup visits every loaded PCM (`ASTReader.cpp:9206`). A header of
  ~20 declarations parses in about 220 us, so below that size a separate copy
  loses to plain parsing: 100 small headers took 78 ms as separate PCMs, 52 ms
  as text, 52 ms folded into one. A header with one parent folds with no
  duplication -- 65.7% of redis's shared headers, 52.6% of git's -- though one
  parent is a property of a build, not of a header. Large headers with many
  parents keep their own copy, since folding them duplicates them everywhere.
  Duplicate-merge cost not yet isolated. Scripts:
  `scratch/loadtime/{synth,real}.py`, `benchmarks/ctxvariety.py`.
- Most misses on redis are false rejections from conditionals, not from the
  syntactic check. Measured 2026-09-18, each `.c` file's direct includes built
  in the context of the includes before them: anet.c served 6 of 18, sds.c 1
  of 9, object.c 3 of 6, with identical assembly each time and no candidate
  rejected for a syntactic reason. Every rejection was a conditional recorded
  "defined" and live "undefined". Most were transient macros a header sets for
  itself -- glibc `#define`s `__need_size_t` or
  `__GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION`, includes a header that
  tests and `#undef`s it -- which 1.5 records without a provenance filter, so
  no includer can ever match. The rest was `_BSD_SOURCE` in `fmacros.h`, the
  same case: `fmacros.h` defines it and `<features.h>` tests it (section 3).
  The 1.5 filter was deferred as an efficiency improvement; this is its cost.
  Rerun 2026-09-29 with macro replay and the rejection log: same counts, and
  every rejection named a macro the header's own closure set.
- A cheaper syntactic check, adapted from Bothner's GCC compile server (2003,
  §8.2). Proposed 2026-09-22, not approved, not built. The check in 1.12 asks
  "is this a macro here?" for every NotAMacro name of a saved copy, about 500
  lookups per hit. Bothner checks at the `#define` instead of at the include.
  The daemon keeps one build-wide set of every name any saved copy uses as a
  plain name, in the shared region. Each time the compile makes a name a macro
  -- `#define`, `#pragma pop_macro`, builtins, all through
  `Preprocessor::appendMacroDirective` -- it looks the name up in that set and,
  on a match, adds it to a short list of its own. Before serving a saved copy,
  it checks only the short list against that copy's names. The file a
  `#define` sits in plays no part: a `-DUSE_X` that switches on
  `#define grab other` inside a header is caught the same way as one in the
  `.c` file. A copy stored after the compile started has names the set did not
  hold when the compile's earlier `#define`s were looked up; a counter the
  daemon bumps on every addition, read at startup, sends such copies to the
  full check.

  Measured over every `.c` file of five projects with their real flags
  (`scratch/names/union3.py`): the set is 6,749 names in 94 KB (nginx) to
  238,507 in 5.1 MB (qemu), still growing at qemu's last file. The short list
  is ~600 names per `.c` file -- no saving -- because glibc writes
  `#define _CS_PATH _CS_PATH` after about 500 enum constants and
  `#define stdin stdin` to satisfy the standard's "is a macro". Such a macro
  still yields the token it replaced, so the saved copy parsed the same
  tokens; leaving these out gives median 9 (FFmpeg) to 114 (qemu). Whether the
  conditional check alone covers `#ifdef` on such a name is not verified.

  An empty `#define X` needs no rule of its own. It is a flag or an include
  guard, whose name appears only in `#ifdef`/`#ifndef`, never as a plain name
  in a header, so it never enters the set and is never checked: across all
  7,000+ `.c` files, 5 names in qemu and none elsewhere, each an unrelated
  coincidence (`audio/mixeng.c`'s `#define SIGNED` flag against the enum
  constant `SIGNED` in a hexagon header it never includes). A rule to ignore
  empty `#define`s is rejected anyway: a header that did use such a name as a
  plain name would lose that token, and the set lookup catches it at no cost.

  Whether any of this is worth building depends on what the 500 lookups cost
  now, which is not measured; in C each lookup visits every loaded PCM
  (`ASTReader.cpp:9206`).
- Correctness will be established by building the benchmark projects with the
  mechanism enabled and running each project's own test suite. This bounds risk
  over the code those suites exercise; it does not establish that the record is
  complete.
