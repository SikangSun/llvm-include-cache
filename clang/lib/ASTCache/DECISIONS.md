# Header cache — decision record

Why the code in `clang/lib/ASTCache`, `clang/lib/CacheObserver`, and the
observation hooks in Lex/Parse/Sema are shaped the way they are. Each entry
records what was decided, the reason, and what was rejected — the reasons are
the part that doesn't survive in the code.

Last updated 2026-09-11.

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

Value dependencies are filtered — a macro whose `#define` sits inside the
frame's own closure is not recorded. Conditionals are not filtered, even
though the same refinement would apply.

Over-recording is always safe: it can only make validation re-check something
that was in fact invariant. Under-recording is never safe. The filter for
conditionals is also harder to state correctly, because an *undefined* macro
has no `#define` location to test provenance against. Deferred as an
efficiency question, not a correctness one.

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
other two sit after the C early-exit in
`TryAnnotateTypeOrScopeTokenAfterScopeSpec`. C++ overload sets are a
substantially harder problem and are deferred; reaching a C++-only query
poisons the frame (1.6) rather than recording something unvalidated.

---

## 2. What gets stored and where

### 2.1 A separate sub-compile of the header, not a slice of the real compile

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

The context goes in through a generated wrapper header rather than `-include`
flags. `-include` lists are among the preprocessor options the reader
validates on load (`checkPreprocessorOptions`); keeping context out of them
means the saved copy's options are exactly the compile's own.

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

---

## 3. Things that turned out to be wrong

Kept because the reasoning that produced them is likely to recur.

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

## 4. Open

- The `pop_macro` hole: a macro's textual definition location is not trustworthy
  as provenance after `#pragma push_macro`/`pop_macro`. Fix agreed (record the
  popped macro as a value dependency), not written.
- Token tier ships a duplicate payload when a header is re-included.
- No hit path: nothing inside the compiler asks the cache at an `#include`
  yet. The daemon, protocol, producer, and the client's matching exist and are
  tested.
- `__COUNTER__` and the other translation-unit-varying builtins are saved
  wrong today; see 2.7.
- Loading a saved copy must leave the includer as a textual include would:
  the guard macro defined and the file marked as included, so a second
  `#include` of the same header is skipped. Not written.
- A population request whose context cannot be reproduced fails every time it
  is retried; the daemon should remember the failure.
- **Replaying context by absolute path breaks `#include_next`.** The wrappers
  write `#include "/abs/path/h.h"`. `#include_next` inside a header reached
  that way cannot tell where in the search path it stands, so it searches from
  the start. Clang's own `<stdint.h>` does `#include_next <stdint.h>` to reach
  glibc's; by absolute path it finds itself again, its guard is already
  defined, and `uint32_t` is never declared. Found by the `anet.h` smoke test
  (context `<stdint.h>`, declined: unknown type name 'uint32_t'). The target
  wrapper has the same flaw, and there it can produce a wrong copy rather than
  a decline. Fix proposed, not written: record each include as written
  (`<stdint.h>` or `"anet.h"`) and replay it with the `.c` file's directory
  first on the quoted search path.
- Correctness will be established by building the benchmark projects with the
  mechanism enabled and running each project's own test suite. This bounds risk
  over the code those suites exercise; it does not establish that the record is
  complete.
