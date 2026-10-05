# Header cache — what was built, file by file

A description of everything added for the **AST tier** of the header cache:
what each file is, why it exists, and, for files that already existed in
Clang, what was changed inside them. It describes the code as it is now, at
the commit that last changed this file, and is written to be read start to
finish and checked against the code.

The **token tier** (`clang/lib/CacheObserver`, `clang/include/clang/CacheObserver`)
came earlier and is unchanged by this work. It is not described here.

Scope of the whole design: **C only**.

Everything is against upstream `ca7933e47`. Line counts are from
`git diff --stat ca7933e47..HEAD`. §13 maps every file to the commits that
changed it.

**How new material is marked.** The last code review was at `2a4866313`
(2026-09-30). Text with no mark describes code that already existed then and
has not changed since. Two marks show the rest, each with the commit that
made the change:

- **NEW (`commit`)**: added after `2a4866313`. A whole section can be new; its
  heading then carries the mark.
- **CHANGED (`commit`)**: existed at `2a4866313` and works differently now.
  The text says what it did before.

The commits after `2a4866313`, in order:

| Commit | Date | What it did |
|---|---|---|
| `9b86dcc4e` | 2026-10-02 | Check `__FILE__` and the include path at a hit; the export experiment |
| `9f46203f6` | 2026-10-05 | Export a header's part at a miss: first debugging version |
| `0afcadce2` | 2026-10-05 | A part refers to outside declarations by name |
| `f05e79584` | 2026-10-05 | Print where the cache's time goes |

To read one change in the code: `git show <commit> -- <file>`. Commits that
only changed documents are left out of this table.

---

## Terms

Names used throughout. Most are names this project made up; Clang does not
use them.

- **A compile**: one run of `clang` on one `.c` file.
- **PCM**: Clang's precompiled module file. Every stored header below is a
  PCM. A compile loads one with `ASTReader::ReadAST`.
- **Saved copy**: a PCM the cache holds for one header, such as redis's
  `anet.h`. A compile can load it instead of reading `anet.h`'s text.
- **Part** (NEW, `9b86dcc4e`): a saved copy exported from a real compile: the
  declarations and macros the header's own text produced in that compile,
  and nothing else (§5.2). The older kind of saved copy is built by a
  separate compile, the **producer** (§5.1).
- **Metadata**: what a saved copy depended on when it was made, stored next
  to it (`HeaderDependencySet`, §4.1). A later compile checks the metadata
  before loading the saved copy.
- **Hit** and **miss**: at an `#include`, a hit loads a saved copy instead of
  the file; a miss reads the file as text.
- **Candidate**: one saved copy the cache holds for the header being
  included. A header can have several, made in different contexts; a compile
  checks them one at a time.
- **Daemon**: `clang-header-cache-daemon`, a separate process that holds the
  saved copies and answers compiles over a Unix socket.
- **Shared region**: a file mapped into memory by the daemon and by every
  compile. The saved copies' bytes sit there, so a compile reads them without
  copying them over the socket.
- **Ask-list**: the headers some compile found slow enough to be worth
  saving. A compile asks the daemon only about headers on the ask-list.
- **Cache query**: a compile asking the daemon, over the socket, for the
  candidates of one header.
- **Header search**: Clang finding the file an `#include` names, by trying
  each include directory in order. For redis the directories are `.`,
  `../deps/hiredis`, `../deps/lua/src` and so on from the `-I` flags, then
  `/usr/include`.
- **Region** (of a header): the source locations Clang made while the header
  was open: its own text and every file first entered under it. A part holds
  what the region produced.
- **Outside declaration**: a declaration a part's own declarations refer to
  but that the region did not produce: `anet.h` declares functions taking
  `uint32_t`, and `uint32_t` comes from `<stdint.h>`.

---

## 1. The one-paragraph version

A compile that reaches `#include "anet.h"` should be able to load a
previously parsed `anet.h` instead of parsing the text again. To do that
safely, the cache needs four things. It needs a way to **record** what a
parse of `anet.h` depended on (§4). It needs a way to **make and store** the
parsed form (§5, §6). It needs a way to **check**, at a later `#include`,
that the recorded dependencies still hold (§6.4). And the compiler must
**ask** the cache at each `#include` (§6.6). All four exist and are tested.
A hit leaves the compile as reading the header would have, macros and
pragma state included.

**NEW:** the saved copies are no longer built by a separate compile. A
compile that misses exports the header's part of its own work and pushes it
to the daemon (`9f46203f6`, §5.2). A part refers to outside declarations by
name, and the loading compile uses its own declaration of each name
(`0afcadce2`, §5.3). A hit also checks the include path and `__FILE__`
(`9b86dcc4e`, §6.4).

---

## 2. What runs, and what does not

**Runs:**

- `clang-header-cache-daemon` (§6.3): holds saved copies in the shared region,
  answers cache queries, keeps the ask-list.
- The hit path, when `CLANG_HEADER_CACHE` names the daemon's socket (§6.6).
  Unset, nothing is installed and the compile is unchanged.
- Every compile with the hit path times each header it reads and reports the
  times to the daemon, which keeps the ask-list (§6.6).
- **NEW (`9f46203f6`):** a compile exports each header on the ask-list that it
  has to read itself, and pushes the part to the daemon (§5.2). This is how
  the cache fills during a build.
- **NEW (`0afcadce2`):** after its first hit too. Before `0afcadce2`, a compile
  stopped exporting at its first hit.
- **NEW (`f05e79584`):** `CLANG_HEADER_CACHE_STATS=1` prints where the cache's
  time went in the compile (§6.6).
- The separate producer and `clang-header-cache-populate` (§5.1, §6.5) still
  work. Nothing needs them now that compiles export parts.
- 139 unit tests, all passing (§8). 115 existed at `2a4866313`.

**Does not exist** (§10 has the full list): the `__COUNTER__` dependency,
persistence across a daemon restart, eviction from a full region, and a run
over a whole build with compiles in parallel.

---

## 3. Observation hooks added to Clang itself

The cache needs to see things Clang did not previously report. Each hook is
small, off by default, and shaped after an existing Clang interface so it
looks like the surrounding code. None changed after `2a4866313`.

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

- **`clang/include/clang/Lex/PPCallbacks.h` (+22 lines)**: adds
  `UndefinedIdentifierInDirective(const Token &)`, plus forwarding in
  `PPChainedCallbacks` so a chain of observers all receive it.
- **`clang/lib/Lex/PPExpressions.cpp` (+6 lines, at `EvaluateValue`)**: fires
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

- **`clang/include/clang/Lex/DeclBoundaryObserver.h` (42 lines, new)**: a
  one-method interface, `bool isAtDeclarationBoundary() const`, modelled on
  `CodeCompletionHandler`.
- **`clang/include/clang/Lex/Preprocessor.h`**: one member plus
  `setDeclBoundaryObserver` / `getDeclBoundaryObserver` /
  `clearDeclBoundaryObserver`.
- **`clang/lib/Lex/Preprocessor.cpp`**: the include, and an out-of-line
  destructor so the class has a home for its vtable.
- **`clang/include/clang/Parse/Parser.h` (+21 lines)**: `Parser` implements
  the interface: `ParenCount == 0 && BraceCount == 0 && BracketCount == 0 &&
  !InDeclGroup`.
- **`clang/include/clang/Parse/RAIIObjectsForParser.h` (+31 lines)**:
  `ParsingDeclGroupRAII`, which sets `InDeclGroup`.
- **`clang/lib/Parse/ParseDecl.cpp` (+16 lines)**: that RAII object wraps the
  body of `ParseDeclGroup`.
- **`clang/lib/Parse/Parser.cpp` (+3 lines)**: registers the parser as the
  observer in the constructor, clears it in the destructor.

Why `InDeclGroup` exists separately: in `int a, b;` an `#include` between
`a,` and `b;` touches none of the three nesting counters, so without the flag
the preprocessor would think that point was a safe boundary.

The hit path (§6.6) asks it at every eligible `#include`. Its first real use
exposed that it rejected almost everything: `InDeclGroup` was held while the
terminating `;` was consumed, and because the parser reads one token ahead,
consuming that `;` processes the `#include` on the next line with the flag
still set. `ParseDeclGroup` now restores the flag before the `;` and before a
function body (`ab65051ab`).

### 3.4 What an identifier currently means

- **`clang/include/clang/Sema/NameClassificationObserver.h` (100 lines, new)**:
  four methods, one per query Clang's parser uses to resolve grammar
  ambiguity: `recordTypeNameQuery`, `recordClassification`,
  `recordTemplateNameQuery`, `recordDeductionGuideNameQuery`.
- **`clang/include/clang/Sema/Sema.h` (+84 lines)**: the observer pointer,
  its set/get/clear, and declarations of the four renamed `…Impl` functions.
- **`clang/lib/Sema/SemaDecl.cpp` (+61 lines)**: `getTypeName` and
  `ClassifyName` each become a thin wrapper around a renamed `…Impl` holding
  the original body, reporting once at the wrapper's single return.
- **`clang/lib/Sema/SemaTemplate.cpp` (+29 lines)**: the same for
  `isTemplateName` and `isDeductionGuideName`. C never calls either; the
  tracker poisons the frame if one fires (DECISIONS 1.11).

Why wrappers and not edits at each `return`: `getTypeName` has 24 return
points across 321 lines and `ClassifyName` 22 across ~390. Editing each one
would be invasive and easy to get wrong.

Why these four: `clang/docs/InternalsManual.rst:1252` states the parser's own
discipline — every `tok::identifier` or `tok::coloncolon` goes through
`TryAnnotateTypeOrScopeToken` or `TryAnnotateCXXScopeToken`. Tracing every
call site in `clang/lib/Parse` yields exactly these four entry points.

Cost to an ordinary compile: one null check per call.

### 3.5 Every identifier considered for expansion

- **`clang/include/clang/Lex/ExpansionCandidateObserver.h` (50 lines, new)**:
  one method, `expansionCandidate(const Token &)`, called for each identifier
  at the point where the preprocessor decides whether to expand it.
- **`clang/include/clang/Lex/Preprocessor.h`**: the observer pointer and its
  set/get/clear; `noteExpansionCandidate`, inline and one pointer test; and
  `refreshIdentifier`, which brings a name up to date after a module load
  before anyone asks whether it is a macro.
- **`clang/lib/Lex/Preprocessor.cpp`**: the out-of-line half, which skips
  identifiers read with expansion off: a macro's own name in `#define`, the
  operand of `#ifdef` or `defined()`.
- **`clang/lib/Lex/Lexer.cpp` (+2 lines)**: text read from a file
  (`LexIdentifierContinue`) and clang-scan-deps' minimized sources
  (`LexDependencyDirectiveToken`).
- **`clang/lib/Lex/TokenLexer.cpp` (+5 −2 lines)**: tokens coming out of a
  macro's body, a `##` result included.
- **`clang/lib/Lex/PPMacroExpansion.cpp` (+6 lines)**: a macro whose whole
  body is one token, which `HandleMacroExpandedIdentifier` returns without a
  token lexer.

Why it was needed: an earlier `#define` can turn any identifier in a header
into something else — zmalloc.c's `#define malloc tc_malloc`, qemu's
`#define inline __attribute__((gnu_inline))` — so every identifier the header
reads is a dependency, macro or not. A name that is not a macro passes the
expansion check without calling anything, so no existing callback saw it
(DECISIONS 1.12).

It is a pointer on `Preprocessor`, not a `PPCallbacks` method, because an
ordinary compile always has callbacks attached and a new one would put a
chain of virtual calls on every identifier. Measured with no observer set:
redis `object.c` 3,070 → 3,084 ms, `server.c` 4,943 → 4,919 ms, within
noise.

---

## 4. The data model, and collecting it

### 4.1 `clang/include/clang/ASTCache/DependencyRecord.h` (265 lines)

What a parsed header can depend on, and what it does to the compile that
includes it. `HeaderDependencySet` bundles all of it; `HeaderRecord` pairs one
set with the header's path.

| Struct | Records |
|---|---|
| `ConditionalDependency` | A macro whose *definedness* was tested (`#ifdef`, `defined()`), and the answer. |
| `SyntacticDependency` | An identifier the preprocessor considered for expansion inside the header, and what it had to be: *not a macro*, or a macro with exactly this replacement list (DECISIONS 1.12). |
| `SymbolDependency` | An identifier a grammar-ambiguity query asked about, which query, and the answer, including the resolved type's canonical spelling. |
| `InclusionDependency` | An `#include` that did nothing because the file was already included (`#pragma once`, `#import`). |
| `PragmaSnapshot` | Pragma state, twice: `Entry`, the state the header was compiled under, and `Exit`, the state the header leaves behind. |
| `MacroEffect` | A macro the header defined or undefined, and whether the header left it defined. This is an effect, not a dependency: a hit replays it (§6.6), nothing checks it. |
| **NEW (`9b86dcc4e`)** `IncludeDependency` | One `#include`, `#include_next`, `__has_include` or `__has_include_next` the header's region ran, and the file it found. |
| **NEW (`9b86dcc4e`)** `FileNameDependency` | One `__FILE__` or `__FILE_NAME__` the region expanded, and the text it produced. |
| **NEW (`9f46203f6`)** `DeclarationDependency` | One outside declaration a part refers to, by name space, name and fingerprint. |

Design notes worth knowing. `SymbolDependency::Classification` mirrors
`clang::NameClassificationKind`'s integer values rather than including
`Sema.h`, so the record stays cheap to include. A macro is a dependency
**only if its state came from outside the header's own closure**: a macro
defined and used inside the same header can never vary (§4.2).

**NEW (`9b86dcc4e`): the include path.** The daemon keys saved copies by the
header's path, so a hit loads a copy of the right file. Nothing checked which
files that header's *own* `#include`s find. They depend on the compile's
`-I` flags. qemu compiles some files once per target CPU, and
`include/system/kvm.h`'s `#include "cpu.h"` finds `target/arm/cpu.h` under
`-I../target/arm` and `target/i386/cpu.h` under `-I../target/i386`. An
`IncludeDependency` records each such header search, in the order the
directives ran:

- `K`: which directive, and `Angled`: `<>` or `""`.
- `Spelled`: the name as written, after macro expansion.
- `Includer`: which file holds the directive. -1 for the header itself;
  otherwise the index of the earlier entry that found that file. So the
  entries form a tree, and a hit can redo them in order (§6.4).
- `Found`: the real path of the file found, empty when `__has_include` found
  nothing.
- `Name` (**NEW, `9f46203f6`**): the name the search found the file by,
  `./ae.h` or `/usr/include/stdio.h`. A part keeps its files under these
  names, and `__FILE__` and debug information repeat them, so a hit of a part
  requires the same names.

**NEW (`9b86dcc4e`): `__FILE__`.** `__FILE__` expands to the name the compile
opened the file by, not to the file. redis's `ae.c` includes `ae_epoll.c`,
found beside it: a compile run from `src/` calls it `./ae_epoll.c`, one that
opened it by absolute path calls it `/home/.../redis/src/ae_epoll.c`. The text
is inside the parsed header, so a `FileNameDependency` keeps it: which macro,
which file (-1 or an `IncludeDependency` index, as above), and the text after
`-fmacro-prefix-map`. A use after a `#line "name"` in the file's own text is
not recorded: every compile reads that `#line` the same way.

**NEW (`9f46203f6`): outside declarations.** A part's declarations refer to
declarations its region did not produce: `anet.h`'s functions take
`uint32_t`. A `DeclarationDependency` names each one at file scope
(`In`: the tag name space, for `struct`/`union`/`enum` names, or the
ordinary one) with its fingerprint (§4.6). A hit requires the loading compile
to have a declaration of that name with a matching fingerprint.
**CHANGED (`0afcadce2`):** why the hit requires it. In `9f46203f6` a part held
a deep copy of the declaration, and the copy had to merge with the loading
compile's own. Now the part refers to the declaration by name and uses the
loading compile's own (§5.3), so the part's declarations must have been
compiled against an equal one.

**NEW (`9f46203f6`): `HeaderName`.** The name the exporting compile opened
the header by. A hit of a part requires the same.

### 4.2 `clang/include/clang/ASTCache/ASTCacheDependencyTracker.h` (311) and `clang/lib/ASTCache/ASTCacheDependencyTracker.cpp` (510)

A `PPCallbacks` that also implements `ExpansionCandidateObserver` and
`NameClassificationObserver`, so one object collects the preprocessor's, the
lexer's and Sema's facts.

**A stack of frames.** The tracker keeps one frame per file entered, not one
set per translation unit. Headers nest, and the same macro expansion has a
different answer at each level: a macro defined in `a.h` and used in the
`b.h` that `a.h` includes is a real dependency of `b.h` and not one of `a.h`,
whose own text fixes it. So each sighting of a name is asked the provenance
question once per open frame.

**First sighting decides.** A frame records a name the first time it sees it
and never again. A header that uses `BUF` and then defines it depended on
`BUF` not being a macro at that use, so it records NotAMacro; one that defines
`BUF` first does not depend on the includer for it, and records nothing.

**Conditionals face outward.** A definedness test is recorded for a frame
only if the state the test saw came from outside that frame's closure: from
the active `#define`, or, for an undefined name, from the `#undef` that
removed it (`stateOrigin`). A name never defined, a `-D`, and a builtin count
as outside. redis's `fmacros.h` defines `_BSD_SOURCE` and then includes
`<features.h>`, which tests it; `fmacros.h`'s own test is not its dependency,
`features.h`'s still is. The same rule skips an include-guard or
`#pragma once` skip of a file the frame itself read earlier (`ClosureFiles`).

**Poisoning.** A frame that meets something the tracker cannot describe is
dropped entirely and emits no record. A partial record would carry *fewer*
constraints than the compile that produced it, so it would match compiles it
must not. No record means a miss.

**NEW (`9b86dcc4e`): recording header searches.**

- `InclusionDirective` and `HasInclude` call `recordLookup`, which appends an
  `IncludeDependency` to every open frame whose closure holds the directive.
- The entry's `Includer` comes from `EntryOf`, a per-frame map from each file
  entered under the frame to the entry that found it. `FileChanged` fills it:
  `InclusionDirective` leaves its new entries in `PendingEntries`, with the
  file they found in `PendingFile`, and when that file is entered each frame
  maps the new `FileID` to its entry.
- `HasInclude` cannot tell `__has_include_next` from `__has_include`;
  `MacroExpands` sees which builtin ran just before and sets
  `NextHasIncludeIsNext`.
- Two cases poison the frame. A directive in a file of the region that no
  recorded search entered cannot be placed in the tree. And with
  `-fms-compatibility`, a quoted search also looks beside every file on the
  include stack, which a hit's search from one includer cannot reproduce.

**NEW (`9b86dcc4e`): recording `__FILE__`.** `MacroExpands` sees `__FILE__`
and `__FILE_NAME__` and calls `recordFileName`. It computes the text the
same way `Preprocessor::ExpandBuiltinMacro` does (through `fileMacroText`,
§4.3) and records it once per file and macro per frame (`SeenFileNames`).

**NEW (`9f46203f6`): tracking only some headers.** In a compile that exports,
the tracker runs inside the real compile. `trackOnly(Pred)` makes it open a
frame only for the files the exporter is following. Every file still joins
the closures of the open frames, so a tracked header's record is the same.
With no frame open, every hook returns at once, so a compile exporting
nothing pays almost nothing. `recordFor(FileID)` returns the finished record
of one file (`CompletedFIDs` holds the file for each finished record).

### 4.3 `clang/include/clang/ASTCache/MacroTokens.h` (62) and `.cpp` (53)

`serializeMacroTokens` turns a macro's replacement list into a string. It is
its own file because two sides must produce byte-identical output: the
tracker when recording, and the client when checking a stored record against
a live macro. If those two ever drifted, every ExpandsTo dependency would
compare unequal and nothing would ever hit — a failure that looks exactly like
the cache simply not helping. A literal's text is read from the source with
`Lexer::getSpelling`, because a macro loaded from a saved copy carries no
pointer to its text.

**NEW (`9b86dcc4e`):** `fileMacroText(Name, LastComponentOnly, ...)`, what
`__FILE__` (or `__FILE_NAME__`) expands to for a file the compile names
`Name`, through the preprocessor's own `processPathForFileMacro` and
`processPathToFileName`. It lives here for the same reason: the tracker
records the text and the client computes it again, and the two must not
drift.

### 4.4 `clang/include/clang/ASTCache/PragmaStateCheck.h` (87) and `.cpp` (111)

`#pragma pack(1)` before an `#include` changes the layout of every record in
the header, and no macro is tested or expanded, so the other categories record
nothing. This file captures the pragma stacks (pack, float control) into a
`PragmaSnapshot`, and reports any pragma state a record cannot express, in
which case the header is not saved.

Serialization does not solve this on its own:
`ASTWriter::WritePackPragmaOptions` returns early while a module is being
written, so the PCM carries no pragma state in either direction.

### 4.5 `clang/include/clang/ASTCache/ByteStream.h` (133), `DependencySerialization.h` (77) and `.cpp` (267)

A flat byte encoding for `HeaderDependencySet`, the little-endian primitives
under it, and the NUL-separated lists in which a record's NotAMacro names and
its macro effects travel (§6.3).

Two properties matter. Strings are length-prefixed, not NUL-terminated,
because a serialized replacement list contains NULs by construction — they
are its token separator. And the reader **stays failed**: once any read runs
past the end, every later read returns zero and `ok()` is false, so a caller
decodes a whole structure and checks once. A decoder that returned what it had
managed to read would produce a partial record, which is the unsafe direction
(§4.2).

**CHANGED:** the encoding version is 6; it was 4 at `2a4866313`. Version 5
(`9b86dcc4e`) adds the `IncludeDependency` and `FileNameDependency` lists.
Version 6 (`9f46203f6`) adds the `DeclarationDependency` list,
`IncludeDependency::Name` and `HeaderName`. The decoder rejects an `Includer`
or `File` index that points at an entry after its own: a hit would redo a
search from the wrong file.

### 4.6 `clang/include/clang/ASTCache/DeclFingerprint.h` (65) and `.cpp` (158) — NEW (`9f46203f6`, extended in `0afcadce2`)

What must be equal between the outside declaration a part was compiled
against and the loading compile's declaration of the same name. One file,
for the same reason as §4.3: the exporting compile computes the fingerprint
and the hit check computes it again.

`declarationFingerprint(D)` returns a string. Examples, from redis:

```
typedef unsigned int                                   (uint32_t)
struct defined 15253398 layout 216/8@0,64,128,...      (struct _IO_FILE)
function int (const char *, ...) asm(printf) declared  (printf)
enumerator int 4                                        (BUFN in a test)
```

- *Kind* first: `typedef`, the tag kind, `function`, `variable`,
  `enumerator`.
- *A struct, union or enum*: `declared`, or `defined` with Clang's ODR hash
  of the definition. **NEW (`0afcadce2`):** a struct or union also gets its
  layout: size, alignment and every field offset, from
  `ASTContext::getASTRecordLayout`. The ODR hash leaves out attributes such
  as `packed` and `aligned`, and a part can hold constants computed from the
  layout: `char raw[sizeof(struct pt)]` is an array of 8 in the part.
- *A typedef*: the canonical type it names, the description of the struct it
  names if any, and **NEW (`0afcadce2`)** its `aligned` attribute, which the
  canonical type does not show.
- *A function or variable*: the canonical type, the asm label, and whether a
  definition exists; for a function, the definition's ODR hash; **NEW
  (`0afcadce2`)** for a variable, the hash of its initializer, since Clang
  folds a `const` variable's value into constants such as array bounds.
- **NEW (`0afcadce2`):** *an enumerator*: its type and value. A part holding
  `char data[BUFN]` holds the array bound as a number.

`fingerprintSatisfies(Carried, Live)`: equal strings always satisfy. A part
compiled against only a declaration (`... declared`) is also satisfied by a
definition of the same kind and type: code that compiled against an
incomplete `struct pt` compiles the same against a complete one. The reverse
is not satisfied.

**NEW (`0afcadce2`): taken as of the header's start.** The exporting compile
has, by the time it writes the part, also read the header's own
redeclarations. `bits/stdlib-bsearch.h` defines the `bsearch` that
`stdlib.h` declared; fingerprinted then, `bsearch` read "defined", and a
compile where `bsearch` was only declared at that `#include` rejected the
part. `declarationFingerprint` takes a `Counts` predicate saying which
redeclarations to look at for a definition or an asm label; the exporter
passes those made before the header (§5.2).

`nameSpaceOf(D)` says which name space a name is looked up in. A declaration
the function cannot fingerprint (no name, not at file scope) has none, and
such a part is not written by name (§5.3).

---

## 5. Making a saved copy

There are two ways. The first, a separate compile per header, came first and
still works. The second, exporting from the real compile, is **NEW
(`9f46203f6`)** and is what a compile does now.

### 5.1 The separate producer: `clang/include/clang/ASTCache/ASTCacheProducer.h` (251) and `clang/lib/ASTCache/ASTCacheProducer.cpp` (940)

**Why a separate compile.** The parsed form of a header seemed impossible to
slice out of a real compile because of include guards: in a translation unit
that reads `bar.h` before `foo.h`, `foo.h`'s own `#include "bar.h"` does
nothing, so `bar.h`'s declarations are outside the slice while `foo.h`'s
declarations point at them. A separate compile of `foo.h` fires that
`#include` for real. The user never approved this design; exporting from the
real compile became the default on 2026-09-30 (DECISIONS 2.1), and §5.2 is
that export. A part solves the include-guard problem differently: it refers
to `bar.h`'s declarations by name (§5.3).

**The context.** A header like redis's `anet.h` uses `uint32_t` and never
includes `<stdint.h>`, because whatever includes `anet.h` included
`<stdint.h>` first. So a request carries the lines the compile had run before
the header: `#include`, `#define` and `#undef` lines, in order
(`ContextEntry`), plus the compile's flags. For a header included from
another header, the list holds the `.c` file's lines before `#include "a.h"`,
then `a.h`'s lines before `#include "b.h"`. The `#define`s are what make glibc
work: `bits/stdio_lim.h` begins `#ifndef _STDIO_H` /
`#error "Never include <bits/stdio_lim.h> directly"`.

**What the producer writes.** Clang only builds a PCM from a module map, so
the producer writes three files into a fresh temporary directory:

```
__hc_context.h     #include <stdint.h>                     (one line per context entry)
__hc_target.h      #include "/…/redis/src/anet.h"
module.modulemap   module __hc_<hash> {
                     explicit module context { header ".../__hc_context.h" }
                     module target { header ".../__hc_target.h" export * }
                   }
```

`explicit` keeps the context hidden: a textual `#include "anet.h"` makes
`anet.h`'s declarations visible and nothing else. The three files are stored
inside the PCM (`-fmodules-embed-file=`), since the directory is deleted.

**Replaying an include the way the compile found it.** Each context line
keeps the text as written (`<stdint.h>`) and the file opened. The text
matters because `#include_next` continues the search *after* the directory
the current file was found in; that is how Clang's `stdint.h` reaches
glibc's. The recorded path checks the replay: a replay that opens a different
file is declined.

**Module names.** `__hc_` plus 16 hex digits of xxh3 over the header, flags
and context. Two PCMs with one module name cannot load into one compile.

**Effects.** `TargetWatcher` collects every macro the target defines or
undefines and records its final state as a `MacroEffect`.

**When the producer declines** (each is a miss, never a wrong answer): the
header does not compile in its context; it was reached through its own
context; a replayed include opened another file; pragma state the record
cannot carry; the header expands `__COUNTER__`, `__INCLUDE_LEVEL__`,
`__BASE_FILE__`, `__DATE__`, `__TIME__` or `__TIMESTAMP__`, directly or
through a macro; the tracker poisoned the frame.

**Job files** (`clang-header-cache-jobs 3`): a plain-text list of requests, so
a compile that missed can hand its work to `clang-header-cache-populate`.
Version 3 adds `define` and `undef` lines; version 2 still reads.

**NEW (`9f46203f6`):** `isTranslationUnitVaryingBuiltin(Name)` exposes the
builtin list above, which the exporter shares.

### 5.2 Exporting from the real compile: `clang/include/clang/ASTCache/PartExporter.h` (85) and `clang/lib/ASTCache/PartExporter.cpp` (538) — NEW (`9f46203f6`, `0afcadce2`)

A compile that misses a header on the ask-list saves the header's part of its
own work and pushes it to the daemon. DECISIONS 2.17 and 2.18 have the
design and the measurements.

**When.**

1. At a miss, `HeaderCacheServer::tryServe` calls
   `PartExporter::exportNext(File)` before returning, and the preprocessor
   enters the file as usual.
2. The exporter is a `PPCallbacks`. When that file is entered, `begin()` opens
   a `Part`: the `FileID`, the `#include`'s location, the name the compile
   opened the header by (`./anet.h`), its absolute path (the daemon's key),
   and the header's first source-location offset.
3. When the file is left, `end()` notes the next free offset. The region is
   every offset in between: the header's own text and every file first
   entered under it. `end()` also records the macro effects (every macro the
   region defined or undefined, and its state now) and the pragma state.
4. The part is written **not** when the file ends: the parser reads one token
   ahead, so the header's last declaration may still be open then. It is
   written at the first top-level declaration the parser finishes afterwards,
   or at the end of the translation unit. That signal comes from
   `ExportConsumer`, an AST consumer registered as a frontend plugin
   (`header-cache-export`, `AddBeforeMainAction`). Every compile gets one; it
   finds its compile's exporter through a registry, or does nothing.

**What it writes** (`write()`, `writePart()`):

- *The metadata.* The tracker's record for the header (§4.2, with
  `trackOnly` set to the headers being exported), plus the effects, the
  pragma snapshots, and `HeaderName`.
- *The module name.* `__hcp_` plus 16 hex digits of xxh3 over the absolute
  path, the compile's module hash (`CompilerInvocation::getModuleHash`) and
  the encoded metadata. The same part from two compiles gets the same name,
  so the daemon keeps one.
- *The part itself*, through Clang's own `ASTWriter` with a part filter
  (§5.3): `inPart(P, Loc)` accepts a declaration whose location, after macro
  expansion, falls in the region.
- *The outside declarations*, by name by default (§5.3). Each file-scope name
  the part refers to becomes a `DeclarationDependency`, fingerprinted as of
  the header's start: the latest declaration made before the header, and a
  `Counts` predicate (`Before`) that ignores the region's own redeclarations
  and anything made after the header (`madeAfter`).

**`madeAfter(P, D)`:** whether the compile made `D` after the header ended.
Offsets cannot say: a file's offsets are handed out when the file is
entered, so `anet.c`'s text after `#include "anet.h"` has offsets *below*
`anet.h`'s. It uses `SourceManager::isBeforeInTranslationUnit` against the
`#include`'s location.

**When a header is not exported** (`decline`):

- the region expands one of the builtins in §5.1;
- pragma state a record cannot carry, at entry or exit;
- the tracker poisoned the frame, or Sema did not exist;
- a saved copy was loaded, or served, inside the header: the part would hold
  an import, not the declarations (`served()`, **NEW `0afcadce2`**);
- with deep copies (`CLANG_HEADER_CACHE_OUTSIDE=copy`), a saved copy was
  loaded before the header;
- an outside declaration cannot be named, after a load (without a load, the
  part is written with deep copies instead);
- the same part was already exported in this compile.

**Options.** `CLANG_HEADER_CACHE_OUTSIDE=copy` writes deep copies of outside
declarations (§5.3). `CLANG_HEADER_CACHE_DUMP_PARTS=<dir>` also writes each
part to `<dir>/<module name>.pcm`, with a `.txt` naming the compile it came
from and its outside declarations. `stats()` counts parts exported, written
with deep copies, and declined, with a reason for each; the server prints
them (§6.6).

### 5.3 What Clang's PCM writer does for a part — NEW (`9b86dcc4e`, `9f46203f6`, `0afcadce2`)

Files: **`clang/include/clang/Serialization/ASTWriter.h` (+112)**,
**`clang/lib/Serialization/ASTWriter.cpp` (+222)**,
**`clang/lib/Serialization/ASTWriterDecl.cpp` (+118)**,
**`clang/include/clang/Serialization/ASTBitCodes.h` (+24)**. Nothing here
changes what the writer does without a part filter; `prune.m` is the one
Clang module test that fails, and it fails because file access times do not
update on this NFS home directory.

**The filter** (`setPartFilter`, four predicates, `9b86dcc4e` and
`9f46203f6`):

- `PartDecls`: which declarations start what is written. Only the
  translation unit's declarations the predicate accepts are written as
  roots (`PrepareWritingSpecialDecls`); the rest of the translation unit is
  written only as far as those reach. Sema's lists (tentative definitions,
  unused file-scope declarations and so on) are skipped, as for a reduced
  BMI.
- `PartMacros`: which macro directives are the part's. Their final values are
  written as the part's own module macros, as for a C++20 header unit.
  **NEW (`0afcadce2`):** the writer walks back to the region's own last
  directive of each macro: a header loaded after the region may have replayed
  a newer one, and the region's `#define` was then dropped.
- `PartKeepsExpansion`: which macro expansions keep their entry in the
  source-location table, by starting offset. A part keeps only those inside
  the region. A location inside a dropped expansion is written as the place
  the macro was used (`getAdjustedLocation`), which is the file, line and
  column debug information reports anyway. This reuses the mechanism that
  drops unused module maps (`computeNonAffectingInputFiles`). It took
  `util.h`'s whole-compile tables from 797 KB to 42 KB. **NEW
  (`0afcadce2`):** an expansion of a macro spelled in a loaded module file is
  dropped too.
- `PartKeepsHeaderInfo`: which files keep their entry in the header-search
  table, with their include guard, `#pragma once` and "already included"
  marks. A part keeps only the files its region entered, so a loading compile
  does not skip a file it never read.

**Other part-mode changes:**

- Lookup tables and C's per-name declaration chains list only declarations
  the part holds. **CHANGED (`0afcadce2`):** the chain for a name lists the
  part's own *latest* redeclaration. It used to list the latest at write
  time and drop it if the part did not hold it; when a later header had
  redeclared `size_t`, the part listed no `size_t` at all.
- Identifiers are written only when something written uses them.
- Input files keep the name the compile opened them by (`./ae.h`), not an
  absolute path, so the loading compile's debug information names files as
  a compile that read them would.
- **NEW (`0afcadce2`):** the special types Clang writes into every PCM for
  builtins (`FILE`, `jmp_buf`, `sigjmp_buf`, `ucontext_t`) are written only by
  the part that declares them. Before, every part required `FILE`.
- **NEW (`0afcadce2`):** a location in a loaded module file has no encoding
  in a part, which imports nothing. Such a location is written as invalid and
  counted (`getPartDroppedLocations`); none were left on redis.

**Outside declarations by name** (`setPartOutside`, **NEW `0afcadce2`**).
How it works, mechanism first:

- *Clang's mechanism.* Clang serializes a reference to a declaration as a
  declaration ID. Every declaration the writer reaches gets an ID and is
  written in full, so a part used to hold a **deep copy** of `size_t` from
  `stddef.h`, and the loading compile merged the copy with its own `size_t`.
- *Which declarations are outside.* `isPartLocal(D)` asks whether the
  outermost declaration around `D` (a field's struct, a parameter's function)
  is accepted by `PartDecls`. Anything else is outside, and
  `isPartNameReference(D)` says the part refers to it by name.
- *The record.* `WriteDecl` writes a new record, `DECL_NAME_REFERENCE`
  (`ASTBitCodes.h`), instead of the declaration
  (`ASTWriter::WritePartNameReference`). It holds the declaration's kind, how
  to find it (`NameReferenceKind`), and a parent:

  | How | Parent | Then | Example |
  |---|---|---|---|
  | `Named` | none | tag or ordinary, the name | `size_t`, `struct sockaddr`, the enumerator `RED` |
  | `Named` | a struct | the name | the field `sa_family` of `struct sockaddr` |
  | `TagOfTypedef` | a typedef | — | the unnamed struct `fd_set` names |
  | `Unnamed` | a struct | its position among unnamed members of its kind | an anonymous union inside a struct |
  | `Unnameable` | — | — | the writer could not name it |

  Types need nothing new: a `TypedefType` or `RecordType` refers to its
  declaration by ID, so a type built on a name reference becomes the loading
  compile's own type.
- *Checked when written.* A file-scope name must find this very declaration
  in the exporting compile (`ASTReader::lookupAtFileScope`, §5.4), and an
  `overloadable` function is not named, since the name alone does not say
  which. Otherwise the record is `Unnameable`, and the exporter writes the
  part with deep copies instead, or declines it after a load.
- *Loaded declarations.* A declaration from a loaded module file is referred
  to the same way, with a local ID (`GetDeclRef`, `getDeclID`,
  `wasDeclEmitted`). So a compile keeps exporting after its first hit.
- *Not listed anywhere.* `wasDeclEmitted` is false for a name reference, so
  no lookup table, name chain or Sema list in the part names it, and its
  offset entry has no location.
- *Redeclarations.* A part's declaration that redeclares an outside one
  (`bsearch`) lists the outside one as its first declaration. `IsLocalDecl`
  is false for name references, so the part's own first redeclaration is the
  first local one, and `VisitRedeclarable` lists only the part's own.
- *Declarations made after the header* (`setPartOutside`'s second argument,
  in both modes). They are not local either, so a redeclaration chain neither
  lists them nor writes the most recent one when it is one of them. Before,
  a deep copy of a chain could carry the main file's definition: `sds.c`
  defines `SDS_NOINIT` right after including `sds.h`, which declares it.
- *What the exporter gets back:* `getPartNameReferences()`, the file-scope
  names referred to, and `getPartNameReferenceFailure()`, the first
  declaration that could not be named and why.

### 5.4 What Clang's PCM reader does — NEW (`0afcadce2`)

Files: **`clang/include/clang/Serialization/ASTReader.h` (+13)**,
**`clang/lib/Serialization/ASTReader.cpp` (+36)**,
**`clang/lib/Serialization/ASTReaderDecl.cpp` (+70)**.

- **`ASTReader::lookupAtFileScope(Sema, Name, Tag, CreateBuiltin)`.** Walks
  Sema's identifier chain for the name and returns the first visible
  declaration in the right name space whose context is file scope (a struct
  or enum at file scope counts, since C puts their tags and enumerators in
  the enclosing scope). It passes over a block-scope declaration that hides
  it, because a name reference may be resolved while the compile is inside a
  function. With `CreateBuiltin`, a builtin that is not a library function is
  declared if it is not yet, as its first use in the text would. Three places
  call it: the writer's check (§5.3), the hit check (§6.4), and the reader.
  So the reader resolves a name to the declaration the hit check approved.
- **`ReadDeclRecord`** handles `DECL_NAME_REFERENCE` before creating any
  declaration (`resolveNameReference`). It resolves the parent, then the
  name: `lookupAtFileScope`, a lookup in the struct's definition, the
  typedef's underlying type, or the unnamed member at that position. The
  result must have the recorded kind. It is stored as the loaded declaration
  for that ID, as `getPredefinedDecl` does for the translation unit, so every
  reference in the part points at the loading compile's own declaration.
  A failure is fatal: the hit check approved every name before the load.
- **Redeclarations** need nothing new: a part's declaration whose first
  declaration resolves to the compile's own is attached at the end of the
  compile's chain, as for a module that redeclares an imported declaration.
- A deserialization listener is not told about a declaration the compile made
  itself (`GetDecl`).

---

## 6. Storing and serving saved copies

### 6.1 `clang/include/clang/ASTCache/SharedRegion.h` (114) and `.cpp` (196)

An append-only file, mapped into memory by the daemon and by every compile.
Append-only is the concurrency argument: a written entry is never modified or
moved, so readers need no locks, and the socket reply that hands out an
offset is itself the synchronisation point — a compile only learns of an
entry after it was fully written. The size is fixed at creation (4 GiB by
default, `/dev/shm/clang-header-cache.region`) because growing a mapping can
move it, which would invalidate every offset already handed out. The file is
sparse, so reserved space costs nothing.

The region's header holds the offset of the current ask-list. The daemon
publishes a new list with a release store and a compile reads it with an
acquire load, so a compile sees either the old list or the whole new one. The
header also holds an instance id, which a compile checks when it connects
(§6.4).

### 6.2 `clang/include/clang/ASTCache/CacheProtocol.h` (208) and `.cpp` (309)

The messages between a compile and the daemon, each length-prefixed:

- `HELLO`: answered with the region's path and instance id.
- `LOOKUP(filename)`: the cache query. Answered with every candidate stored
  under that name: its metadata, its module name, and where in the region its
  bytes, its NotAMacro names and its macro effects sit. The names (several
  hundred for a system header) and effects stay in the region, out of the
  reply.
- `PUSH(filename, metadata, bytes)`: store a saved copy; acknowledged.
- `TIMING`: the times this compile took to read each header; acknowledged.

The daemon never interprets metadata. Deciding which candidate fits needs the
compile's live macro table and Sema, which the daemon does not have.

### 6.3 `clang/include/clang/ASTCache/CacheDaemon.h` (160) and `.cpp` (262), plus `clang/tools/clang-header-cache-daemon/` (113 + 13)

A table from header path to candidates, over the region. Message handling is
a pure function from message to reply, so it is tested without a socket. One
thread per connection, blocking reads, no timeout: after a compile's last
cache query it spends a long time generating code without touching the
socket, and a parked thread costs nothing.

- **On a push**, the daemon moves the NotAMacro names and the effects into
  the region, and keeps the entry only if they and the bytes were written. A
  push is a duplicate only if both its metadata and its module name match a
  stored one: the compiler flags are not in the metadata, so a copy built with
  `-fshort-enums` must not shut out one built without it.
- **The ask-list.** The daemon keeps each header's first reported time and
  lists the headers at or above `-ask-cutoff-us` (226 µs by default, the cost
  of one PCM load measured on release clang 18). `-list-stored` also lists
  every header the moment a copy is stored.
- **Nothing survives a restart**: the daemon starts with an empty table and a
  fresh region. **A full region** is not an error: the push is acknowledged
  and the entry is not recorded. There is no eviction.

### 6.4 `clang/include/clang/ASTCache/CacheClient.h` (189) and `.cpp` (482)

The compile's side of the connection, and the judgement: does a candidate fit
this compile, here?

**Connecting** (`CacheClient::connect`). The compile sends `HELLO`, attaches
to the region path in the reply, and refuses the region if its instance id
differs. A compile once mapped the default region while its test daemon
wrote another file, and read names at offsets meant for that other file.

**`isListed(Path)`** reads the ask-list from the region, with no socket
traffic, and re-reads it only when its version changes.

**`lookup(...)`** sends the cache query, then checks each candidate in order
with `matchesLiveState`. A candidate that passes is offered to the caller
with its bytes and module name; the caller may still refuse it (a copy built
with other options, §6.6), and the next candidate is tried.

**`matchesLiveState`**: checks run cheapest first and stop at the first
mismatch. With no Sema, everything is rejected: pragma state lives in Sema,
and a check that cannot run must reject. In order:

1. Conditional dependencies: is each macro defined now as it was?
2. Syntactic dependencies that require a macro: same replacement text?
3. Inclusion dependencies: a `#pragma once` file skipped then must be
   skippable now.
4. The NotAMacro names, read from the region: none may be a macro now.
5. Symbol dependencies: does each name still resolve to the same type?
6. Pragma state at entry.
7. **NEW (`9b86dcc4e`, names `9f46203f6`): the header's own header searches**
   (`redoLookups`). Each recorded search is redone through the compile's own
   `HeaderSearch`, in order, the way `Preprocessor::LookupFile` makes it: a
   quoted search also looks beside the file holding the directive, and a
   `_next` search starts after the directory that file was found in. The
   `#include` being served supplies the first includer and its directory
   (`IncludeSite`). The first search that finds a different file rejects the
   candidate. For a part, the names must match too: the header itself must be
   opened by `HeaderName`, and each search must find its file by the
   recorded `Name`. A file the compile already read keeps the name it was
   first read by, as Clang's source manager keeps one record per file
   (`refOnEntry`).
8. **NEW (`9b86dcc4e`): `__FILE__`.** Each recorded text is computed again
   from the name the file has in this compile (`fileMacroText`) and compared.
9. **NEW (`9f46203f6`, `0afcadce2`): outside declarations.** Each name is
   looked up with `ASTReader::lookupAtFileScope`, the reader's own lookup
   (§5.4), with builtin creation allowed; the declaration found must satisfy
   the stored fingerprint (§4.6). **CHANGED (`0afcadce2`):** the lookup used
   to be Sema's `LookupName` and accepted any declaration in the result.

Every name is refreshed (`Preprocessor::refreshIdentifier`) before it is
asked about. A hit earlier in the compile loads a module, which can define a
name the compile had already seen, and until the identifier is updated its
macro bit still says it is not a macro.

`MismatchReason` names each kind of rejection; with
`CLANG_HEADER_CACHE_STATS=2` each rejection also says what failed:
`` `_BSD_SOURCE` was defined, is not ``, `` `<cfg.h>` found ./a/cfg.h, finds
something else ``. **NEW** reasons: `FileName`, `Include` (`9b86dcc4e`),
`Declaration` (`9f46203f6`).

**NEW (`f05e79584`): time counters.** `Stats` holds the wall time spent
waiting for the daemon's replies, checking candidates, reporting timings and
connecting. Measured on redis with a full cache, checking dominates, and
step 7 is most of it: 328 to 605 repeated header searches per compile, 4 to
6.6 ms (DECISIONS 2.18).

### 6.5 `clang/tools/clang-header-cache-populate/` (262 + 13)

The command-line front end to the separate producer (§5.1): headers as
arguments, the build's flags with `-Xcc`, the context with `-context`, or a
whole job file with `-job` (`-Xcc` may be combined with it). `-dry-run` builds
everything and reports without needing a daemon. Nothing needs this tool now
that compiles export parts; it still works.

### 6.6 The hit path

The pieces inside Clang:

- **`clang/include/clang/Lex/HeaderCacheHook.h` (49, new)**: one method,
  `Module *tryServe(FileEntryRef, SourceLocation, ConstSearchDirIterator)`.
  Null means "enter the file as usual". **CHANGED (`9b86dcc4e`):** the third
  argument is new: the search directory the file was found in, from which an
  `#include_next` inside it would continue (§6.4, step 7).
- **`clang/include/clang/Lex/Preprocessor.h` and
  `clang/lib/Lex/Preprocessor.cpp`**: the preprocessor owns the hook.
- **`clang/lib/Lex/PPDirectives.cpp` (+15)**: the hook is asked at the top of
  `case Enter:`, before the include-depth check and before a `FileID` is made.
  A returned module takes the same two lines as `case Import:`, so the parser
  and Sema learn of it through the ordinary annotation token. **CHANGED
  (`9b86dcc4e`):** passes `CurDir`.
- **`clang/include/clang/Frontend/CompilerInstance.h` (+11),
  `clang/lib/Frontend/CompilerInstance.cpp` (+3),
  `clang/tools/driver/cc1_main.cpp` (+6),
  `clang/tools/driver/CMakeLists.txt` (+1)**: a callback slot cc1 fills,
  because `clangASTCache` depends on `clangFrontend` and the dependency cannot
  also run the other way.

**`clang/include/clang/ASTCache/HeaderCacheServer.h` (67) and
`clang/lib/ASTCache/HeaderCacheServer.cpp` (816)**, the implementation.
`installHeaderCacheHook(CI)` reads the environment (§7). With
`CLANG_HEADER_CACHE` unset, it installs nothing.

`HeaderCacheServer::tryServe`, at each `#include`:

1. **Gates with no socket traffic.** The header is on the ask-list
   (`isListed`; otherwise counted as "not on the ask-list"); the `#include`
   is at a declaration boundary (§3.3); Sema exists. An `#include` in any
   file is asked about, in the `.c` file or in a header.
2. **The cache query**, with the header's absolute path, and the
   `IncludeSite` (**NEW `9b86dcc4e`**): the file, the directory it was found
   in, and the `#include`'s location.
3. **The load** (`load`), for a candidate that passed: its bytes are handed
   to the module reader under a name no file has (`/__hc_memory/N.pcm`) and
   read with `ReadAST`, which with `ARR_ConfigurationMismatch` returns quietly
   for a copy built with other options; the next candidate is then tried.
   A copy loaded before is reused (`LoadedTargets`): `stddef.h` has no guard
   and is included again and again, and loading a second module of the same
   name is a fatal error. **CHANGED (`9f46203f6`):** a producer's copy keeps
   the header in its `target` submodule; a part is the header, whole, so its
   top module is used.
4. **`__COUNTER__` is put back.** Loading a module file sets the counter to
   the value its writer ended with; the server saves it before the query and
   restores it after.
5. **After a hit**, the server makes the module visible, then replays the
   macro effects (`replayMacroEffects`): for each macro the header touched, a
   local `#undef`, or a local `#define` whose definition is the loaded
   module's own. A local directive overrides visible module macros, which is
   how a textual include behaves: an `#undef` in the header takes effect, and
   of two served headers defining one name, the later wins. Then the pragma
   state the header left is applied (`applyPragmaDelta`).
6. **NEW (`9f46203f6`): after a miss**, if the compile exports, the server
   calls `exportNext(File)` (§5.2). **NEW (`0afcadce2`): after a hit**, it
   calls `served()`, so an exported header with a served one inside it is not
   saved.

**Timing.** `ServerCallbacks` (a `PPCallbacks`) times each header from
entering it to leaving it, headers it includes counted in, first entry only.
At the end of the main file, `mainFileEnded` sends the times to the daemon
(`TIMING`), and the stats are printed: not from a destructor, because cc1
runs with `-disable-free` and destructors never run.

**Recording jobs** (`JobRecorder`, for the separate producer): with
`CLANG_HEADER_CACHE_JOBS=<dir>`, the compile writes, for every header it
reads, the request that would build its copy in context (§5.1).

**NEW (`9f46203f6`): export modes.** `installHeaderCacheHook` installs a
`PartExporter` (§5.2) unless `CLANG_HEADER_CACHE_EXPORT_PARTS=0`. With
`populate`, the server serves nothing and exports every header on the
ask-list that no stored copy fits (`AnyFits`).

**NEW (`9b86dcc4e`): the export experiment.** `CLANG_HEADER_CACHE_EXPORT=
<header>=<out.pcm>` exports one header's part to a file, and
`CLANG_HEADER_CACHE_LOAD=<header>=<in.pcm>` serves it from the file in a later
compile, checking nothing (`LoadExperiment`). For looking inside a part, not
for serving one.

**Stats** (`CLANG_HEADER_CACHE_STATS=1`): hits, cache queries, candidates
rejected and why, headers not on the ask-list. **NEW (`9f46203f6`):** parts
exported and declined, with each reason at level 2. **NEW (`f05e79584`):** one
line of time, for example
`header cache time: 10.6 ms in lookups (0.8 waiting for the daemon, 7.8
checking candidates, 1.3 loading), 0.1 reporting timings, 0.3 connecting`.

---

## 7. Environment variables and daemon options

| Setting | What it does | Since |
|---|---|---|
| `CLANG_HEADER_CACHE=<socket>` | Turns the cache on for a compile | `ab65051ab` |
| `CLANG_HEADER_CACHE_STATS=1` / `2` | Prints the stats at the end of the main file; `2` adds a reason per rejection, the served headers and each export | `ab65051ab`; **CHANGED** `9f46203f6`, `f05e79584` |
| `CLANG_HEADER_CACHE_JOBS=<dir>` | Records the producer's jobs | `2a4866313` |
| `CLANG_HEADER_CACHE_EXPORT_PARTS` | Unset: serve and export misses. `0`: serve only. `populate`: export only | **NEW** `9f46203f6` |
| `CLANG_HEADER_CACHE_OUTSIDE=copy` | Deep copies of outside declarations instead of references by name | **NEW** `0afcadce2` |
| `CLANG_HEADER_CACHE_DUMP_PARTS=<dir>` | Also writes each exported part and its outside declarations to `<dir>` | **NEW** `0afcadce2` |
| `CLANG_HEADER_CACHE_EXPORT=<header>=<out.pcm>` | Experiment: export one header's part to a file | **NEW** `9b86dcc4e` |
| `CLANG_HEADER_CACHE_LOAD=<header>=<in.pcm>` | Experiment: serve one header from a file, checking nothing | **NEW** `9b86dcc4e` |

Daemon options: `-socket` (default `/tmp/clang-header-cache.sock`),
`-region` (default `/dev/shm/clang-header-cache.region`), `-region-size`
(default 4 GiB), `-ask-cutoff-us` (default 226), `-list-stored`, and `-v`
(stats on exit).

---

## 8. Tests — 139, all passing

`clang/unittests/ASTCache/` (plus `CMakeLists.txt`, 26 lines, and one line in
`clang/unittests/CMakeLists.txt`). 115 tests existed at `2a4866313`; the 24
new ones are listed after each table row.

| File | Tests | What they pin down |
|---|---|---|
| `ASTCacheDependencyTrackerTest.cpp` (525) | 28 | Provenance: an internally defined macro is not a dependency; one from the includer is; a sub-header's macro is internal to its parent; a conditional is recorded at every open level; `#if UNDEFINED > 5` is recorded; a guard-suppressed `#include` is recorded. Syntactic: a plain name, a keyword, a `##` result and a name from an outside macro's body are recorded; the first sighting decides; names read with expansion off are not recorded. Outward conditionals: a flag a header sets for itself is not its dependency; an `#undef` inside the header pins the name. |
| | **NEW (`9b86dcc4e`), 4** | A `__FILE__` and a `__FILE_NAME__` in the header record their text; each `__FILE__` names the search that found its file; every header search records what it found, a guard-skipped `#include` and a `__has_include` that found nothing included; a `#line` name is not recorded. |
| `SymbolDependencyTest.cpp` (180) | 4 | The `T * p;` ambiguity: a typedef from the includer is a dependency; one the header declares itself is not; the resolved type's identity is recorded; a typedef from a sub-header is internal to the parent. |
| `DependencySerializationTest.cpp` (318) | 17 | Round trips; embedded NULs survive; unknown states, damaged lists, truncation, a version mismatch, trailing bytes and out-of-range values are rejected rather than partially decoded; a corrupt count does not size an allocation. |
| | **NEW, 4** | An out-of-range macro kind and an index pointing forward are rejected (`9b86dcc4e`); an out-of-range name space is rejected, and `fingerprintSatisfies`: a declaration is satisfied by a definition, not the reverse, and type, asm label and kind must match (`9f46203f6`). The full round trip covers every new field. |
| `CacheProtocolTest.cpp` (405) | 22 | Message framing and daemon behaviour: names and effects kept in the region, a clean miss, every candidate returned, duplicates discarded but the same metadata with another module name kept, stored bytes come back exactly, garbage closes the connection, the ask-list. |
| `EndToEndTest.cpp` (350) | 7 | Push through a real socket, then reject the entry on each kind of mismatch in turn, and choose correctly between candidates. |
| `ProducerTest.cpp` (434) | 14 | The separate producer: context, effects, pragma state, nested headers, module names, `#include_next`, `__COUNTER__`, job files. |
| `LoadTest.cpp` (216) | 1 | A PCM loads from bytes in memory partway through a compile, and its declarations work with that compile's own types. |
| `HitTest.cpp` (1259) | 41 | Served without the file being opened; not served inside a function body; a mismatching record is not used; every syntactic case; effects (`#undef`, later definition wins, guards and `#pragma once`, the counter); the ask-list; nested headers; the region check; a copy loaded once; a macro compared by its text. |
| | **NEW (`9b86dcc4e`), 3** | A header using `__FILE__` is served under the name it was built by and read under another; the same for a file beside the header; a copy built with `-Ia` is served under `-Ia` and read under `-Ib`, where `<cfg.h>` finds another file. |
| | **NEW (`9f46203f6`), 3** | `ExportTest`: a miss exports parts the next compile serves, a static inline function and a macro among them; a part is not served where an outside `struct pt` has other members; a part holding `./use.h` is not served to a compile that opens `/…/use.h`. |
| | **NEW (`0afcadce2`), 5** | `ExportTest`: a header is exported after the compile loads a part, and served next; a header with a served header inside is not exported; a part whose header defines a struct declared before it is served; a part is not served where an outside enumerator has another value, or where an outside struct is `packed`. |
| `PartExportTest.cpp` (372) | 5 | **NEW.** Through the experiment's variables (§7). A part holding a function body, a macro and a struct whose type comes from an expansion before the header serves the compile it came from (`9b86dcc4e`); a part written with deep copies holds the outside declaration it uses but not the outside file's macros (`9b86dcc4e`). Loaded, a part's declarations use the compile's own `outside_t`, `struct pt`, field `x` and enumerator `RED`; with deep copies, `outside_t` is a copy; in both modes, the main file's definition of a function the header declared stays out of the part (`0afcadce2`). |

---

## 9. Results on redis

redis is the first benchmark: a C key-value server, about 100 `.c` files.
`anet.c` is its networking helpers; `object.c` its value objects, which
includes the large `server.h`. All runs compare each compile's assembly with
a plain compile's (`-O2 -S`, and with `-g` where noted).

**At `2a4866313`, with the separate producer** (2026-10-01, six files on one
daemon): `anet.c` 18/18 hits, `sds.c` 9/9, `zmalloc.c` 12/12, `util.c` 23/23,
all identical. `ae.c` compiled but differed in one string, the `__FILE__` of
`ae_epoll.c`. `object.c` did not compile: copies built after different `.c`
files held conflicting `fd_set`s in their hidden context.

**NEW (`9b86dcc4e`):** with the `__FILE__` check, `ae.c` gives identical
assembly, its `ae_epoll.c` copy correctly rejected. The export experiment:
seven headers each exported from its own `.c` file and served back, with and
without `-g`: all 14 compiles identical.

**NEW (`9f46203f6`):** with export at a miss
(`scratch/e2e/export_e2e.py`: pass 1 times the headers, pass 2 populates,
pass 3 serves), all 18 compiles identical, `object.c` included. Pass 3: `anet.c`
21/23, `sds.c` 16/20, `zmalloc.c` 22/26, `util.c` 34/40, `ae.c` 12/12,
`object.c` 6/6.

**NEW (`0afcadce2`):** by name and with deep copies, each with and without
`-g`: all 72 compiles identical, and pass 3 hit every cache query in both
modes. The gain is in the first build, where by name a compile keeps
exporting after its first hit:

| File | Pass 1 hits, by name | Pass 1 hits, deep copies |
|---|---|---|
| `zmalloc.c` | 88 / 189 | 18 / 202 |
| `util.c` | 118 / 187 | 16 / 221 |
| `ae.c` | 104 / 154 | 15 / 179 |
| `object.c` | 158 / 279 | 18 / 307 |

Parts by name are 3% smaller (25.06 MB against 25.91 MB for the same 421
parts). Loading them is no faster.

**NEW (`f05e79584`): speed, with a release build** (`build_rel`, no
assertions; medians of 21 interleaved runs, in ms):

| File | `-O2 -S`: plain | served |
|---|---|---|
| `anet.c` | 121.1 | 118.0 |
| `ae.c` | 115.8 | 107.9 |
| `zmalloc.c` | 167.5 | 159.6 |
| `util.c` | 274.2 | 247.3 |

With `-fsyntax-only`, `object.c` goes from 114 to 54 ms. The cache's own time
per compile is 10.5 to 12.8 ms, most of it checking candidates, and most of
that the repeated header searches (§6.4). These timings do not include a
check that each file's contents are unchanged since its part was saved,
which a fair comparison needs; it is not built (§10).

---

## 10. What does not exist yet

1. **A check that a file's contents did not change** since its part was
   saved. A hit never checks it; a plain compile reads every file. Proposed
   2026-10-05: compare each file's size and modification time, saved at
   export. Not approved yet.
2. **Cheaper header-search checks.** Proposed 2026-10-05, not approved:
   compare directory modification times instead of redoing every search, or
   check each file and directory once per build in the daemon.
3. **Cache queries without the socket**: the daemon's table in the shared
   region. Proposed, not approved.
4. **Cross-reference IDs** for outside declarations, the third way to refer
   to one (the user: "keep both capabilities"). Needs a writer chained to the
   loaded module files. Not started.
5. **A whole-build run**: all of redis, with compiles in parallel (`make -j`)
   on one daemon. The daemon has a thread per connection, but every run so far
   compiled one file at a time.
6. **The `__COUNTER__` dependency.** Decided: the counter's starting value as
   a dependency and its advance as an effect (DECISIONS 2.7). A header that
   uses it is still not saved.
7. **Daemon persistence and eviction.** A restart empties the cache; a full
   region drops new pushes.
8. **Folding by parse time** (DECISIONS 2.15): the 226 µs cutoff is decided;
   the mechanism is not approved.
9. **Removing the separate producer**, job recording and the populate tool.
10. **A served file is not registered under its name** (DECISIONS §3): if the
    same file is later included as text under another name, `__FILE__` there
    gives that other name, where a textual compile would give the first.
11. **The token tier's server half.**

Every gap in the metadata so far was found by a hand-built counterexample,
not by a method, and no test reports "this record is missing something".

---

## 11. Corrections made along the way

Kept because each one is a mistake that could recur.

- **"Sound by construction" was false.** Ten adversarial attacks on the
  provenance check produced ten silently-corrupting counterexamples.
  Soundness holds only where the recorder actually observes the relevant
  event.
- **Cacheability numbers measured the producer, not the headers.**
- **The producer read freed memory** after `ExecuteAction` returned; results
  are now copied out during `HandleTranslationUnit`.
- **Replaying context by absolute path broke `#include_next`** (§5.1).
- **The declaration-boundary check rejected almost every `#include`** (§3.3).
- **The first redis run missed every cache query**: the producer stored
  absolute paths and redis names `anet.h` relatively.
- **Syntactic names were first collected at the wrong hook**, which missed a
  name coming out of an outside macro's body (§3.5).
- **A stale macro bit let the conditional check pass wrongly** after an
  earlier hit (§6.4).
- **"No benchmark header uses `__COUNTER__`" was wrong**: 21% of qemu's and
  35% of the kernel's direct includes use it through a macro.
- **A module load reset `__COUNTER__`** (§6.6); **the daemon dropped a copy
  built with other options as a duplicate** (§6.3); **a compile mapped the
  wrong region** (§6.4); **literal text compared as "a number"** (§4.3).
- **NEW: "`__FILE__` is the same in every compile" was wrong** (found
  2026-10-01, fixed in `9b86dcc4e`, §4.1).
- **NEW: the include path was not checked** (`9b86dcc4e`, §4.1).
- **NEW: the PCM writer made file names absolute**, so `-g` named `ae.h`
  `/home/.../src/ae.h` where a plain compile says `./ae.h` (`9b86dcc4e`,
  §5.3).
- **NEW: the first hit ended a compile's exports** (`9f46203f6`): a part
  could not refer to a loaded declaration. On `anet.c`, 171 headers were
  declined after the compile hit its own `stddef.h` part. Lifted by
  references by name (`0afcadce2`).
- **NEW: deep copies could carry the main file's definitions**
  (`SDS_NOINIT`), found by reading the writer's redeclaration code and fixed
  in `0afcadce2` (§5.3).
- **NEW: a fingerprint included the header's own definition** (`bsearch`),
  fixed in `0afcadce2` (§4.6).
- **NEW: every part required `FILE`**, through the special types; fixed in
  `0afcadce2` (§5.3).
- **NEW: three bugs that only appear when exporting after a load**, fixed in
  `0afcadce2`: a header served inside the exported one was not noticed when
  its part was already loaded; the region's own `#define` was dropped when a
  later header replayed the macro; a name chain could list none of the part's
  declarations of `size_t` (§5.2, §5.3).
- **NEW: "served compiles are 4 to 5 ms slower at `-O2`" was the benchmark's
  own fault** (`f05e79584`): it filled the cache with `-fsyntax-only`, so its
  `-O2` compiles missed every header that tests `__OPTIMIZE__`.

---

## 12. Where to look next

- `DECISIONS.md`: why each choice was made, and what was rejected. 2.17 and
  2.18 cover everything new here.
- `NEXT.md`: the worklist.
- `README.md`: how to run the daemon and the variables.

---
## 13. Commit map

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
| [`7b5230e84`](https://github.com/SikangSun/llvm-include-cache/commit/7b5230e84947dd5c9d94b83db0d007c9442a6a67) | [clang] Header cache: walkthrough covers effects, ask-list, nested headers |
| [`9b86dcc4e`](https://github.com/SikangSun/llvm-include-cache/commit/9b86dcc4ed77e1869b83485f53f73954898975a9) | [clang] Header cache: check __FILE__ and the include path; export a header's part (experiment) |
| [`7c820ad9c`](https://github.com/SikangSun/llvm-include-cache/commit/7c820ad9ceb366012f721d156efd8c27563f189f) | [clang] Header cache: walkthrough marks each update's additions; covers 9b86dcc4e |
| [`9f46203f6`](https://github.com/SikangSun/llvm-include-cache/commit/9f46203f6e482c1ba9dfcdc3bfdb3252950e7d8c) | [clang] Header cache: export a header's part at a miss (first debugging version) |
| [`aeee051cc`](https://github.com/SikangSun/llvm-include-cache/commit/aeee051cceec385f62c3da35ce264e58b05122b5) | [clang] Header cache: walkthrough covers export at a miss (9f46203f6) |
| [`0afcadce2`](https://github.com/SikangSun/llvm-include-cache/commit/0afcadce281bfd348e97710320397de048048aa8) | [clang] Header cache: a part refers to outside declarations by name |
| [`5b241c272`](https://github.com/SikangSun/llvm-include-cache/commit/5b241c27257fb2a7bad784ff68eb5339fbe37da3) | [clang] Header cache: walkthrough covers references by name (0afcadce2) |
| [`f05e79584`](https://github.com/SikangSun/llvm-include-cache/commit/f05e7958451f6f98bc02415483b0e5d451f34c2f) | [clang] Header cache: print where the hook's time goes |

Which commits changed each file, oldest first:

| File | Commits |
|---|---|
| `clang/include/clang/ASTCache/ASTCacheDependencyTracker.h` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` |
| `clang/include/clang/ASTCache/ASTCacheProducer.h` | `91182dc08` `a5a384e19` `ea53c042d` `2a4866313` `9f46203f6` |
| `clang/include/clang/ASTCache/ByteStream.h` | `91182dc08` `1291faa49` |
| `clang/include/clang/ASTCache/CacheClient.h` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` `f05e79584` |
| `clang/include/clang/ASTCache/CacheDaemon.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/CacheProtocol.h` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/include/clang/ASTCache/DeclFingerprint.h` | `9f46203f6` `0afcadce2` |
| `clang/include/clang/ASTCache/DependencyRecord.h` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` |
| `clang/include/clang/ASTCache/DependencySerialization.h` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` |
| `clang/include/clang/ASTCache/HeaderCacheServer.h` | `ab65051ab` `2a4866313` `9f46203f6` this commit |
| `clang/include/clang/ASTCache/MacroTokens.h` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` |
| `clang/include/clang/ASTCache/PartExporter.h` | `9f46203f6` `0afcadce2` |
| `clang/include/clang/ASTCache/PragmaStateCheck.h` | `91182dc08` |
| `clang/include/clang/ASTCache/SharedRegion.h` | `91182dc08` `2a4866313` |
| `clang/include/clang/Frontend/CompilerInstance.h` | `ab65051ab` |
| `clang/include/clang/Lex/DeclBoundaryObserver.h` | `91182dc08` |
| `clang/include/clang/Lex/ExpansionCandidateObserver.h` | `1291faa49` |
| `clang/include/clang/Lex/HeaderCacheHook.h` | `ab65051ab` `9b86dcc4e` |
| `clang/include/clang/Lex/PPCallbacks.h` | `91182dc08` |
| `clang/include/clang/Lex/Preprocessor.h` | `91182dc08` `ab65051ab` `1291faa49` |
| `clang/include/clang/Lex/TransitiveClosureTracker.h` | `91182dc08` |
| `clang/include/clang/Parse/Parser.h` | `91182dc08` |
| `clang/include/clang/Parse/RAIIObjectsForParser.h` | `91182dc08` `ab65051ab` |
| `clang/include/clang/Sema/NameClassificationObserver.h` | `91182dc08` |
| `clang/include/clang/Sema/Sema.h` | `91182dc08` |
| `clang/include/clang/Serialization/ASTBitCodes.h` | `0afcadce2` |
| `clang/include/clang/Serialization/ASTReader.h` | `0afcadce2` |
| `clang/include/clang/Serialization/ASTWriter.h` | `9b86dcc4e` `9f46203f6` `0afcadce2` |
| `clang/lib/ASTCache/ASTCacheDependencyTracker.cpp` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` |
| `clang/lib/ASTCache/ASTCacheProducer.cpp` | `91182dc08` `a5a384e19` `ea53c042d` `f976d8fd4` `95232c6b6` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` |
| `clang/lib/ASTCache/CMakeLists.txt` | `91182dc08` `ab65051ab` `9f46203f6` |
| `clang/lib/ASTCache/CacheClient.cpp` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` `0afcadce2` `f05e79584` this commit |
| `clang/lib/ASTCache/CacheDaemon.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/CacheProtocol.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/lib/ASTCache/DECISIONS.md` | `91182dc08` `a5a384e19` `ea53c042d` `f976d8fd4` `95232c6b6` `ab65051ab` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` `0afcadce2` `f05e79584` |
| `clang/lib/ASTCache/DeclFingerprint.cpp` | `9f46203f6` `0afcadce2` |
| `clang/lib/ASTCache/DependencySerialization.cpp` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` |
| `clang/lib/ASTCache/FORMALIZATION.md` | `26a28d8ea` |
| `clang/lib/ASTCache/HeaderCacheServer.cpp` | `ab65051ab` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` `0afcadce2` `f05e79584` this commit |
| `clang/lib/ASTCache/MacroTokens.cpp` | `91182dc08` `2a4866313` `9b86dcc4e` |
| `clang/lib/ASTCache/NEXT.md` | `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` `0afcadce2` `f05e79584` |
| `clang/lib/ASTCache/PartExporter.cpp` | `9f46203f6` `0afcadce2` |
| `clang/lib/ASTCache/PragmaStateCheck.cpp` | `91182dc08` |
| `clang/lib/ASTCache/README.md` | `91182dc08` `a5a384e19` `ea53c042d` `2a4866313` `9b86dcc4e` `9f46203f6` `0afcadce2` `f05e79584` |
| `clang/lib/ASTCache/SharedRegion.cpp` | `91182dc08` `2a4866313` |
| `clang/lib/ASTCache/WALKTHROUGH.md` | `7032a4b76` `6996c31c7` `880757f46` `7b5230e84` `7c820ad9c` `aeee051cc` `5b241c272` this commit |
| `clang/lib/CMakeLists.txt` | `91182dc08` |
| `clang/lib/Frontend/CompilerInstance.cpp` | `ab65051ab` |
| `clang/lib/Lex/CMakeLists.txt` | `91182dc08` |
| `clang/lib/Lex/Lexer.cpp` | `1291faa49` |
| `clang/lib/Lex/PPDirectives.cpp` | `91182dc08` `ab65051ab` `9b86dcc4e` |
| `clang/lib/Lex/PPExpressions.cpp` | `91182dc08` |
| `clang/lib/Lex/PPMacroExpansion.cpp` | `1291faa49` |
| `clang/lib/Lex/Preprocessor.cpp` | `91182dc08` `ab65051ab` `1291faa49` |
| `clang/lib/Lex/TokenLexer.cpp` | `1291faa49` |
| `clang/lib/Lex/TransitiveClosureTracker.cpp` | `91182dc08` |
| `clang/lib/Parse/ParseDecl.cpp` | `91182dc08` `ab65051ab` |
| `clang/lib/Parse/Parser.cpp` | `91182dc08` |
| `clang/lib/Sema/SemaDecl.cpp` | `91182dc08` |
| `clang/lib/Sema/SemaTemplate.cpp` | `91182dc08` |
| `clang/lib/Serialization/ASTReader.cpp` | `0afcadce2` |
| `clang/lib/Serialization/ASTReaderDecl.cpp` | `0afcadce2` |
| `clang/lib/Serialization/ASTWriter.cpp` | `9b86dcc4e` `9f46203f6` `0afcadce2` |
| `clang/lib/Serialization/ASTWriterDecl.cpp` | `0afcadce2` |
| `clang/tools/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-daemon/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-daemon/ClangHeaderCacheDaemon.cpp` | `91182dc08` `2a4866313` |
| `clang/tools/clang-header-cache-populate/CMakeLists.txt` | `91182dc08` |
| `clang/tools/clang-header-cache-populate/ClangHeaderCachePopulate.cpp` | `91182dc08` `a5a384e19` `ea53c042d` `1291faa49` `2a4866313` |
| `clang/tools/driver/CMakeLists.txt` | `ab65051ab` |
| `clang/tools/driver/cc1_main.cpp` | `ab65051ab` |
| `clang/unittests/ASTCache/ASTCacheDependencyTrackerTest.cpp` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` |
| `clang/unittests/ASTCache/CMakeLists.txt` | `91182dc08` `a5a384e19` `f976d8fd4` `ab65051ab` `9b86dcc4e` |
| `clang/unittests/ASTCache/CacheProtocolTest.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/unittests/ASTCache/DependencySerializationTest.cpp` | `91182dc08` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` |
| `clang/unittests/ASTCache/EndToEndTest.cpp` | `91182dc08` `1291faa49` `2a4866313` |
| `clang/unittests/ASTCache/HitTest.cpp` | `ab65051ab` `1291faa49` `2a4866313` `9b86dcc4e` `9f46203f6` `0afcadce2` |
| `clang/unittests/ASTCache/LoadTest.cpp` | `f976d8fd4` `95232c6b6` |
| `clang/unittests/ASTCache/PartExportTest.cpp` | `9b86dcc4e` `0afcadce2` |
| `clang/unittests/ASTCache/ProducerTest.cpp` | `a5a384e19` `ea53c042d` `f976d8fd4` `2a4866313` |
| `clang/unittests/ASTCache/SymbolDependencyTest.cpp` | `91182dc08` `1291faa49` |
| `clang/unittests/CMakeLists.txt` | `91182dc08` |
