# Header cache — what was built, file by file

A narrative of everything added for the **AST tier** of the header cache: what
each file is, why it exists, and, for files that already existed in Clang, what
was changed inside them. It is written to be read start to finish, and to be
checked against the code.

The **token tier** (`clang/lib/CacheObserver`, `clang/include/clang/CacheObserver`)
came earlier and is unchanged by this work. It is summarised once, in §2, only
where the AST tier reuses something from it.

Scope of the whole design: **C only**. C++ overload sets are out.

Everything here is against upstream `ca7933e47`; §11 maps every file to the
commits that changed it. Line counts are from
`git diff --stat ca7933e47..HEAD`.

---

## 1. The one-paragraph version

A compile that reaches `#include "anet.h"` should be able to load a
previously parsed `anet.h` instead of parsing the text again. To do that
safely, three things must exist: a way to **record** what a parse of `anet.h`
depended on (§4), a way to **build and store** that parsed form (§5, §6), and
a way to **check** at a later `#include` that the recorded dependencies still
hold (§6.4). All three exist and are tested. The fourth part — the compiler asking
the cache at an `#include` — exists as of `ab65051ab` (§6.6). What remains is
filling the cache from inside a compile (§8).

---

## 2. What is running today, and what is not

This is the part worth checking first, because most of the code is machinery
that nothing inside the compiler calls yet.

**Runs today:**

- `clang-header-cache-populate`, a command-line tool: given a header, flags,
  and the headers included before it, it builds a PCM for that header and
  either reports on it (`-dry-run`) or pushes it to the daemon.
- `clang-header-cache-daemon`: holds pushed PCMs in a shared memory region and
  answers lookups by header path.
- The hit path, when `CLANG_HEADER_CACHE` names a daemon socket (§6.6,
  `ab65051ab`). Unset, nothing is installed.
- 61 unit tests.

**Changed by the hit path (`ab65051ab`):** `Parser::isAtDeclarationBoundary()`
and `CacheClient` now have a real caller, and `clang/lib/Lex/PPDirectives.cpp`
asks the hook at the top of `case Enter:` (the original marker comment, lower
down, is untouched).

**Does not exist at all:** the miss path, population from inside a compile,
and the `__COUNTER__` backfill (§8).

---

## 3. Observation hooks added to Clang itself

The cache needs to see things Clang did not previously report. Each hook is
small, off by default, and shaped after an existing Clang interface so it
looks like the surrounding code.

### 3.1 Which files are inside a header's own closure

- **`clang/include/clang/Lex/TransitiveClosureTracker.h` (56 lines, new)**
- **`clang/lib/Lex/TransitiveClosureTracker.cpp` (34 lines, new)**
- **`clang/lib/Lex/CMakeLists.txt` (+1 line)** to build it.

Tracks the set of real files entered while the preprocessor runs, fed from
`PPCallbacks::FileChanged`. Synthetic buffers — the predefines buffer, `-D`
buffers — have no `FileEntry` and are filtered out. Both tiers use it. The AST
tier's question is: *was this macro defined inside the files this header
already pulled in, or outside them?*

### 3.2 An identifier that was never defined

- **`clang/include/clang/Lex/PPCallbacks.h` (+22 lines)** — adds
  `UndefinedIdentifierInDirective(const Token &)`, plus forwarding in
  `PPChainedCallbacks` so a chain of observers all receive it.
- **`clang/lib/Lex/PPExpressions.cpp` (+6 lines, at `EvaluateValue`)** — fires
  the new callback.

Why it was needed: `#if SOME_UNDEFINED > 5` reaches no existing callback. The
expression evaluator substitutes `0` for an unknown identifier without going
through macro expansion, so nothing reported the name, and a header whose
meaning depended on that name being undefined would have been cached with no
dependency on it. The callback fires only when the branch is actually
evaluated (`ValueLive`), so a branch that was skipped records nothing.

The alternative — adding the identifier list to `PPCallbacks::If`/`Elif` —
would have changed signatures that 17 in-tree overrides implement, for a
feature none of them want.

### 3.3 Is this `#include` at a safe point

- **`clang/include/clang/Lex/DeclBoundaryObserver.h` (42 lines, new)** — a
  one-method interface, `bool isAtDeclarationBoundary() const`, modelled on
  `CodeCompletionHandler`.
- **`clang/include/clang/Lex/Preprocessor.h` (+23 lines)** — one member plus
  `setDeclBoundaryObserver` / `getDeclBoundaryObserver` /
  `clearDeclBoundaryObserver`.
- **`clang/lib/Lex/Preprocessor.cpp` (+3 lines)** — the include, and an
  out-of-line destructor so the class has a home for its vtable.
- **`clang/include/clang/Parse/Parser.h` (+21 lines)** — `Parser` implements
  the interface: `ParenCount == 0 && BraceCount == 0 && BracketCount == 0 &&
  !InDeclGroup`.
- **`clang/include/clang/Parse/RAIIObjectsForParser.h` (+25 lines)** —
  `ParsingDeclGroupRAII`, which sets `InDeclGroup`.
- **`clang/lib/Parse/ParseDecl.cpp` (+6 lines)** — that RAII object wraps the
  body of `ParseDeclGroup`.
- **`clang/lib/Parse/Parser.cpp` (+3 lines)** — registers the parser as the
  observer in the constructor, clears it in the destructor.

Why `InDeclGroup` exists separately: in `int a, b;` an `#include` between
`a,` and `b;` touches none of the three nesting counters, so without the flag
the preprocessor would think that point was a safe boundary.

The hit path (§6.6) asks it at every eligible `#include`. Its first real
use exposed that it rejected almost everything: `InDeclGroup` was held while
the terminating `;` was consumed, and because the parser reads one token
ahead, consuming that `;` processes the `#include` on the next line with the
flag still set. `ParseDeclGroup` now restores the flag before the `;` and
before a function body (`ab65051ab`).

### 3.4 What an identifier currently means

- **`clang/include/clang/Sema/NameClassificationObserver.h` (100 lines, new)**
  — four methods, one per query Clang's parser uses to resolve grammar
  ambiguity: `recordTypeNameQuery`, `recordClassification`,
  `recordTemplateNameQuery`, `recordDeductionGuideNameQuery`.
- **`clang/include/clang/Sema/Sema.h` (+84 lines)** — the observer pointer,
  its set/get/clear, and declarations of the four renamed `…Impl` functions.
- **`clang/lib/Sema/SemaDecl.cpp` (+61 lines)** — `getTypeName` and
  `ClassifyName` each become a thin wrapper around a renamed `…Impl` holding
  the original body, reporting once at the wrapper's single return.
- **`clang/lib/Sema/SemaTemplate.cpp` (+29 lines)** — the same for
  `isTemplateName` and `isDeductionGuideName`.

Why wrappers and not edits at each `return`: `getTypeName` has 24 return
points across 321 lines and `ClassifyName` 22 across ~390. Editing each one
would be invasive and easy to get wrong.

Why these four: `clang/docs/InternalsManual.rst:1252` states the parser's own
discipline — every `tok::identifier` or `tok::coloncolon` goes through
`TryAnnotateTypeOrScopeToken` or `TryAnnotateCXXScopeToken`. Tracing every
call site in `clang/lib/Parse` yields exactly these four entry points. Two
others were deliberately left alone: `isCurrentClassName`, which cannot vary
independently of things already tracked, and the error-recovery paths
(`isTagName`, `DiagnoseUnknownTypeName`), where reaching them excludes the
header from caching instead.

Cost to an ordinary compile: one null check per call.

---

## 4. The data model, and collecting it

### 4.1 `clang/include/clang/ASTCache/DependencyRecord.h` (143 lines)

What a parsed header can depend on, in five pieces:

| Struct | Records |
|---|---|
| `ConditionalDependency` | A macro whose *definedness* was tested, and the answer. |
| `ValueDependency` | A macro that was *expanded*, and its replacement list as text. |
| `SymbolDependency` | An identifier a grammar-ambiguity query asked about, which query, and the answer — including the resolved type's canonical spelling. |
| `InclusionDependency` | An `#include` that did nothing because the file was already included (`#pragma once`, `#import`). |
| `PragmaSnapshot` | Pragma state, twice: `Entry`, the state the header was compiled under, and `Exit`, the state the header leaves behind. |

`HeaderDependencySet` bundles all of the above; `HeaderRecord` pairs one set
with the header's path.

Design notes worth knowing: `SymbolDependency::Classification` mirrors
`clang::NameClassificationKind`'s integer values rather than including
`Sema.h`, so the record stays cheap to include. And a macro is a dependency
**only if its definition lies outside the header's own closure** — a macro
defined and used inside the same header can never vary.

### 4.2 `clang/include/clang/ASTCache/ASTCacheDependencyTracker.h` (207) and `clang/lib/ASTCache/ASTCacheDependencyTracker.cpp` (322)

A `PPCallbacks` that also implements `NameClassificationObserver`, so one
object collects preprocessor facts and Sema facts.

The important design point is that it keeps a **stack of frames**, one per
file entered, not one set per translation unit. Headers nest, and the same
macro expansion has a different answer at each level: a macro defined in `a.h`
and used in the `b.h` that `a.h` includes is a real dependency of `b.h` and
not one of `a.h`, whose own text fixes it. So each expansion is asked the
provenance question once per open frame.

An earlier flat version got this wrong twice: it asked the question once per
translation unit, and it treated the main `.c` file as internal to everything,
which silently dropped real dependencies.

**Poisoning:** a frame that meets something the recorder cannot describe —
currently, any C++-only query, or a symbol query with no usable location — is
dropped entirely and emits no record. A partial record would carry *fewer*
constraints than the compile that produced it, so it would match compiles it
must not. Missing entirely means a miss.

### 4.3 `clang/include/clang/ASTCache/MacroTokens.h` (40) and `.cpp` (27)

One function that turns a macro's replacement list into a string. It is its
own file because two sides must produce byte-identical output: the tracker
when recording, and the client when checking a stored record against a live
macro. If those two ever drifted, every value dependency would compare
unequal and nothing would ever hit — a failure that looks exactly like the
cache simply not helping.

### 4.4 `clang/include/clang/ASTCache/PragmaStateCheck.h` (87) and `.cpp` (111)

`#pragma pack(1)` before an `#include` changes the layout of every record in
the header, and no macro is tested or expanded, so the other categories record
nothing. This file captures the pragma stacks (pack, float control) into a
`PragmaSnapshot`, and reports any pragma state a record cannot express, in
which case the header is declined.

Serialization does not solve this on its own:
`ASTWriter::WritePackPragmaOptions` returns early while a module is being
written, so the PCM carries no pragma state in either direction.

### 4.5 `clang/include/clang/ASTCache/ByteStream.h` (133), `DependencySerialization.h` (53) and `.cpp` (117)

A flat byte encoding for `HeaderDependencySet`, and the little-endian
primitives under it.

Two properties matter. Strings are length-prefixed, not NUL-terminated,
because a serialized replacement list contains NULs by construction — they
are its token separator. And the reader **stays failed**: once any read runs
past the end, every later read returns zero and `ok()` is false, so a caller
decodes a whole structure and checks once. A decoder that returned what it had
managed to read would produce a partial record, which is the unsafe direction
(§4.2).

---

## 5. Building a PCM for one header

### 5.1 `clang/include/clang/ASTCache/ASTCacheProducer.h` (194) and `clang/lib/ASTCache/ASTCacheProducer.cpp` (838)

This is the largest piece, and the one that changed most often.

**Why a separate compile at all.** The parsed form of a header cannot be
sliced out of a real compile, because of include guards: in a translation unit
that reads `bar.h` before `foo.h`, `foo.h`'s own `#include "bar.h"` does
nothing, so `bar.h`'s declarations are outside the slice while `foo.h`'s
declarations point at them. Reverse the order and the slice changes. A
separate compile of `foo.h` fires that `#include` for real, every time.

**The context.** A header like redis's `anet.h` uses `uint32_t` and never
includes `<stdint.h>`, because whatever includes `anet.h` included
`<stdint.h>` first. So a request carries the includes the `.c` file had
already performed, in order, plus the compile's own flags. Nothing about that
list is chosen by policy: it is what the compile that missed actually had.

**What the producer writes.** Clang only builds a PCM from a module map, and a
module map lists headers by path, which cannot express `#include <stdint.h>`.
So the producer writes three files into a fresh temporary directory:

```
__hc_context.h     #include <stdint.h>                     (one line per context entry)
__hc_target.h      #include "/…/redis/src/anet.h"
module.modulemap   module __hc_<hash> {
                     explicit module context { header ".../__hc_context.h" }
                     module target { header ".../__hc_target.h" export * }
                   }
```

`context` and `target` are submodule names we chose; Clang attaches no meaning
to either word.

**Why the context is hidden.** `explicit` on the `context` submodule means its
contents are only available to someone who names it. A textual `#include
"anet.h"` makes `anet.h`'s declarations visible and nothing else; if the
context became visible too, loading `anet.h` would declare all of `<stdint.h>`
in a file that never included it. The loading compile names only
`__hc_<hash>.target`.

**A consequence:** only headers included directly by a `.c` file are
populated. A header reached from inside another header would have to be
reached by including all of that other header, which puts the wanted header
inside the hidden half. The producer detects that and declines.

**Replaying an include the way the compile found it.** Each entry of a request
is an `IncludeRef` with two parts: the text as written (`<stdint.h>`) and the
file that was opened. The text matters because `#include_next` continues the
search *after* the directory the current file was found in — which is how
Clang's `stdint.h` reaches glibc's. A file opened by absolute path was found
in no directory, so its `#include_next` restarts from the top, finds Clang's
`stdint.h` again, and `uint32_t` is never declared. The recorded path is used
to check the replay: if the replay opens a different file, the producer
declines. A file the original compile found *without* searching — next to the
`.c` file, or by absolute path — is replayed by absolute path, which behaves
identically.

**Module names.** `__hc_` plus 16 hex digits of xxh3 over the header, flags
and context. Two PCMs with the same module name cannot be loaded into one
compile, and `err_module_file_conflict` is a fatal error, so the single fixed
name used at first would have made two hits in one compile impossible.

**Storing the three generated files inside the PCM.** The producer deletes its
temporary directory, but the PCM records those three files as the ones its
declarations were reached through, and a compile that touches a header the PCM
also contains then needs them on disk. Each is passed with
`-fmodules-embed-file=`, which stores its text in the PCM. Cost, measured on
redis `anet.h` with `<stdint.h>` as context: 116,744 → 117,036 bytes, +0.25%.

**Flags every cache compile uses:** `-fmodules -fno-implicit-module-maps`. The
first is required — a PCM only loads into a compile with modules enabled — and
the second stops Clang from turning any nearby header into a module of its
own. Measured cost on a normal build: redis `object.c` 3025 → 3030 ms.

**When the producer declines** (each is a miss, never a wrong answer):

- the header does not compile in the given context;
- the header was reached through its own context;
- a replayed include opened a different file than the request recorded;
- pragma state the record cannot carry;
- the header expands `__COUNTER__`, `__INCLUDE_LEVEL__`, `__BASE_FILE__`,
  `__DATE__`, `__TIME__` or `__TIMESTAMP__` in its own text, whose values
  belong to the compile that includes it, until the backfill exists;
- the recorder was poisoned (§4.2).

**Job files.** A plain-text format (`clang-header-cache-jobs 2`) carrying a
list of requests, so a compile that missed can hand its work to another
process. Values that cannot be represented, such as a flag containing a
newline, are rejected rather than silently split.

**One bug worth remembering:** the producer used to read the tracker and the
observer *after* `ExecuteAction` returned, when both had been destroyed. The
results are now copied out during `HandleTranslationUnit`.

---

## 6. Storing and serving PCMs

### 6.1 `clang/include/clang/ASTCache/SharedRegion.h` (101) and `.cpp` (155)

An append-only region, mapped by the daemon and by every compile. Append-only
is the concurrency argument: a written entry is never modified or moved, so
readers need no coordination, and the socket reply that hands out an offset is
itself the synchronisation point — a client only learns of an entry after it
was fully written. The size is fixed generously at creation because growing a
mapping can move it, which would invalidate every offset already handed out.
The backing file is sparse, so reserved space costs nothing.

### 6.2 `clang/include/clang/ASTCache/CacheProtocol.h` (141) and `.cpp` (211)

Two request types: `LOOKUP(filename)`, answered with every entry stored under
that name, each carrying its dependency record and where its bytes sit in the
region; and `PUSH(filename, record, data)`, answered with an acknowledgment.

The daemon never interprets a record. Deciding which entry fits needs the
compile's live macro table and Sema, which the daemon does not have and could
not be sent cheaply.

### 6.3 `clang/include/clang/ASTCache/CacheDaemon.h` (115) and `.cpp` (166), plus `clang/tools/clang-header-cache-daemon/` (95 + 13)

A filename → entries table over the region. No state survives a restart, by
choice: this is a proof of concept, and the daemon is not restarted during an
evaluation. Message handling is a pure function from message to reply, so it
is tested without any socket. One thread per connection, blocking reads, no
timeout — after a compile finishes its lookups it spends a long time in code
generation without touching the socket, and a parked thread costs nothing.

### 6.4 `clang/include/clang/ASTCache/CacheClient.h` (112) and `.cpp` (192)

The judgement: given a stored record and the live compile, does the entry
apply? Checks run cheapest first and stop at the first mismatch — conditional,
then value, then inclusion, then symbol, then pragma entry state. A header
with a hundred conditional dependencies usually fails on one of them, and the
symbol checks are the only ones costing a name lookup. With no `Sema`
available, everything is rejected.

The hit path (§6.6) calls it.

### 6.5 `clang/tools/clang-header-cache-populate/` (238 + 13)

The command-line front end to the producer: headers as arguments, the build's
flags with `-Xcc`, the context with `-context` (a path, or a spelling such as
`'<stdint.h>'`), or a whole job file with `-job`. `-dry-run` builds everything
and reports without needing a daemon: how many headers were saved, the mean
size of a PCM and of a record, and a tally of why the rest were declined.

### 6.6 The hit path (`ab65051ab`)

- **`clang/include/clang/Lex/HeaderCacheHook.h` (new)** — one method,
  `Module *tryServe(FileEntryRef, SourceLocation)`, modelled on
  `DeclBoundaryObserver`. Null means "enter the file as usual".
- **`clang/include/clang/Lex/Preprocessor.h` (+14) and
  `clang/lib/Lex/Preprocessor.cpp` (+7)** — the preprocessor owns the hook.
- **`clang/lib/Lex/PPDirectives.cpp` (+11)** — asked at the top of
  `case Enter:`, before the include-depth check and before a FileID is made.
  A returned module takes the same two lines as `case Import:`, so the parser
  and Sema learn of it through the ordinary annotation token.
- **`clang/include/clang/ASTCache/HeaderCacheServer.h` and
  `clang/lib/ASTCache/HeaderCacheServer.cpp` (new)** — the implementation.
  Three gates with no socket traffic (the `#include` is in the main file, at a
  declaration boundary, and Sema exists), the lookup name normalised the way
  the producer writes it, one lookup, load from shared memory, pragma replay.
- **`clang/include/clang/Frontend/CompilerInstance.h` (+11),
  `clang/lib/Frontend/CompilerInstance.cpp` (+3), `clang/tools/driver/cc1_main.cpp`
  (+6), `clang/tools/driver/CMakeLists.txt` (+1)** — a callback slot filled by
  cc1, because `clangASTCache` depends on `clangFrontend` and the dependency
  cannot also run the other way.
- **`clang/lib/Parse/ParseDecl.cpp` (+10) and
  `clang/include/clang/Parse/RAIIObjectsForParser.h` (+6)** — the boundary
  fix described in §3.3.

`CLANG_HEADER_CACHE_STATS=1` prints one line when the main file ends — not
from a destructor, because cc1 runs with `-disable-free` and destructors never
run.

**First hit on a real benchmark file.** redis `anet.c`, compiled from `src/`
with the real `clang`, after producing `anet.h` in the context of the 16
includes `anet.c` has before it: `header cache: 1 hits / 18 lookups`. The
daemon agrees (`lookups 18 (1 with entries)`), and the object file's
disassembly is identical to an uncached compile's, all 1,517 lines. 19 direct
includes gave 18 lookups because `<string.h>` had already been read through an
earlier header, so Clang skipped it before the hook is asked.

---

## 7. Tests — 61, all passing

`clang/unittests/ASTCache/` (plus `CMakeLists.txt`, 24 lines, and one line in
`clang/unittests/CMakeLists.txt`).

| File | Tests | What it pins down |
|---|---|---|
| `ASTCacheDependencyTrackerTest.cpp` (289) | 13 | Provenance: an internally defined macro is not a dependency; one from the includer is; a sub-header's macro is internal to its parent; a conditional is recorded at every open level; `#if UNDEFINED > 5` is recorded; a guard-suppressed `#include` is recorded. |
| `SymbolDependencyTest.cpp` (179) | 4 | The `T * p;` ambiguity: a typedef from the includer is a dependency; one the header declares itself is not; the resolved type's identity is recorded, not just "it was a type"; a typedef from a sub-header is internal to the parent. |
| `DependencySerializationTest.cpp` (157) | 8 | An empty set and a full set round-trip; embedded NULs survive; truncation, a version mismatch, trailing bytes and an out-of-range classification are all rejected rather than partially decoded; a corrupt count does not size an allocation. |
| `CacheProtocolTest.cpp` (220) | 12 | Message framing (length prefix, round trip, wrong kind, truncation) and daemon behaviour: a miss is clean, every variant comes back, identical pushes are discarded, distinct headers do not collide, stored bytes come back exactly, an out-of-range offset reads as nothing, garbage closes the connection. |
| `EndToEndTest.cpp` (346) | 7 | Push through a real socket, then reject the entry on each kind of mismatch in turn, and choose correctly between variants. |
| `ProducerTest.cpp` (355) | 12 | Context makes an otherwise unbuildable header buildable; a context macro becomes a conditional dependency; pragma entry state is the header's, not the compile's; a header reached through its own context is declined; module names are unique and stable; `#include_next` works only when the context is replayed as written; a replay that opens a different file is declined; a header reading `__COUNTER__` is declined; job files round-trip. |
| `LoadTest.cpp` (216) | 1 | A PCM is loaded from bytes in memory, under a name no file has, partway through a compile that already parsed the context header itself — and its declarations then work with that compile's own types. |
| `HitTest.cpp` | 4 | A stored header is served without the file being opened; an `#include` right after a function definition is served while one inside a body is not; a header the compile names relatively is still found; an entry whose record does not match is not used. |

---

## 8. What does not exist yet

1. **The miss path.** Nothing builds a request when a header is not found in
   the cache.
2. **Population from inside a compile.** Decided but unwritten: a thread
   started at the first miss, with whatever it has not finished handed to a
   detached `clang-header-cache-populate -job` when the compile exits.
3. **The `__COUNTER__` backfill.** Such headers are declined today. The plan
   is to record where each varying value landed and rewrite it on load.
4. **Loading the context's guard state.** A hit must leave the compile as a
   textual include would.
5. **The token tier's server half.** The token tier can record and ship;
   nothing reads its payloads back.

Known gaps in the record itself are listed in DECISIONS.md §3 and §4. The
honest summary: every gap so far was found by hand-built counterexample, not
by a method, and there is no test that reports "this record is missing
something".

---

## 9. Corrections made along the way

Kept because each one is a mistake that could recur.

- **"Sound by construction" was false.** The provenance check was presented as
  making the design sound by construction; ten adversarial attacks produced
  ten silently-corrupting counterexamples. Soundness holds only where the
  recorder actually observes the relevant event, which is empirical.
- **Cacheability numbers measured the producer, not the headers.** An early
  "42% cacheable" figure was a property of the producer's missing context
  support.
- **The producer read freed memory** (§5.1).
- **Replaying context by absolute path broke `#include_next`** (§5.1). The
  first proposed fix dropped the path and kept only the spelling; the right
  fix keeps both, the path being what checks the replay.
- **The unloadable-PCM failure was blamed on the wrong check.** The reader's
  up-front input-file validation is skipped for explicitly loaded modules
  (`ASTReader.cpp:3202`); the failure comes from a later read. Embedding the
  generated files removed the dependence on disk either way.
- **A comment claimed the target could be entered before `Sema` existed.**
  `Parser::Initialize` calls `Actions.Initialize()` before its first
  `ConsumeToken()`, so it cannot; the fallback that remains is defensive.
- **The declaration-boundary check rejected almost every `#include`** (§3.3,
  fixed in `ab65051ab`).
- **The first redis run missed every lookup:** the producer stores absolute
  paths and redis names `anet.h` relatively. The daemon's count gave it away,
  `lookups 18 (0 with entries)`; fixed in `ab65051ab`.

---

## 10. Where to look next

- `DECISIONS.md` — why each choice was made, and what was rejected.
- `README.md` — how to run the tools.
- This file — what exists.

---

## 11. Commit map

Every commit since upstream, oldest first. Linked ones are on
`github.com/SikangSun/llvm-include-cache`, branch `header-cache`; the ones
marked local have not been pushed.

| Commit | What it did |
|---|---|
| [`91182dc08`](https://github.com/SikangSun/llvm-include-cache/commit/91182dc08708b524cf29cae785aa38ce8fa0414a) | [clang] Per-header compilation cache: observation layer and AST tier |
| [`a5a384e19`](https://github.com/SikangSun/llvm-include-cache/commit/a5a384e19b83b6dbbf5ccbd0058ee4a3dbbe72e8) | [clang] Header cache: producer replays context into a hidden submodule |
| [`ea53c042d`](https://github.com/SikangSun/llvm-include-cache/commit/ea53c042db80c37d0f716e8dd7ab2def6ab5d54a) | [clang] Header cache: replay each include the way the compile found its file |
| [`f976d8fd4`](https://github.com/SikangSun/llvm-include-cache/commit/f976d8fd40648c2967f60567e2f0f31fa445198b) | [clang] Header cache: decline TU-varying builtins; test loading from memory |
| [`95232c6b6`](https://github.com/SikangSun/llvm-include-cache/commit/95232c6b69c0525d65e5d03a90208291491a2de0) | [clang] Header cache: store the producer's generated files in the saved copy |
| `7032a4b76` (local) | [clang] Header cache: a file-by-file walkthrough of what was built |
| `26a28d8ea` (local) | [clang] Header cache: shelve the read-set argument for the record |
| `ab65051ab` (local) | [clang] Header cache: serve an #include from the cache (the hit path) |

Which commits changed each file, oldest first:

| File | Commits |
|---|---|
| `clang/include/clang/ASTCache/ASTCacheDependencyTracker.h` | `91182dc08` |
| `clang/include/clang/ASTCache/ASTCacheProducer.h` | `91182dc08` `a5a384e19` `ea53c042d` |
| `clang/include/clang/ASTCache/ByteStream.h` | `91182dc08` |
| `clang/include/clang/ASTCache/CacheClient.h` | `91182dc08` |
| `clang/include/clang/ASTCache/CacheDaemon.h` | `91182dc08` |
| `clang/include/clang/ASTCache/CacheProtocol.h` | `91182dc08` |
| `clang/include/clang/ASTCache/DependencyRecord.h` | `91182dc08` |
| `clang/include/clang/ASTCache/DependencySerialization.h` | `91182dc08` |
| `clang/include/clang/ASTCache/HeaderCacheServer.h` | `ab65051ab` |
| `clang/include/clang/ASTCache/MacroTokens.h` | `91182dc08` |
| `clang/include/clang/ASTCache/PragmaStateCheck.h` | `91182dc08` |
| `clang/include/clang/ASTCache/SharedRegion.h` | `91182dc08` |
| `clang/include/clang/Frontend/CompilerInstance.h` | `ab65051ab` |
| `clang/include/clang/Lex/DeclBoundaryObserver.h` | `91182dc08` |
| `clang/include/clang/Lex/HeaderCacheHook.h` | `ab65051ab` |
| `clang/include/clang/Lex/PPCallbacks.h` | `91182dc08` |
| `clang/include/clang/Lex/Preprocessor.h` | `91182dc08` `ab65051ab` |
| `clang/include/clang/Lex/TransitiveClosureTracker.h` | `91182dc08` |
| `clang/include/clang/Parse/Parser.h` | `91182dc08` |
| `clang/include/clang/Parse/RAIIObjectsForParser.h` | `91182dc08` `ab65051ab` |
| `clang/include/clang/Sema/NameClassificationObserver.h` | `91182dc08` |
| `clang/include/clang/Sema/Sema.h` | `91182dc08` |
| `clang/lib/ASTCache/ASTCacheDependencyTracker.cpp` | `91182dc08` |
| `clang/lib/ASTCache/ASTCacheProducer.cpp` | `91182dc08` `a5a384e19` `ea53c042d` `f976d8fd4` `95232c6b6` |
| `clang/lib/ASTCache/CMakeLists.txt` | `91182dc08` `ab65051ab` |
| `clang/lib/ASTCache/CacheClient.cpp` | `91182dc08` |
| `clang/lib/ASTCache/CacheDaemon.cpp` | `91182dc08` |
| `clang/lib/ASTCache/CacheProtocol.cpp` | `91182dc08` |
| `clang/lib/ASTCache/DECISIONS.md` | `91182dc08` `a5a384e19` `ea53c042d` `f976d8fd4` `95232c6b6` `ab65051ab` |
| `clang/lib/ASTCache/DependencySerialization.cpp` | `91182dc08` |
| `clang/lib/ASTCache/FORMALIZATION.md` | `26a28d8ea` |
| `clang/lib/ASTCache/HeaderCacheServer.cpp` | `ab65051ab` |
| `clang/lib/ASTCache/MacroTokens.cpp` | `91182dc08` |
| `clang/lib/ASTCache/PragmaStateCheck.cpp` | `91182dc08` |
| `clang/lib/ASTCache/README.md` | `91182dc08` `a5a384e19` `ea53c042d` |
| `clang/lib/ASTCache/SharedRegion.cpp` | `91182dc08` |
| `clang/lib/ASTCache/WALKTHROUGH.md` | `7032a4b76` |
| `clang/lib/CMakeLists.txt` | `91182dc08` |
| `clang/lib/Frontend/CompilerInstance.cpp` | `ab65051ab` |
| `clang/lib/Lex/CMakeLists.txt` | `91182dc08` |
| `clang/lib/Lex/PPDirectives.cpp` | `91182dc08` `ab65051ab` |
| `clang/lib/Lex/PPExpressions.cpp` | `91182dc08` |
| `clang/lib/Lex/Preprocessor.cpp` | `91182dc08` `ab65051ab` |
| `clang/lib/Lex/TransitiveClosureTracker.cpp` | `91182dc08` |
| `clang/lib/Parse/ParseDecl.cpp` | `91182dc08` `ab65051ab` |
| `clang/lib/Parse/Parser.cpp` | `91182dc08` |
| `clang/lib/Sema/SemaDecl.cpp` | `91182dc08` |
| `clang/lib/Sema/SemaTemplate.cpp` | `91182dc08` |
| `clang/tools/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-daemon/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-daemon/ClangHeaderCacheDaemon.cpp` | `91182dc08` |
| `clang/tools/clang-header-cache-populate/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-populate/ClangHeaderCachePopulate.cpp` | `91182dc08` `a5a384e19` `ea53c042d` |
| `clang/tools/driver/CMakeLists.txt` | `ab65051ab` |
| `clang/tools/driver/cc1_main.cpp` | `ab65051ab` |
| `clang/unittests/ASTCache/ASTCacheDependencyTrackerTest.cpp` | `91182dc08` |
| `clang/unittests/ASTCache/CMakeLists.txt` | `91182dc08` `a5a384e19` `f976d8fd4` `ab65051ab` |
| `clang/unittests/ASTCache/CacheProtocolTest.cpp` | `91182dc08` |
| `clang/unittests/ASTCache/DependencySerializationTest.cpp` | `91182dc08` |
| `clang/unittests/ASTCache/EndToEndTest.cpp` | `91182dc08` |
| `clang/unittests/ASTCache/HitTest.cpp` | `ab65051ab` |
| `clang/unittests/ASTCache/LoadTest.cpp` | `f976d8fd4` `95232c6b6` |
| `clang/unittests/ASTCache/ProducerTest.cpp` | `a5a384e19` `ea53c042d` `f976d8fd4` |
| `clang/unittests/ASTCache/SymbolDependencyTest.cpp` | `91182dc08` |
| `clang/unittests/CMakeLists.txt` | `91182dc08` |
