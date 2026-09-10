# Header cache — decision record

Why the code in `clang/lib/ASTCache`, `clang/lib/CacheObserver`, and the
observation hooks in Lex/Parse/Sema are shaped the way they are. Each entry
records what was decided, the reason, and what was rejected — the reasons are
the part that doesn't survive in the code.

Last updated 2026-09-05.

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

The cost is a scope limit: the header must compile on its own. One that assumes
its includer already provided something fails in isolation and is declined —
which is the same self-containment the piggyback design would have needed
anyway, discovered earlier and more cleanly.

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

---

## 4. Open

- The `pop_macro` hole: a macro's textual definition location is not trustworthy
  as provenance after `#pragma push_macro`/`pop_macro`. Fix agreed (record the
  popped macro as a value dependency), not written.
- Token tier ships a duplicate payload when a header is re-included.
- Nothing reads a cache. No hit path, no daemon, no measurements.
- Correctness will be established by building the benchmark projects with the
  mechanism enabled and running each project's own test suite. This bounds risk
  over the code those suites exercise; it does not establish that the record is
  complete.
