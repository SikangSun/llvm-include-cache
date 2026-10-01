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
the cache at an `#include` — exists as of `ab65051ab` (§6.6). As of
`1291faa49` the record covers every identifier the header read, not only its
macros (§3.5). As of `2a4866313` a hit leaves the compile as reading the
header would have (§6.7), a compile asks only about headers slow enough to be
worth a lookup (§6.8), and a header included from inside another header is
served too (§6.9). What remains is filling the cache during a build, and
saved copies built after different `.c` files that conflict when loaded
together (§8).

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
- Timing every header and reporting the times to the daemon, which keeps the
  list of headers worth asking about (§6.8, `2a4866313`).
- With `CLANG_HEADER_CACHE_JOBS` set, recording the job that builds each
  header's saved copy (§6.9, `2a4866313`).
- 115 unit tests.

**Changed by the hit path (`ab65051ab`):** `Parser::isAtDeclarationBoundary()`
and `CacheClient` now have a real caller, and `clang/lib/Lex/PPDirectives.cpp`
asks the hook at the top of `case Enter:` (the original marker comment, lower
down, is untouched).

**Changed by syntactic dependencies (`1291faa49`):** the lexers report each
identifier at the expansion decision to an `ExpansionCandidateObserver` when
one is registered (§3.5). None is registered in an ordinary compile.

**Changed by `2a4866313`:** the hit path serves an `#include` written in any
file, not only in the `.c` file; the dependency tracker no longer records a
definedness test of a macro the header set itself; `serializeMacroTokens`
reads literal text from the source.

**Does not exist at all:** running recorded jobs automatically during a build,
the `__COUNTER__` dependency, and exporting a header's PCM from the real
compile instead of a separate one (§8).

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
  `isTemplateName` and `isDeductionGuideName`. **C++ only, not needed for the
  C scope:** C never calls either, and the tracker poisons the frame if one
  fires (DECISIONS 1.11). Of the four queries, C needs `getTypeName` and
  `ClassifyName`.

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

### 3.5 Every identifier considered for expansion (`1291faa49`)

- **`clang/include/clang/Lex/ExpansionCandidateObserver.h` (50 lines, new)** —
  one method, `expansionCandidate(const Token &)`, called for each identifier
  at the point where the preprocessor decides whether to expand it.
- **`clang/include/clang/Lex/Preprocessor.h` (+39 lines)** — the observer
  pointer and its set/get/clear; `noteExpansionCandidate`, inline and one
  pointer test; and `refreshIdentifier`, which brings a name up to date after
  a module load before anyone asks whether it is a macro.
- **`clang/lib/Lex/Preprocessor.cpp` (+11 lines)** — the out-of-line half,
  which skips identifiers read with expansion off: a macro's own name in
  `#define`, the operand of `#ifdef` or `defined()`.
- **`clang/lib/Lex/Lexer.cpp` (+2 lines)** — text read from a file
  (`LexIdentifierContinue`) and clang-scan-deps' minimized sources
  (`LexDependencyDirectiveToken`).
- **`clang/lib/Lex/TokenLexer.cpp` (+5 −2 lines)** — tokens coming out of a
  macro's body, a `##` result included.
- **`clang/lib/Lex/PPMacroExpansion.cpp` (+6 lines)** — a macro whose whole
  body is one token, which `HandleMacroExpandedIdentifier` returns without a
  token lexer.

Why it was needed: an earlier `#define` can turn any identifier in a header
into something else — zmalloc.c's `#define malloc tc_malloc`, qemu's
`#define inline __attribute__((gnu_inline))` — so every identifier the header
reads is a dependency, macro or not. A name that is not a macro passes the
expansion check without calling anything, so no existing callback saw it
(DECISIONS 1.12).

The fourth site was found by a failing test: `#define ALLOC grab_impl` is the
commonest macro shape there is, and `grab_impl` went unrecorded. It is a
pointer on `Preprocessor`, not a `PPCallbacks` method, because an ordinary
compile always has callbacks attached and a new one would put a chain of
virtual calls on every identifier. Measured with no observer set: redis
`object.c` 3,070 → 3,084 ms, `server.c` 4,943 → 4,919 ms, within noise.

---

## 4. The data model, and collecting it

### 4.1 `clang/include/clang/ASTCache/DependencyRecord.h` (172 lines)

What a parsed header can depend on, in five pieces:

| Struct | Records |
|---|---|
| `ConditionalDependency` | A macro whose *definedness* was tested, and the answer. |
| `SyntacticDependency` | An identifier the preprocessor considered for expansion inside the header, and what it had to be: *not a macro*, or a macro with exactly this replacement list (DECISIONS 1.12). |
| `SymbolDependency` | An identifier a grammar-ambiguity query asked about, which query, and the answer — including the resolved type's canonical spelling. |
| `InclusionDependency` | An `#include` that did nothing because the file was already included (`#pragma once`, `#import`). |
| `PragmaSnapshot` | Pragma state, twice: `Entry`, the state the header was compiled under, and `Exit`, the state the header leaves behind. |
| `MacroEffect` | A macro the header defined or undefined, and whether it was left defined. Not a dependency but an effect: a hit replays it (§6.7, `2a4866313`). |

`HeaderDependencySet` bundles all of the above; `HeaderRecord` pairs one set
with the header's path.

Design notes worth knowing: `SymbolDependency::Classification` mirrors
`clang::NameClassificationKind`'s integer values rather than including
`Sema.h`, so the record stays cheap to include. And a macro is a dependency
**only if its definition lies outside the header's own closure** — a macro
defined and used inside the same header can never vary. As of `2a4866313`
the same holds for a definedness test (§4.2).

### 4.2 `clang/include/clang/ASTCache/ASTCacheDependencyTracker.h` (251) and `clang/lib/ASTCache/ASTCacheDependencyTracker.cpp` (369)

A `PPCallbacks` that also implements `ExpansionCandidateObserver` and
`NameClassificationObserver`, so one object collects the preprocessor's, the
lexer's and Sema's facts.

The important design point is that it keeps a **stack of frames**, one per
file entered, not one set per translation unit. Headers nest, and the same
macro expansion has a different answer at each level: a macro defined in `a.h`
and used in the `b.h` that `a.h` includes is a real dependency of `b.h` and
not one of `a.h`, whose own text fixes it. So each sighting of a name is
asked the provenance question once per open frame.

**First sighting decides.** A frame records a name the first time it sees it
and never again. A header that uses `BUF` and then defines it depended on
`BUF` not being a macro at that use, so it records NotAMacro; one that defines
`BUF` first does not depend on the includer for it, and records nothing.

An earlier flat version got this wrong twice: it asked the question once per
translation unit, and it treated the main `.c` file as internal to everything,
which silently dropped real dependencies.

**Conditionals face outward (`2a4866313`).** redis's `fmacros.h` defines
`_BSD_SOURCE` and then includes `<features.h>`, which tests it. Recorded
without a filter, `fmacros.h`'s record said "`_BSD_SOURCE` must be defined",
which no compile satisfies before including `fmacros.h`, so its copy never
matched. A definedness test is now recorded for a frame only if the state it
saw came from outside that frame's closure: from the active `#define`, or,
for an undefined name, from the `#undef` that removed it (`stateOrigin`). A
name never defined, a `-D`, and a builtin count as outside. For `features.h`
the definition still comes from outside, so it remains a dependency there. The
same rule skips an include-guard or `#pragma once` skip of a file the frame
itself read earlier. On `anet.c` alone, "recorded defined, live undefined"
rejections fell from 42 to 2 (DECISIONS 1.5).

**Poisoning:** a frame that meets something the recorder cannot describe —
currently, any C++-only query, or a symbol query with no usable location — is
dropped entirely and emits no record. A partial record would carry *fewer*
constraints than the compile that produced it, so it would match compiles it
must not. Missing entirely means a miss.

### 4.3 `clang/include/clang/ASTCache/MacroTokens.h` (51) and `.cpp` (34)

One function that turns a macro's replacement list into a string. It is its
own file because two sides must produce byte-identical output: the tracker
when recording, and the client when checking a stored record against a live
macro. If those two ever drifted, every ExpandsTo dependency would compare
unequal and nothing would ever hit — a failure that looks exactly like the
cache simply not helping.

**Literal text comes from the source (`2a4866313`).** A token keeps a pointer
to its literal's text only while the text it was lexed from is around. A macro
loaded from a saved copy has none, so `#define __WORDSIZE 64` serialized as
"a number" and compared equal to `#define __WORDSIZE 32`. The text is now read
with `Lexer::getSpelling` from where the token was written; a definition
whose text cannot be read is neither recorded (the frame is poisoned) nor
found to match.

### 4.4 `clang/include/clang/ASTCache/PragmaStateCheck.h` (87) and `.cpp` (111)

`#pragma pack(1)` before an `#include` changes the layout of every record in
the header, and no macro is tested or expanded, so the other categories record
nothing. This file captures the pragma stacks (pack, float control) into a
`PragmaSnapshot`, and reports any pragma state a record cannot express, in
which case the header is declined.

Serialization does not solve this on its own:
`ASTWriter::WritePackPragmaOptions` returns early while a module is being
written, so the PCM carries no pragma state in either direction.

### 4.5 `clang/include/clang/ASTCache/ByteStream.h` (133), `DependencySerialization.h` (77) and `.cpp` (193)

A flat byte encoding for `HeaderDependencySet`, the little-endian primitives
under it, and the NUL-separated list a record's NotAMacro names travel as
(§6.3). The macro effects travel the same way, each entry `+NAME` (left
defined) or `-NAME` (left undefined); `decodeMacroEffects` rejects any other
shape. Encoding version 4.

Two properties matter. Strings are length-prefixed, not NUL-terminated,
because a serialized replacement list contains NULs by construction — they
are its token separator. And the reader **stays failed**: once any read runs
past the end, every later read returns zero and `ok()` is false, so a caller
decodes a whole structure and checks once. A decoder that returned what it had
managed to read would produce a partial record, which is the unsafe direction
(§4.2).

---

## 5. Building a PCM for one header

### 5.1 `clang/include/clang/ASTCache/ASTCacheProducer.h` (243) and `clang/lib/ASTCache/ASTCacheProducer.cpp` (939)

This is the largest piece, and the one that changed most often.

**Why a separate compile at all.** The parsed form of a header cannot be
sliced out of a real compile, because of include guards: in a translation unit
that reads `bar.h` before `foo.h`, `foo.h`'s own `#include "bar.h"` does
nothing, so `bar.h`'s declarations are outside the slice while `foo.h`'s
declarations point at them. Reverse the order and the slice changes. A
separate compile of `foo.h` fires that `#include` for real, every time.

This was never approved, and on 2026-09-30 the user made exporting from the
real compile the default (DECISIONS 2.1). The separate compile is what exists
until that is designed and built.

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

**Headers included from headers (`2a4866313`).** Including all of `a.h` to
reach the `b.h` inside it would compile `b.h` in the hidden half, so the
producer declines a header its own context reaches. Instead, the context is a
list of *lines*: an `#include`, a `#define` or an `#undef` (`ContextEntry`).
For `b.h` it holds the `.c` file's lines before `#include "a.h"`, then
`a.h`'s lines before `#include "b.h"`, never `a.h` itself. The `#define`s are
what make glibc work: `bits/stdio_lim.h` begins `#ifndef _STDIO_H` /
`#error "Never include <bits/stdio_lim.h> directly"`, and `_STDIO_H` comes
from `stdio.h`'s own text. What `a.h` declared above the `#include` is
missing, so a `b.h` that needs it fails to compile here and is declined. The
`.c` file's own `#define`s come along the same way, which fixes the
`_GNU_SOURCE` miss of DECISIONS §4. Before this, only headers a `.c` file
includes directly were populated.

**The effects (`2a4866313`).** While the target part is open, `TargetWatcher`
collects every macro it defines or undefines; when the target wrapper ends, it
reads each one's final state into `MacroEffects` (§6.7).

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
  `__DATE__`, `__TIME__` or `__TIMESTAMP__`, directly or through a macro such
  as glib's `G_LIKELY`, whose values belong to the compile that includes it.
  Decided 2026-09-30, not built: `__COUNTER__` becomes a dependency on its
  starting value plus an effect that advances it (DECISIONS 2.7);
- the recorder was poisoned (§4.2).

**Job files.** A plain-text format carrying a list of requests, so a compile
can hand its work to another process. Version 3 (`2a4866313`) adds `define`
and `undef` lines, replayed in order with the `context` lines; version 2 files
still read. Values that cannot be represented, such as a flag containing a
newline, are rejected rather than silently split.

**One bug worth remembering:** the producer used to read the tracker and the
observer *after* `ExecuteAction` returned, when both had been destroyed. The
results are now copied out during `HandleTranslationUnit`.

---

## 6. Storing and serving PCMs

### 6.1 `clang/include/clang/ASTCache/SharedRegion.h` (114) and `.cpp` (196)

An append-only region, mapped by the daemon and by every compile. Append-only
is the concurrency argument: a written entry is never modified or moved, so
readers need no coordination, and the socket reply that hands out an offset is
itself the synchronisation point — a client only learns of an entry after it
was fully written. The size is fixed generously at creation because growing a
mapping can move it, which would invalidate every offset already handed out.
The backing file is sparse, so reserved space costs nothing.

As of `2a4866313` the region's header also holds the offset of the current
ask-list (§6.8), published with a release store and read with an acquire
load, so a reader sees either the old list or the whole new one. The header's
random instance id, unused until then, is now what a client checks at connect
(§6.4).

### 6.2 `clang/include/clang/ASTCache/CacheProtocol.h` (208) and `.cpp` (309)

Two request types: `LOOKUP(filename)`, answered with every entry stored under
that name, each carrying its dependency record and where its bytes sit in the
region; and `PUSH(filename, record, data)`, answered with an acknowledgment.
A lookup reply carries each record without its NotAMacro names, and says
where in the region those names sit (§6.3).

Added in `2a4866313`: a lookup reply also says where each entry's macro
effects sit and gives its module name; a push carries the module name;
`TIMING`, which reports how long the compile took to read each header and is
acknowledged; and `HELLO`, answered with the region's path and instance id.

The daemon never interprets a record. Deciding which entry fits needs the
compile's live macro table and Sema, which the daemon does not have and could
not be sent cheaply.

### 6.3 `clang/include/clang/ASTCache/CacheDaemon.h` (160) and `.cpp` (262), plus `clang/tools/clang-header-cache-daemon/` (113 + 13)

A filename → entries table over the region. No state survives a restart, by
choice: this is a proof of concept, and the daemon is not restarted during an
evaluation. Message handling is a pure function from message to reply, so it
is tested without any socket. One thread per connection, blocking reads, no
timeout — after a compile finishes its lookups it spends a long time in code
generation without touching the socket, and a parked thread costs nothing.

On a push the daemon moves the record's NotAMacro names — several hundred for
a system header — into the region, so they stay out of every lookup reply,
and keeps the entry only if both the names and the PCM were written.

Changed in `2a4866313`. The macro effects go into the region beside the
names. A push is a duplicate only if both its record *and* its module name
match a stored one: the compiler flags are not in the record, so a copy built
with `-fshort-enums` used to shut out every later copy of the same header
built without it. A timing report keeps each header's first reported time
and puts headers at or above `-ask-cutoff-us` on the ask-list (§6.8);
`-list-stored` also lists every header the moment a copy is stored. The
region path is made absolute, so a client elsewhere can attach to it.

### 6.4 `clang/include/clang/ASTCache/CacheClient.h` (160) and `.cpp` (315)

The judgement: given a stored record and the live compile, does the entry
apply? Checks run cheapest first and stop at the first mismatch — conditional,
then the ExpandsTo syntactic dependencies, then inclusion, then the NotAMacro
names, then symbol, then pragma entry state. A header with a hundred
conditional dependencies usually fails on one of them, and the symbol checks
are the only ones costing a name lookup. With no `Sema` available, everything
is rejected.

Every name is refreshed (`Preprocessor::refreshIdentifier`) before it is
asked about. A hit earlier in the compile loads a module, which can define a
name the compile had already seen, and until the identifier is updated its
macro bit still says it is not a macro. The conditional check had this bug
before the syntactic one existed; `1291faa49` fixes both.

The hit path (§6.6) calls it.

Changed in `2a4866313`:

- **Connecting** sends `HELLO` and attaches to the region file the daemon
  names, refusing one whose instance id differs. Before, a client attached
  to the default path whatever its daemon wrote, and read names and saved
  copies at offsets meant for another file.
- **A candidate that passes** is offered to the caller with its module name,
  and the caller may refuse it — the module reader refuses a copy built with
  other options — in which case the next candidate is tried.
- **A rejection names its reason** when asked (`CLANG_HEADER_CACHE_STATS=2`):
  "`_BSD_SOURCE` was defined, is not", "`X` expands differently", and so on.
- **`isListed`** reads the ask-list from the region with no socket traffic.

### 6.5 `clang/tools/clang-header-cache-populate/` (262 + 13)

The command-line front end to the producer: headers as arguments, the build's
flags with `-Xcc`, the context with `-context` (a path, or a spelling such as
`'<stdint.h>'`), or a whole job file with `-job`. `-dry-run` builds everything
and reports without needing a daemon: how many headers were saved, the mean
size of a PCM and of a record, and a tally of why the rest were declined.

As of `2a4866313`, `-Xcc` may be combined with `-job`: a compile records its
jobs without the driver's flags, which cc1 never sees, and the build passes
them here.

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
  `2a4866313` removed the main-file gate and added the ask-list check
  (§6.8, §6.9).
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
run — followed, as of `1291faa49`, by one line per rejection reason.

**First hit on a real benchmark file.** redis `anet.c`, compiled from `src/`
with the real `clang`, after producing `anet.h` in the context of the 16
includes `anet.c` has before it: `header cache: 1 hits / 18 lookups`. The
daemon agrees (`lookups 18 (1 with entries)`), and the object file's
disassembly is identical to an uncached compile's, all 1,517 lines. 19 direct
includes gave 18 lookups because `<string.h>` had already been read through an
earlier header, so Clang skipped it before the hook is asked.

**With syntactic dependencies (`1291faa49`).** Each `.c` file's direct
includes built in the context of the ones before them: `anet.c` served 6 of
18, `sds.c` 1 of 9, `object.c` 3 of 6, with identical assembly each time and
no candidate rejected for a syntactic reason. Every rejection was a
conditional recorded defined and live undefined: macros glibc sets for itself
(`__need_size_t`), which conditionals record without a provenance filter
(DECISIONS 1.5), and `_BSD_SOURCE` from `fmacros.h`, whose `#define`s do not
take effect after a hit because macro replay is not built yet.

### 6.7 A hit leaves the compile as the header would (`2a4866313`)

Loading a module makes its macros *visible*, which is not what a textual
`#include` does. Three differences showed up, each a different program:

- **An `#undef` in the header did nothing.** A module's `#undef` of a macro
  the includer defined does not hide the includer's definition.
- **Of two loaded modules defining one name, neither wins**; the name is
  ambiguous. Included as text, the later definition replaces the earlier.
- **A module's macro does not override a local `#define`.**

`replayMacroEffects` fixes all three by appending, at the `#include`, a local
directive for each macro the header touched: an `#undef`, or a `#define`
whose definition is the loaded module's own `MacroInfo`. A local directive
overrides visible module macros, which is how text inclusion behaves. Include
state needed nothing new: the module file carries `#pragma once` and
included marks, and replayed guard macros skip guarded headers (four tests).

Also on a hit:

- **`__COUNTER__` is put back.** Loading a module file sets the counter to
  the value its producer's compile ended with (`PCHValidator::ReadCounter`),
  which counts the context's uses. A saved part uses none (the producer
  declines such headers), so the counter after the hit is the counter before.
- **A copy already loaded is reused.** `stddef.h` has no guard and is
  entered again each time; when the same copy fits twice, loading a second
  module of the same name is a fatal error.
- **A refused copy is passed over.** `ReadAST` with
  `ARR_ConfigurationMismatch` returns quietly for a copy built with other
  options; `loadModuleFile` would report it as an error, since
  `-Wmodule-file-config-mismatch` is one by default.

### 6.8 Asking only about slow headers (`2a4866313`, DECISIONS 2.14)

A lookup costs a socket round trip, and loading a PCM costs about 226 µs on
release clang 18. A header that parses faster than that is cheaper to parse.
So every compile times each header, from entering it to leaving it, headers
it includes counted in, first entry only, and reports the times when its main
file ends. The daemon keeps each header's first reported time and lists those
at or above the cutoff. The list sits in the shared region; a compile checks
it before touching the socket and records "not on the ask-list" otherwise.

On a debug build every header takes longer than 226 µs, so everything is
listed; the cutoff only filters on a release build.

### 6.9 Headers included from headers (`2a4866313`)

Three pieces:

1. **The hit path asks at every `#include`**, in a `.c` file or in a header.
   The declaration-boundary and ask-list checks still apply.
2. **The producer builds a nested header after its chain's lines** (§5.1).
3. **A compile records the jobs** (`JobRecorder` in `HeaderCacheServer.cpp`)
   when `CLANG_HEADER_CACHE_JOBS` names a directory. It keeps one frame per
   open file with that file's own `#include`, `#define` and `#undef` lines so
   far. At each `#include` at a declaration boundary it builds the job: the
   header, and every open frame's lines in order. The job is kept only if the
   file is then entered, not if a guard skipped it or the cache served it.
   An `#include` that entered its file joins the includer's lines when that
   file ends, so a later sibling's context replays it. A quoted `#include`
   found beside the file that wrote it, and every `#include_next`, are
   replayed by path, because from the producer's wrapper the same spelling
   would search elsewhere. Lines inside the predefines buffer and
   `-include` files are not recorded; they come back through the flags. One
   new file per compile, read by `clang-header-cache-populate -job`.

**On redis, debug clang** (each `.c` file compiled once to record, its jobs
populated, then compiled again with the cache and compared with a plain
compile): `anet.c` on its own daemon, 156 of 217 jobs saved; 18 hits of 18
lookups, no candidate rejected, assembly identical. Every header `anet.c`
includes directly was served, so nothing inside them was asked about.

**Saved copies built after different `.c` files conflict.** A saved copy's
context sits in a hidden submodule: hidden from name lookup, but not from
declaration merging. With several `.c` files sharing a daemon, two copies
held `sys/select.h`'s `fd_set` built under different feature macros (member
`fds_bits` in one, `__fds_bits` in the other), and the compile stopped:
"`fd_set::__fds_bits` from module A.context is not present in definition of
`fd_set` in module B.context". `stdio.h`'s `fscanf` likewise came with two
different asm labels. Each such failure stops the compile with an error; none
produced different code. This is the declaration-merging cost that ended
Google's pre-parsed headers, and the reason to export from the real compile
instead (DECISIONS 2.1, §3).

---

## 7. Tests — 115, all passing

`clang/unittests/ASTCache/` (plus `CMakeLists.txt`, 24 lines, and one line in
`clang/unittests/CMakeLists.txt`).

| File | Tests | What it pins down |
|---|---|---|
| `ASTCacheDependencyTrackerTest.cpp` (454) | 24 | Provenance: an internally defined macro is not a dependency; one from the includer is; a sub-header's macro is internal to its parent; a conditional is recorded at every open level; `#if UNDEFINED > 5` is recorded; a guard-suppressed `#include` is recorded. Syntactic: a plain name, a keyword, a `##` result and a name from an outside macro's body are recorded; the first sighting decides; names read with expansion off are not recorded; a child records a name its parent saw first. Outward conditionals: a flag a header sets for itself is not its dependency; only the first test of a name decides; an `#undef` inside the header pins the name; a `#pragma once` file read twice inside a header is no dependency. |
| `SymbolDependencyTest.cpp` (180) | 4 | The `T * p;` ambiguity: a typedef from the includer is a dependency; one the header declares itself is not; the resolved type's identity is recorded, not just "it was a type"; a typedef from a sub-header is internal to the parent. |
| `DependencySerializationTest.cpp` (235) | 13 | An empty set and a full set round-trip; embedded NULs survive; an unknown syntactic state and a damaged name list are rejected; NotAMacro names and macro effects move into their lists, and a damaged effect list is rejected; truncation, a version mismatch, trailing bytes and an out-of-range classification are all rejected rather than partially decoded; a corrupt count does not size an allocation. |
| `CacheProtocolTest.cpp` (405) | 22 | Message framing (length prefix, round trip, wrong kind, truncation, where the NotAMacro names and effects are, the module name, timing reports, the hello reply, a socket path too long for an address) and daemon behaviour: the names and effects are kept in the region, a miss is clean, every variant comes back, identical pushes are discarded but the same record built with other options is kept, distinct headers do not collide, stored bytes come back exactly, an out-of-range offset reads as nothing, garbage closes the connection; a slow header goes on the ask-list, a header's first time is the one kept, stored headers are listed when asked to. |
| `EndToEndTest.cpp` (350) | 7 | Push through a real socket, then reject the entry on each kind of mismatch in turn, and choose correctly between variants. |
| `ProducerTest.cpp` (434) | 14 | Context makes an otherwise unbuildable header buildable; a context macro becomes a conditional dependency; the macros the header touches are its effects; pragma entry state is the header's, not the compile's; a header reached through its own context is declined; a header included from a header builds after its includer's lines, `#define` and `#undef` included; module names are unique and stable; `#include_next` works only when the context is replayed as written; a replay that opens a different file is declined; a header reading `__COUNTER__` is declined; job files round-trip, version 2 still reads, and macro lines that cannot be represented are rejected. |
| `LoadTest.cpp` (216) | 1 | A PCM is loaded from bytes in memory, under a name no file has, partway through a compile that already parsed the context header itself — and its declarations then work with that compile's own types. |
| `HitTest.cpp` (974) | 30 | A stored header is served without the file being opened; an `#include` right after a function definition is served while one inside a body is not; a header the compile names relatively is still found; an entry whose record does not match is not used. Not served when the includer makes a name the header read as plain text into a macro — a plain name, a keyword, a pasted name, a name from an outside macro's body; served when the header defines the name before using it; a conditional and a syntactic check both see a macro an earlier hit defined. Effects: a redefinition and an `#undef` in the header take effect; of two served headers the later definition wins; a served header's flag is seen; `#pragma once` and guarded headers, served or inside a served one, are skipped when included again; the counter is where the header found it; a copy built with other options is passed over. Ask-list: a stored header is served only once a compile has timed it; one faster than the cutoff is never asked about. Nested: a header included from a header is served; a recorded job builds a copy the next compile serves; a recorded include is replayed the way it was found. A client attaches only to its daemon's region; an unguarded header served twice loads its copy once; a header that sets a flag for its own include is served; a macro from a served copy compares by its text. |

---

## 8. What does not exist yet

1. **Exporting from the real compile.** Decided 2026-09-30 as the default
   (DECISIONS 2.1); not designed yet. It is also the way out of item 2.
2. **Copies that conflict when loaded together** (§6.9). Blocks a benchmark
   run over more than one `.c` file.
3. **Filling the cache during a build.** A compile records its jobs (§6.9);
   nothing runs them automatically, and a build needs one compile to record,
   a populate step, and a second compile to hit.
4. **The `__COUNTER__` dependency.** Decided: the counter's starting value as
   a dependency and its advance as an effect (DECISIONS 2.7). Relocating the
   numbers on load is deferred until the first benchmark round.
5. **Folding by parse time** (DECISIONS 2.15). The 226 µs cutoff is decided;
   the mechanism is not approved.
6. **A release build** for timings. On the debug build every header is
   listed (§6.8).
7. **The token tier's server half.** The token tier can record and ship;
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
- **Syntactic names were first collected at the wrong hook.**
  `Preprocessor::LookUpIdentifierInfo` misses a name that comes out of an
  outside macro's body: with `#define ALLOC grab_impl` outside the header,
  `grab_impl` was looked up when that `#define` was read. Replaced by the
  expansion-decision sites (§3.5) before `1291faa49`.
- **A stale macro bit let the conditional check pass wrongly** after an
  earlier hit (§6.4); fixed in `1291faa49`.
- **"No benchmark header uses `__COUNTER__`" was wrong.** The search looked
  for the word in header text, and found it only inside `#define`s. A header
  runs the counter whenever it uses such a macro: glib's `G_LIKELY`, the
  kernel's `READ_ONCE`. Preprocessing every `.c` file of qemu and the Linux
  kernel showed 21% and 35% of their direct includes declined (DECISIONS
  2.7). Found 2026-09-29, the day the claim was made.
- **A module load reset `__COUNTER__`** to the value the producer's compile
  ended with (§6.7); fixed in `2a4866313`.
- **The daemon dropped a copy built with other options as a duplicate**
  (§6.3); fixed in `2a4866313`.
- **A client mapped the wrong region** (§6.4). The first nested end-to-end
  run made 140 lookups and no hits, and 27 rejections read "the not-a-macro
  name list is damaged": the test daemon wrote one region file and the
  compiler read the default one. Fixed in `2a4866313` with the hello check.
- **Literal text compared as "a number"** for a macro from a saved copy
  (§4.3); fixed in `2a4866313`.
- **The redis misses blamed on missing macro replay** were conditionals the
  header set for itself; replay did not change them, the outward filter did
  (§4.2).

---

## 10. Where to look next

- `DECISIONS.md` — why each choice was made, and what was rejected.
- `README.md` — how to run the tools.
- This file — what exists.

---

## 11. Commit map

Every commit since upstream, oldest first, all on
`github.com/SikangSun/llvm-include-cache`, branch `header-cache`. The commit
that last refreshed this file is not listed in its own table.

| Commit | What it did |
|---|---|
| [`91182dc08`](https://github.com/SikangSun/llvm-include-cache/commit/91182dc08708b524cf29cae785aa38ce8fa0414a) | [clang] Per-header compilation cache: observation layer and AST tier |
| [`a5a384e19`](https://github.com/SikangSun/llvm-include-cache/commit/a5a384e19b83b6dbbf5ccbd0058ee4a3dbbe72e8) | [clang] Header cache: producer replays context into a hidden submodule |
| [`ea53c042d`](https://github.com/SikangSun/llvm-include-cache/commit/ea53c042db80c37d0f716e8dd7ab2def6ab5d54a) | [clang] Header cache: replay each include the way the compile found its file |
| [`f976d8fd4`](https://github.com/SikangSun/llvm-include-cache/commit/f976d8fd40648c2967f60567e2f0f31fa445198b) | [clang] Header cache: decline TU-varying builtins; test loading from memory |
| [`95232c6b6`](https://github.com/SikangSun/llvm-include-cache/commit/95232c6b69c0525d65e5d03a90208291491a2de0) | [clang] Header cache: store the producer's generated files in the saved copy |
| [`7032a4b76`](https://github.com/SikangSun/llvm-include-cache/commit/7032a4b76765506386f053df56aa4ba271b22622) | [clang] Header cache: a file-by-file walkthrough of what was built |
| [`26a28d8ea`](https://github.com/SikangSun/llvm-include-cache/commit/26a28d8ea4e47d907adf320ab9935b04803d9bf8) | [clang] Header cache: shelve the read-set argument for the record |
| [`ab65051ab`](https://github.com/SikangSun/llvm-include-cache/commit/ab65051ab35c09641ab79a65feef251a707cc761) | [clang] Header cache: serve an #include from the cache (the hit path) |
| [`6996c31c7`](https://github.com/SikangSun/llvm-include-cache/commit/6996c31c77bf3f6458e35a37503395836d23f79d) | [clang] Header cache: walkthrough covers the hit path; add a commit map |
| [`1291faa49`](https://github.com/SikangSun/llvm-include-cache/commit/1291faa49476645f91a02ae8a8e4cbd87f89b62d) | [clang] Header cache: syntactic dependencies |
| [`880757f46`](https://github.com/SikangSun/llvm-include-cache/commit/880757f467035bc8e71ffa7eb7d208bfb5a2cc1e) | [clang] Header cache: walkthrough covers syntactic dependencies |
| [`2a4866313`](https://github.com/SikangSun/llvm-include-cache/commit/2a4866313949a5a150cff00c4519854494e6b5e6) | [clang] Header cache: replay a hit's effects, ask-list, serve nested headers |

Which commits changed each file, oldest first:

| File | Commits |
|---|---|
| `clang/include/clang/ASTCache/ASTCacheDependencyTracker.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/ASTCacheProducer.h` | `91182dc08` `a5a384e19` `ea53c042d` `2a4866313` |
| `clang/include/clang/ASTCache/ByteStream.h` | `91182dc08` `1291faa49` |
| `clang/include/clang/ASTCache/CacheClient.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/CacheDaemon.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/CacheProtocol.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/DependencyRecord.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/DependencySerialization.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/HeaderCacheServer.h` | `ab65051ab` `2a4866313` |
| `clang/include/clang/ASTCache/MacroTokens.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/PragmaStateCheck.h` | `91182dc08` |
| `clang/include/clang/ASTCache/SharedRegion.h` | `91182dc08` `2a4866313` |
| `clang/include/clang/Frontend/CompilerInstance.h` | `ab65051ab` |
| `clang/include/clang/Lex/DeclBoundaryObserver.h` | `91182dc08` |
| `clang/include/clang/Lex/ExpansionCandidateObserver.h` | `1291faa49` |
| `clang/include/clang/Lex/HeaderCacheHook.h` | `ab65051ab` |
| `clang/include/clang/Lex/PPCallbacks.h` | `91182dc08` |
| `clang/include/clang/Lex/Preprocessor.h` | `91182dc08` `ab65051ab` `1291faa49` |
| `clang/include/clang/Lex/TransitiveClosureTracker.h` | `91182dc08` |
| `clang/include/clang/Parse/Parser.h` | `91182dc08` |
| `clang/include/clang/Parse/RAIIObjectsForParser.h` | `91182dc08` `ab65051ab` |
| `clang/include/clang/Sema/NameClassificationObserver.h` | `91182dc08` |
| `clang/include/clang/Sema/Sema.h` | `91182dc08` |
| `clang/lib/ASTCache/ASTCacheDependencyTracker.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/ASTCacheProducer.cpp` | `91182dc08` `a5a384e19` `ea53c042d` `f976d8fd4` `95232c6b6` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/CMakeLists.txt` | `91182dc08` `ab65051ab` |
| `clang/lib/ASTCache/CacheClient.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/CacheDaemon.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/CacheProtocol.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/DECISIONS.md` | `91182dc08` `a5a384e19` `ea53c042d` `f976d8fd4` `95232c6b6` `ab65051ab` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/DependencySerialization.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/FORMALIZATION.md` | `26a28d8ea` |
| `clang/lib/ASTCache/HeaderCacheServer.cpp` | `ab65051ab` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/MacroTokens.cpp` | `91182dc08` `2a4866313` |
| `clang/lib/ASTCache/NEXT.md` | `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/PragmaStateCheck.cpp` | `91182dc08` |
| `clang/lib/ASTCache/README.md` | `91182dc08` `a5a384e19` `ea53c042d` `2a4866313` |
| `clang/lib/ASTCache/SharedRegion.cpp` | `91182dc08` `2a4866313` |
| `clang/lib/ASTCache/WALKTHROUGH.md` | `7032a4b76` `6996c31c7` `880757f46` this commit |
| `clang/lib/CMakeLists.txt` | `91182dc08` |
| `clang/lib/Frontend/CompilerInstance.cpp` | `ab65051ab` |
| `clang/lib/Lex/CMakeLists.txt` | `91182dc08` |
| `clang/lib/Lex/Lexer.cpp` | `1291faa49` |
| `clang/lib/Lex/PPDirectives.cpp` | `91182dc08` `ab65051ab` |
| `clang/lib/Lex/PPExpressions.cpp` | `91182dc08` |
| `clang/lib/Lex/PPMacroExpansion.cpp` | `1291faa49` |
| `clang/lib/Lex/Preprocessor.cpp` | `91182dc08` `ab65051ab` `1291faa49` |
| `clang/lib/Lex/TokenLexer.cpp` | `1291faa49` |
| `clang/lib/Lex/TransitiveClosureTracker.cpp` | `91182dc08` |
| `clang/lib/Parse/ParseDecl.cpp` | `91182dc08` `ab65051ab` |
| `clang/lib/Parse/Parser.cpp` | `91182dc08` |
| `clang/lib/Sema/SemaDecl.cpp` | `91182dc08` |
| `clang/lib/Sema/SemaTemplate.cpp` | `91182dc08` |
| `clang/tools/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-daemon/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-daemon/ClangHeaderCacheDaemon.cpp` | `91182dc08` `2a4866313` |
| `clang/tools/clang-header-cache-populate/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-populate/ClangHeaderCachePopulate.cpp` | `91182dc08` `a5a384e19` `ea53c042d` `1291faa49` `2a4866313` |
| `clang/tools/driver/CMakeLists.txt` | `ab65051ab` |
| `clang/tools/driver/cc1_main.cpp` | `ab65051ab` |
| `clang/unittests/ASTCache/ASTCacheDependencyTrackerTest.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/unittests/ASTCache/CMakeLists.txt` | `91182dc08` `a5a384e19` `f976d8fd4` `ab65051ab` |
| `clang/unittests/ASTCache/CacheProtocolTest.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/unittests/ASTCache/DependencySerializationTest.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/unittests/ASTCache/EndToEndTest.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/unittests/ASTCache/HitTest.cpp` | `ab65051ab` `1291faa49` `2a4866313` |
| `clang/unittests/ASTCache/LoadTest.cpp` | `f976d8fd4` `95232c6b6` |
| `clang/unittests/ASTCache/ProducerTest.cpp` | `a5a384e19` `ea53c042d` `f976d8fd4` `2a4866313` |
| `clang/unittests/ASTCache/SymbolDependencyTest.cpp` | `91182dc08` `1291faa49` |
| `clang/unittests/CMakeLists.txt` | `91182dc08` |
