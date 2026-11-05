# Bothner's compile server in Clang: design

Written 2026-11-01 for review, on `bothner-model`; revised the same day after
three independent reviews: Clang mechanics, fidelity to the paper,
evaluation. S0, the model's prediction of the server, ran on 2026-11-03; its
results are in section 6 and the decisions it raises in section 8. Nothing
of the server is built yet.

The real server replaces the model of COMPILE_SERVER_PLAN.md as the
baseline. The model stays for two jobs:
- it predicts the server's decisions before any of the server is built
  (step S0);
- its rows are estimates beside the server's measured ones.

Source: Bothner, "GCC Compile Server", GCC Developers' Summit 2003,
pp. 21-34 ("p." below).

Evidence:
- code references are to this checkout;
- the Clang facts were read by one reader and re-checked by a second;
- the source-location figures were measured on 2026-11-01 with a one-off
  script, which S1 checks in and reruns.

## 0. Goal and scope

**What is built.** A long-lived Clang process that does what his server
did:
- it serves compiles one at a time;
- it keeps every header fragment it parses;
- at each fragment start, it either restores the stored declarations and
  skips the text, or parses it.

**How it is judged.** Wall time on this machine, beside plain clang and the
header cache, in one harness.

**Flags.** Each compile's recorded flags. The cache adds
`-fmodules -fno-implicit-module-maps`, which it needs; plain clang and the
server do not.

**Correct.** Each compile through the server yields the same results as
plain clang, judged as strictly as the cache is (section 5):
- the same LLVM IR for the compile, in a validation mode;
- the same declarations, layouts and attributes;
- no assertion failures in an assertions build.

**In scope:**
- redis's 187 compiles, front end only (`-fsyntax-only`), C;
- option 2 fragments first, any directive ends one (his prototype, p.26);
- then option 3 and fragment groups (2.7): his own fixes for the largest
  loss;
- first build and recompilation;
- one core, then `-j4`.

**Out of scope:**
- objects (code generation) outside the validation mode;
- the other five projects. Their build logs are not on this machine, and
  each needs its own address-space measurement first (2.2).

## 1. His server, and ours

| His (page) | Ours |
|---|---|
| A compiler process in a loop, serving one request at a time over a Unix socket (p.21-23) | The same: a plugin action in a long-lived `clang -cc1`, with a serial accept loop (2.1) |
| Commands F/T/I/S/O: flags fixed when the server starts; I invalidates edited files (p.23) | A request carries cwd, arguments and a build ID. Each configuration's parsed invocation is cached, so its flags are effectively fixed. A header whose size or mtime changed between builds loses its stored fragments, which stands in for I |
| The gcc driver is the client, `make CC='gcc --server'`; diagnostics should go back over the socket (p.21-23) | The clang driver forwards its cc1 job; the diagnostics come back in the reply (2.1) |
| One working directory and one flag set per server (p.22) | One state per configuration in one process, keyed by the full invocation (2.1) |
| Three levels of initialization: once, per request, per input file; builtins declared once (p.23-24) | Once per configuration, library builtins included; per compile, the resets of 2.3 |
| The preprocessor keeps header text across compiles (p.24) | The persistent FileManager and SourceManager keep it |
| Fragments between directives (option 2, built); `#define` and `#undef` inside fragments (option 3, "may be the best approach", p.26-27) | Option 2 first, then option 3 (2.7) |
| enter/exit_fragment: on reuse, restore and skip to the next directive (p.26-27) | A preprocessor hook; the skip is a seek; the restore is queued to the parser (2.4, 2.5) |
| Depends-on sets of fragments and the p.28 timestamp rule; a fragment already processed in this compile is re-parsed, which he would rather catch as an error (p.27-28) | Versions: every parse makes a new version, and uses are checked as (fragment, version). This is equivalent to his timestamps with one stored version per location. Repeats are parsed (2.6) |
| §7.2: a tag first declared in a parameter list; other negative dependencies left open (p.28-29) | Never reused; negative lookups are checked in general (2.6) |
| §8: no macro checks in the prototype; a per-fragment not-a-macro list, rejected as "quite expensive"; §8.2's two bits per identifier, proposed (p.29-30) | V1, our own check: the fragment's macro list compared by value (more reuse than §8.2, and a cost per reuse). §8.2 as a variant (2.6) |
| §9: bindings saved per fragment and restored; tags and struct fields and sizes re-set (p.30-31) | Bindings re-pushed into the new compile's file scope (2.5). Tag completeness comes from repairing the redeclaration chain (2.8) |
| §10: an undo buffer for in-place changes, started (p.31-32) | Chain repair, snapshots and resets (2.3, 2.8) |
| §11.1: an enum with `#define` lines; fixed by option 3 (p.32) | Option 3 (2.7) |
| §11.2: `currently_nested`; fragment groups proposed (p.32-33) | The fork's DeclBoundaryObserver, fixed (2.9); fragment groups (2.7) |
| §11.4: `size_t` in several headers loses reuse, "only its performance" (p.33) | The negative-lookup and predecessor checks can turn this loss into refusals; counted (2.6) |
| §12: about 3x on Carbon and Octave, from a prototype with no macro checks and no undo (p.34) | Not comparable; ours is checked for equal results |

### Which way our departures lean

| Departure | Effect on his numbers |
|---|---|
| V1 by value instead of §8.2's bits | Faster: more reuse. It costs about 13 comparisons per reuse, which are timed |
| Correctness checks his design also needs: pragma state, negative lookups, side-effecting builtins, file identity | Slower than his prototype, needed for equal results; refusals counted per check |
| Checks only Clang needs: deps by version, predecessor (redeclaration chains) | Slower; reported apart as Clang's cost, not his |
| Declining while a parser pragma is pending (Clang defers these pragmas to the parser) | Slower; none in redis's headers |
| A new Preprocessor and HeaderSearch per compile (his `cpp_reader` lived on) | Slower: include lookups are redone. File stats and text stay cached |
| Option 2 only, until option 3 and groups are built | Slower; both rows are reported |
| Groups that hold `#define` lines (decision 6) | Faster: 2.17x against 1.66x in S0; §11.2's groups reported beside |
| Per-reading keys (decision 8) | Faster: repeats become reusable; a further row |
| Token names as uses (decision 9) | Slower: 0.08x in S0, from names in other roles; covers uses the AST drops |
| Per-compile resets | As his level-3 initialization (p.24). Reported as the server's own overhead |
| Restart after about seven passes | Not reached: two passes per server (4) |
| Serial server, header text in memory, diagnostics over the socket | As his |

## 2. Design

### 2.1 Processes and requests

- **The server is a plugin,** `BothnerServer.so`. `build.sh` builds it in
  seconds, and it runs as `clang -cc1 -load BothnerServer.so -plugin
  bothner-server`. The clang binary exports every symbol needed
  (`-Wl,--export-dynamic`). Its accept loop is serial: requests wait in the
  listen backlog.
- **Why each compile is hand-driven:**
  - `createPreprocessor` takes no IdentifierInfoLookup
    (CompilerInstance.cpp:457);
  - `BeginSourceFile` creates a fresh ASTContext and sets builtin IDs on the
    Preprocessor's own table (FrontendAction.cpp:1265);
  - `ExecuteAction` clears the SourceManager's ID tables for each input
    (CompilerInstance.cpp:1003-1006).
- **Transport.** The header cache's framing: a 4-byte length, then the body,
  through `sendMessage`/`receiveMessage` and `ByteWriter`/`ByteReader`, with
  two new message kinds.
  - A request carries cwd, the cc1 arguments and the build ID.
  - A reply carries the exit status, the diagnostics, and per-compile counts
    and costs (4).
- **Client.** When `CLANG_COMPILE_SERVER` names a socket, the driver's
  in-process cc1 entry (`clang/tools/driver/driver.cpp` near 378) sends the
  job instead of running it. That is one object plus a relink.
  - The driver still runs, as his did.
  - If the server is unreachable or dies, the driver runs cc1 locally and
    says so. The harness then treats the run as invalid.
  - The harness filters `CLANG_COMPILE_SERVER` out of plain runs, and checks
    the server's request count against the runs it expected to forward.
- **Configurations.** A configuration is the cwd plus the full parsed
  invocation: every LangOption including the 78 "benign" ones that
  `getModuleHash` skips, `NoBuiltinFuncs`, and TargetOptions. Each has its
  own state in the one process; equal configurations have equal LangOptions,
  which the ASTContext requires. The parsed invocation is cached per
  (cwd, arguments minus input and output), so argument processing is not
  repeated per request. Under the model's configuration identity redis has 8
  configurations, with 1, 1, 1, 2, 7, 12, 38 and 125 compiles. The full key
  may split them further; S1 reports the count.
- **Environment.** The server must not see `CLANG_HEADER_CACHE`, and installs
  no cache hook on its compiles.
- **Accounting.** All per-compile work, including resets, teardown and
  recording, finishes between receiving a request and replying.

### 2.2 What lives for the server, what lives for one compile

| Lifetime | Objects |
|---|---|
| Configuration | FileManager, SourceManager, TargetInfo, LangOptions, DiagnosticsEngine; the IdentifierTable, SelectorTable and Builtin::Context, owned by the server; the ASTContext; library builtin declarations |
| Compile | HeaderSearch, Preprocessor, Sema, Parser, consumer, a new TranslationUnitDecl, the main FileID |

- **The three tables are the server's.** A Preprocessor normally owns them,
  and the ASTContext only refers to them (Preprocessor.h:280-294,
  ASTContext.h:790-792). The server initializes the Builtin::Context for the
  target, then calls `initializeBuiltins` once on the persistent table.
- **Builtins are declared once.** Library builtins (`memcpy` and the like)
  are kept from their first declaration, as his were declared "initially".
  Otherwise each compile creates its own, and restored prototypes chain to
  another compile's.
- **Each compile's Preprocessor shares the identifiers.** It gets an
  IdentifierInfoLookup that always returns the persistent table's entry,
  never null (IdentifierTable.h:748-769).
- **Translation units.** Each compile starts with `addTranslationUnitDecl()`
  and then `setTraversalScope`. The first TU is every TU's primary context,
  so a restored declaration of an older TU counts as in scope.
- **TU kinds.** The ASTContext is built `TU_Incremental`; the Preprocessor
  and Sema are `TU_Complete`. Incremental processing stays off: in C it stops
  scopes from removing block-scope names.
- **No persistent root FileID.** Hanging every main file under one root, as
  clang-repl does, would turn off the main-file checks of every `.c` file
  (`isInMainFile`, SourceManager.cpp:1528). Instead:
  - each main file is an ordinary main FileID;
  - `SourceManager::isBeforeInTranslationUnit`, when two locations have no
    common include ancestor ("Unsortable locations", undefined behaviour in
    this build), orders them by compile instead. That is one object.
  - Two C paths reach that case: sorting `overloadable` candidates, and
    `DeclPrinter` placing an inherited attribute.
- **Address space.** Source locations are 32-bit: 2 GiB per SourceManager,
  never reclaimed, and running out is fatal. Measured:
  - redis takes 296 MB a pass, 85% of it header text that every compile
    enters again;
  - so a fill plus a recompilation (593 MB) fits;
  - 4 makes every repetition a fresh server, so no server exceeds two
    passes.
- **Memory.** The ASTContext never frees. Inferred: at most about 0.4 GB of
  AST a pass with no reuse. Each compile alone used at most 86 MB
  (measured).

### 2.3 Per-compile resets

What resets is the compile instance, not the server: the server process and
its store of fragments live on across compiles. Before each compile, the
state the previous compile's instance left in shared objects is cleared:

| What | Why | How |
|---|---|---|
| FETokenInfo of every identifier | The previous Sema's file-scope bindings live there. A name with one declaration keeps it, so the old compile's declarations stay visible; the rest point into freed memory (IdentifierResolver.cpp:92-97, 160-170; Parser.cpp:466-469) | Loop over the persistent table: `setFETokenInfo(nullptr)` |
| HasMacro, poisoned, deprecated, restrict-expansion, final, RevertedTokenID | `isMacroDefined` reads HasMacro, not the macro table | The same loop, with public setters |
| Implicit typedefs: `__int128_t`, `__uint128_t`, `__NSConstantString`, `__builtin_ms_va_list`, `__builtin_va_list` | Sema::Initialize adds the cached ones to the new TU's declaration list, which splices the old and new lists (Sema.cpp:395-438, 577-584) | Push them with AddToContext=false from `SemaConsumer::InitializeSema`, which runs first (Parser.cpp:489-490, Sema.cpp:379-380) |
| ASTContext's FILE, jmp_buf, sigjmp_buf, ucontext_t | Set by parsing the typedef; they decide how builtins are typed | Null at compile start; set again when such a typedef is restored |
| ExternCContextDecl's lookup | Block-scope externs and implicit declarations of any compile leak into later ones | Clear its map each compile; re-register restored ones |
| Memoized type info (`MemoizedTypeInfo`, `MemoizedUnadjustedAlign`) | Keyed by a canonical type shared through a restored forward declaration, so another compile's struct layout would answer `sizeof` | Clear both each compile (private: same access as 2.8) |
| DiagnosticsEngine | Counts and `#pragma diagnostic` state | `Reset(false)`, then `ProcessWarningOptions` (not `ResetPragmas`) |
| Redeclaration chains, attributes, Referenced bits of restored declarations | 2.8 | 2.8 |

Left alone:
- builtin IDs, which nothing changes;
- keywords, set again by each Preprocessor;
- the `__va_list_tag` record, one per server;
- the SourceManager's caches;
- the first TU's lookup table, which C never builds.

### 2.4 Skipping a fragment

- **Two hook sites see every fragment start once, before its first token is
  lexed:**
  - every return from `Preprocessor::HandleDirective`, through a scope guard
    at its top (PPDirectives.cpp). This covers every directive, false
    conditionals, and includes entered, skipped or served;
  - `HandleEndOfFile`, after its ExitFile callbacks and just before
    `return LeavingSubmodule` (PPLexerChange.cpp:527), where the includer
    resumes. Placed any earlier, a pushed token would suppress the ExitFile
    callbacks Sema relies on.
- **The interface.**
  - The Preprocessor asks the hook with the FileID and offset.
  - The hook answers with an end offset, whether the fragment had tokens,
    and whether it expanded macros.
  - The Preprocessor then seeks, updates MultipleIncludeOpt, and pushes the
    restore token (2.5). MultipleIncludeOpt, `InMacroArgs` and the lexer are
    private to the Preprocessor.
  - The hook is a new virtual on HeaderCacheHook. It is called only when the
    installed hook asks for fragments, so the cache's compiles pay nothing.
    That rebuilds 3-4 objects.
- **The Preprocessor declines:**
  - in the main file (`isInPrimaryFile()`);
  - in buffers without a file (the predefines);
  - while macro arguments are being collected.
- **The server declines while a parser pragma is pending.** Clang defers
  `#pragma pack`, `weak`, `visibility`, the floating-point pragmas, and
  others to the parser, and some handlers consume their token before they
  act.
  - The Preprocessor marks the hook when a pragma handler queues such a
    token. That happens inside the `#pragma` directive, so the hook at the
    directive's own end already sees the mark: the parser does not have the
    token yet.
  - It clears the mark when the next token request after the token was
    handed out has returned.
  - S0 found the first version of this rule, which set the mark only at the
    hand-out, missing the fragment right after every such `#pragma` line.
- **The server declines a fragment that ends inside macro arguments.** Its
  end directive sits between a function-like macro's `(` and `)`, so a
  reuse would skip the macro's name. A recorded flag, beside the
  Preprocessor's refusal at the start.
- **The skip:** `Lexer::seek(end, /*IsAtStartOfLine=*/true)`. If the fragment
  had tokens, the Preprocessor also tells MultipleIncludeOpt that a token
  was read, and that a macro was expanded if one was. Otherwise tokens after
  an include guard's `#endif` go unseen, and the header is wrongly treated
  as guarded.
- **Never reusable:** fragments that use `_Pragma`, `__COUNTER__`,
  `__DATE__`/`__TIME__`/`__TIMESTAMP__`, `__BASE_FILE__` or
  `__INCLUDE_LEVEL__`, or `__FILE__`, `__FILE_NAME__` or `__LINE__` after a
  `#line` in the file. Their effects or values are invisible to the macro
  list. `__FILE__` without `#line` is fixed per key.
  - In an option-3 fragment or a group (2.7), any member counts, even one
    that hands the parser no tokens: a lone `_Pragma("push_macro(...)")`
    line is such a member, and a reuse would skip its push.
- **Keys:** configuration, file identity, and the offset just after the
  directive. File identity is the path as opened plus size and mtime,
  checked once per build ID. The model keys fragments by first-token offset.
  S0 records the post-directive offset beside it; on redis the two map one
  to one, and S0's second simulator, keyed by the post-directive offset,
  makes the same decisions.

### 2.5 Restoring

- **When: not inside the hook.** The parser may still be finishing the
  previous declaration: `FinalizeDeclaratorGroup` runs after the `;` lexed
  the next token, and after a function body's `}` Sema is still in the
  function.
- **How it is queued.**
  - The pushed token is `annot_module_include`.
  - The parser handles it at file scope (Parser.cpp:666), after the previous
    declaration is complete.
  - Sema then calls `ModuleLoader::makeModuleVisible` (SemaModule.cpp:746-768;
    no ImportDecl without `-fmodules`), which the server's loader implements.
  - The token needs one dummy Module per compile, made through
    `ModuleMap::createModule`, plus a queue of fragment ids.
  - The cache serves headers through the same token.
- **The primitive:** `PushOnScopeChains(D, TUScope, /*AddToContext=*/false)`,
  for each stored binding in parse order. As in a parse, it replaces an
  earlier declaration of the same entity in scope. It must be the scope
  push: C's lookup for a redeclaration with linkage skips names outside the
  starting scope.
- **The bindings, recorded when the fragment was parsed:**
  - the declarations added to the TU during the fragment that are in TUScope
    at its end (this drops tags first named in a prototype);
  - enumerators and tags nested in C structs, found by recursing into the
    fragment's records and enums.
- **Also redone:**
  - Sema's TentativeDefinitions, UnusedFileScopedDecls and ExtVectorDecls get
    back the restored declarations that were in them;
  - ASTContext's FILE-like declarations are set again;
  - the macros the fragment expanded are marked used (`-Wunused-macros`);
  - in validation mode, each restored declaration goes to the consumer, so
    CodeGen sees what a parse would have handed it.

### 2.6 The rule

**R0, p.28's versioning with our checks, is what the server runs.**
- Every parse makes a new version.
- A use is valid if its fragment was processed in this compile at the
  version the use recorded.
- Repeats are parsed.

**The checks.** Each is counted when it refuses:

| Check | Why |
|---|---|
| Not nested at start or end (§11.2), not §7.2, not using the main file | His rule |
| Uses at the same versions (p.28) | His rule |
| Macro list (V1) equal by value | Ours (§8.2 as a variant) |
| Pragma state equal: pack, floating point, visibility, ms_struct, align, `#pragma clang attribute`, `clang section`, `clang optimize`, `assume_nonnull`; no pending `weak` or `redefine_extname` | Needed by any correct implementation; the paper is silent |
| Negative lookups: a declaration that had no earlier visible declaration still has none | The general case of §7.2, which he left open (p.29) |
| File identity unchanged; no never-reusable builtin (2.4) | Needed by any correct implementation |
| No parser pragma pending at its start; not ending inside macro arguments (2.4) | Needed by any correct implementation |
| A group's conditionals take the same branches (2.7) | His (§11.2) |
| Declares no internal or inline function that it leaves undefined (2.8) | Ours: Used bits leak |
| Deps: the earlier declarations of what it redeclares or uses are at the same versions | Clang only: redeclaration chains |
| Predecessor: each restored declaration's stored predecessor is the visible latest | Clang only: a restore must not hide a newer declaration of the same entity |

**Checks look up names without Sema's lookup.** They walk IdResolver and keep
only declarations in TUScope. After a function body's `}` the function's
locals are still bound, and `LookupName` would find them.

**No R1 in the server.** The model's R1 lets a re-parse whose result is
identical keep its version. That is where the model's gain over R0 comes
from: time reused rises from 62.1% to 76.2%, and 1.66x to 1.93x.
- Almost all of it comes from nested fragments, re-parsed in every compile
  (37.3% of instances).
- A real server cannot do the same safely. A re-parse makes new objects,
  while dependents stored earlier still point at the old ones. Two
  definitions of `struct timestamp` would then meet in one compile and be
  incompatible types.
- Keeping versions only for identical repeats never fires on redis: all
  8,313 repeats are `bits/mathcalls*.h` read under different macros.
- His own remedies for nesting are option 3 and fragment groups (2.7). They
  turn nested fragments into reusable units instead of re-parses.
- The model's R1 rows stay as estimates of what free object identity would
  add.

**The model mirrors the rule.** Before any server code, simulate.py gets this
exact rule, using records extended for the new checks (S0). The server's
decisions are then checked against it compile by compile. That shows that
two implementations of one rule agree; correctness comes from section 5.

### 2.7 Option 3 and fragment groups

- **Option 3:** `#define` and `#undef` lines join the fragment; only other
  directives end one (p.26-27).
  - Reusing such a fragment replays its macro definitions, in order, into
    the current Preprocessor, built from stored tokens. The header cache
    already replays macros at its hits, so its replay code is reused.
  - The fragment's macro list leaves out the names it defines before using
    them.
  - In the model this cuts nested instances from 37.3% to 5.3% (mostly the
    `bits/confname.h` enums, his §11.1 case).
- **Fragment groups** (§11.2): a maximal run of fragments, joined because the
  parser is nested at the boundary between them, is stored and reused as one
  unit. The unit is guarded by the conditionals inside it, so struct
  definitions with `#if` inside (`struct timestamp`, `struct stat`) can be
  reused. Following §11.2, a group contains no `#define` or `#undef`.
- **Both run on the same R0 rule and checks.** S0 predicts their rows before
  they are built.
- **S0: groups as §11.2 has them miss the structs that matter.** The libc
  structs behind most of the nesting have `#define` lines inside or right
  after them:
  - `struct __pthread_mutex_s` (`bits/struct_mutex.h`) has
    `# define __PTHREAD_MUTEX_HAVE_PREV` inside. With option 2 + groups its
    group stops there, and the refusals its nested fragments start cost
    10.5% of header time;
  - under option 3, the last fragment of `struct _IO_FILE`
    (`bits/types/struct_FILE.h`) also holds the `#define` lines after its
    `};`, so that group stops too.
  - Option 3 + groups: 1.66x, below option 2 + groups (1.72x).
- **Groups with `#define` lines (S0's "groups\*").** A group may hold
  `#define` and `#undef` lines. A reuse replays them in order, as option 3
  does, and the group's macro list leaves out names an earlier member
  defines. This combines his option 3 with §11.2. Option 3 + groups\*: 2.17x,
  with 71 of its 220,792 unit instances still nested (decision 6).

### 2.8 Undo: changes Clang makes in place to stored declarations

- **Redeclaration chains.** A new declaration links behind its chain's latest
  declaration and becomes the latest, in place on the first declaration
  (Decl.h:5339-5355). Definition and completeness queries walk from there.
  Left alone:
  - a struct defined in one compile looks complete in the next;
  - a later definition is reported as a redefinition;
  - attributes leak between compiles.
- **The chain repair.** At each restore, set the first declaration's latest
  pointer to the restored declaration, so that later parsed declarations
  link behind the visible one. Previous pointers never change, so a chain
  becomes a tree whose visible path the latest pointer picks.
  - No ExternalASTSource is involved, so no lazily updated latest is either.
  - The members are protected (Redeclarable.h:181-193). The server reaches
    them through explicit-instantiation access, with no header change; a
    public setter would rebuild the 577 objects that include `Decl.h`.
- **Attributes.** A `#pragma weak` or `redefine_extname` that runs after a
  restored declaration adds to that stored declaration in place
  (SemaDecl.cpp:21010-21056). Each restored declaration's attribute vector is
  snapshotted, and put back at the next compile's start (`setAttrs`).
- **Referenced bits.** `setReferenced(false)` on each restored declaration.
- **Used bits** are a private bit-field with no setter, so they leak.
  - They can hide `-Wunused-function`, `-Wunneeded-internal-declaration` and
    `-Wundefined-internal`/`-Wundefined-inline` on header statics: Sema
    records a first use only once.
  - A unit test pins the effect.
  - A fragment that declares an internal or inline function and does not
    define it within the same fragment is not reused. S0 first read the rule
    per declaration; it then also refused fragments that define the
    function right after its prototype, as `keymeta.h` does.
  - **Alternative, S0's "fallback":** reuse such fragments, and if a compile
    ends with a restored internal or inline function still undefined, undo
    the compile's store changes and run it again without reuse. Only that
    ending can hide a `-Wundefined-internal` or `-Wundefined-inline`.
- **Tentative definitions of incomplete array type** are retyped to `T[1]` at
  the end of a TU. Their type is snapshotted and restored.
- **Merged attributes** are written on the new declaration only; nothing to
  undo.

### 2.9 Nesting

DeclBoundaryObserver (Parser.h:1107-1112) reads "between declarations" in two
places where a declaration is still open:
- after a struct or enum body's `}`;
- between decl-specifiers and declarators.

**The fix:**
- Raise the flag at the first token after ParseTopLevelDecl's annotation
  switch. That covers leading attributes, `__extension__` and K&R parameter
  declarations.
- Lower it just before the terminating `;` is consumed, or at a function
  body's `{`.
- The position after a function body's `}` stays a boundary; the queued
  restore handles it.

**What it rebuilds and changes.**
- `Parser.h` and `ParseDecl.cpp`: about 20 objects.
- **It changes the header cache too,** which uses the observer for its splice
  decision. The cache's object check and hit counts are rerun before and
  after the fix, and both are reported.
- The model's records are rerun with the fix. S0 did: on redis the fix
  changes none of the model's decisions or the server's.

### 2.10 Errors, crashes, restarts

- **A compile with an error stores nothing.** None of its new versions are
  kept; its chain repairs, snapshots and resets are undone at the next
  compile start as usual.
- **A fatal error that ends the process.** For example
  `report_fatal_error`, or address space running out. The driver falls back
  to local cc1, the harness marks the pass invalid, and it is rerun.
- **A changed header** (size or mtime, once per build ID) drops its stored
  fragments, and its FileEntry is reloaded.

## 3. Recording

**Live, in the server, per fragment between two hook calls:**
- the end offset;
- whether it had tokens or expanded macros;
- the macro list, from ExpansionCandidateObserver, with states as definition
  hashes (MacroInfo objects die with their Preprocessor);
- the full pragma state;
- flags: §7.2, uses the main file, never-reusable builtins;
- the bindings (2.5);
- the names looked up and not found (negative lookups).

**Uses, deps and predecessors.** BothnerTrace's UseVisitor runs on each
declaration group in `HandleTopLevelDecl`. Groups are attributed to fragments
by location, because they reach the consumer after the next token, so timing
cannot attribute them. Restored fragments keep their stored record.

**The model's records gained the same fields (S0)** in BothnerTrace's record
mode:
- the full pragma state, at every directive and token;
- each fragment's bindings and references, from which the simulator derives
  negative lookups, deps and predecessors per name;
- never-reusable builtins;
- pending parser pragmas;
- macro-argument collection, at a fragment's start and end;
- internal or inline functions declared and left undefined (2.8);
- the fixed observer's view (2.9), from Sema's context, the parser's
  prototype scope and the tokens around each directive;
- the identifier tokens' names, for the token-name variant and oracle.

## 4. Measurement

All of it runs in one harness, recompile.py extended.

**Sources** are read from the DRAM copy (`SRC_ROOT=/dev/shm/bench-src`) for
every row.

**One core.** Every process of the system under test runs on the compile
core, turbo off:
- the client;
- the server;
- the cache's daemon.

The cache's earlier rows put its daemon on a core of its own; that layout is
kept as a second row. Each server and daemon reports its CPU time per
request (from `/proc/<pid>/stat`), so no work hides between requests.

**Repetitions.** Each repetition is its own fresh server and daemon.
- For recompilation: an untimed fill pass, then one timed pass, in log order.
  In each timed pass, plain, cache and server run compile by compile in
  rotated order, as today.
- For the first build: one timed pass from cold. Server start-up and each
  configuration's initialization are inside the total, with the model's
  phase-0 score beside it.
- Per-pass totals are reported (median and spread over five repetitions),
  not only the sum of per-compile minima.

**Rows:**
- plain clang;
- the cache;
- the server with reuse off. This separates what persistence saves (no
  start-up, warm code, header text kept, stats once per build) from what
  reuse saves. It replaces the model's assumed 1.65 ms credit with a
  measured one;
- the server, option 2;
- the server, option 3;
- the server, option 3 with fragment groups;
- the model's estimate for each server row, using the measured credit.

**Beside the rows:**
- the server's cost per compile, split into resets, checks, restores, chain
  repair and recording;
- peak and final memory per system;
- refusals per check, beside the model's reasons for the same compiles.

**Concurrency, `-j4`:**
- a 4-worker pool replays log order on 4 fixed cores (taskset), with every
  process of the system on them;
- setups:
  - one server, where requests queue (the paper is silent on concurrency);
  - one server per worker, with compile i going to server i mod 4. Zapcc
    ran one server per job (DECISIONS.md §2.24);
  - the cache with one daemon;
- the server logs the order it received requests in, and simulate.py replays
  that order;
- reported: makespan and total CPU time, over five repetitions.

## 5. Correctness

**An assertions build.**
- `build_rel` has no assertions, and `llvm_unreachable` is optimized away.
  So the invariants this design can break are checked nowhere there:
  - a declaration in two lists;
  - a broken chain;
  - unsortable locations.
- A second build of the same commit with assertions, untimed, runs every
  correctness check and unit test.

**IR, as strict as the cache's objects.**
- In validation mode, each compile's AST goes to CodeGen
  (`-emit-llvm -O0 -disable-llvm-passes`).
- The IR is compared with plain clang's, after sorting globals, for all 187
  compiles, fill and recompilation, under every rule and option.

**Declarations.** At each compile's end, every visible file-scope
declaration, sorted by presumed location, with:
- its type;
- completeness, and the location of the definition;
- record layout: size, alignment, field offsets;
- attributes, storage class, linkage, and whether it is a definition.

These are compared with plain clang's. The main file's `-ast-dump` is
compared too, with header locations and pointers normalized.

**Diagnostics** are compared with `-Weverything`. With redis's own flags,
plain clang prints nothing on any of the 187 compiles, so that comparison
could not fail.

**Unit tests:**
- The paper's cases:
  - p.26's D1/D2 under M1/M2, and a guarded header;
  - p.27's `h1.h`;
  - p.29's `BUFSIZ`, `#define int size_t` and `#define i j`;
  - p.30's `#define strcmp strcmp`;
  - p.28's §7.2 case;
  - p.32-33's enum with `#define` lines, and `struct timestamp`;
  - p.33's `stdio.h`/`stdlib.h` order with `size_t`.
- Ours, from the hidden-fragment tests: asm label, `weak`, `pack`.
- Cross-compile:
  - a struct completed in one compile and not in the next;
  - two different definitions, then `_Static_assert(sizeof ...)`;
  - a definition in each of two compiles;
  - a block-scope extern;
  - FILE typedef present, then absent;
  - tokens after an include guard's `#endif`;
  - a directive between decl-specifiers;
  - `#pragma weak` after a restored declaration;
  - `#pragma GCC visibility push` followed by `#include`;
  - a directive right after a function whose parameter is named like a
    header global;
  - a header static declared without a definition, used in two compiles;
  - a header edited between builds: same size with a new mtime, and a new
    size;
  - a directive inside macro arguments;
  - `__COUNTER__`, and `__LINE__` after `#line`.

## 6. Steps

| Step | Work | Done when |
|---|---|---|
| S0 | Extend the records (3); add the server's rule, option 3 and groups to simulate.py; run 4-5 phases | Predicted rows and refusals per check; decisions settle after the fill. **Go/no-go.** Done: go (S0 results, below) |
| S1 | Plugin, per-compile driver, resets, configuration key; assertions build; validation mode; no reuse | Two passes of all 187 compiles in one process: IR and declarations equal plain clang's, no assertion fails; overhead, memory and address space measured. **Go/no-go** |
| S2 | Preprocessor hook, skip, queued restore; observer fix and the cache rerun | Unit tests pass; the cache's objects are still identical |
| S3 | Live recording | Records agree with BothnerTrace's |
| S4 | R0 rule, restore, undo (2.8) | Decisions equal S0's on every compile |
| S5 | Correctness with reuse | Section 5 holds through fill and recompilation |
| S6 | Socket, driver forwarding and fallback; harness (4) | Repetitions are independent; CPU accounted per request |
| S7 | Measurements, option 2, one core | Table |
| S8 | Option 3 with macro replay; fragment groups | S4-S5's checks hold; rows |
| S9 | `-j4` harness and runs | Rows |

**Rebuilds.** These are the only changes inside Clang:
- the hook (`HeaderCacheHook.h`, PPDirectives.cpp, PPLexerChange.cpp,
  Preprocessor.cpp);
- the driver's forwarding (driver.cpp);
- the location order (SourceManager.cpp);
- the observer (Parser.h, ParseDecl.cpp).

They are built with `ninja -j2`. The hook costs the cache nothing; the
observer fix does change the cache (2.9).

### S0 results

**What ran.** All 187 compiles recorded with section 3's fields, then
simulated over five passes, the first a fill, in 20 runs: each option, with
the 2.8 rule or its fallback, with the old observer, with per-reading keys,
and with token names as uses. Plain clang takes 15.61 s. Restoring is free,
and start-up is credited 1.65 ms per compile, as in the model. Passes 2-5
make identical decisions.

**Predicted rows**, all 187 compiles:

| Row | Recompilation | First build | Header time reused |
|---|---|---|---|
| Header cache | 2.69x | | |
| Model R0 V1 / R1 V1 | 1.66x / 1.93x | | 62.1% / 76.2% |
| Server, option 2 | 1.54x | 1.47x | 54.6% |
| Server, option 3 | 1.64x | 1.54x | 57.7% |
| Server, option 2 + groups | 1.72x | 1.63x | 66.0% |
| Server, option 3 + groups (§11.2) | 1.66x | 1.56x | 58.9% |
| Server, option 3 + groups\* (2.7) | 2.17x | 2.00x | 81.3% |
| + per-reading keys | 2.64x | 2.36x | 93.9% |
| + token names as uses | 2.09x | 1.94x | 78.6% |
| + both | 2.52x | 2.27x | 91.2% |

- With replayed `#define` lines saving nothing, option 3 + groups\* gives
  2.04x.
- The 2.8 rule and its fallback give the same rows. The rule flags one
  fragment: hiredis `dict.h` declares ten static functions that a `dict.c`
  included later defines. Under the fallback the uses check refuses it
  instead.
- The old observer gives the same decisions as the fixed one in every run.

**Where the rest goes** (recompilation, share of header time):
- **Option 2:** nested fragments, 6.7%, and the uses and deps refusals they
  start, 22.2%. `struct _IO_FILE`'s fields between `#if` lines start 12.0%,
  `server.h` 5.1%. Repeats, 10.5%.
- **Groups (§11.2):** see 2.7.
- **Option 3 + groups\*:**
  - repeats, 10.4%: `bits/mathcalls*.h` read again under other macros,
    which R0 parses;
  - uses, 5.3%, started by headers absent from a compile (2.6%), negative
    lookups (1.5%) and stale versions (1.1%);
  - macro lists, 2.7%.
- **Per-reading keys** turn the repeats into units of their own, each
  checked like any other.
- **Token names** refuse 300 instances directly, and 508 fewer are reused in
  all. For 274 of the 300, checked name by name, the name plays another role
  in the fragment than the declaration it matches; the other 26 are
  unchecked (`significand` 12, `select`, `log` and five more):
  - attribute names and arguments: `malloc`, `printf` in
    `format(printf, ...)`;
  - fields: `read`, `write` and `close` in `cookie_io_functions_t`, `free`
    and `dup` in `adlist.h`;
  - parameters: `user`, `index`, `link`;
  - tags named like a function, variable or typedef: `malloc_stats`,
    `timezone`, `rax`.

**Refusals per check**, option 3 + groups\*, recompilation (instances): 177,204
reused, 4,113 empty; refused for repeat 35,225, macro 2,280, uses 1,660,
negative 109, nested 71, deps 69, conditionals 58, undefined 1, main 1,
predecessor 1. None for param, builtin, macro args, pending or pragma.

**Checks of the prediction:**
- **No unsafe decision in any run.** An oracle compares each reuse with what
  a parse would have given: its tokens, its pragma state, and its
  dependencies' oracles.
- **A stricter oracle** also depends on every name an identifier token
  spells. It rejects 309-368 reuses per run; with token names as uses, none.
- **Two implementations agree.** A second simulator, written from
  simulate.py's docstrings and this design without its code, makes the same
  decision for every compile of every pass in all 20 runs (18,700 of 18,700),
  with the same reasons, times and restore counts.
- **Tests:** `tests/server_record.py` (34 checks) and
  `tests/server_simulate.py` (13 builds, 50 checks). They pin each check
  and each defect the reviews found.
- **Two adversarial review rounds and the tests** found the defects these
  numbers no longer have:
  - main-file identity across passes;
  - fragments ending inside macro arguments, or whose macro name came from
    another macro;
  - directives with no callback (`#`, `#line`, `#warning`) leaving a stale
    view;
  - K&R parameter declarations;
  - the pragma state after a pragma line, and the pending mark (2.4);
  - references missing for cleanup functions, `__builtin_convertvector`'s
    type and block-scope tags;
  - the 2.8 rule missing, then applied per declaration;
  - replayed `#define` lines counted as free;
  - empty members of option-3 fragments;
  - the per-reading oracle.

**What S0 cannot tell:**
- **Restoring is free here.** Option 3 + groups\* restores a median 1,013
  declarations per compile (max 10,380); with per-reading keys, 1,162.
  S1 and S4 measure the cost.
- **The start-up credit is assumed.** The reuse-off row measures it.
- **Correctness is section 5's.** The oracle compares names and versions,
  not ASTs.

**Verdict: go.** As 7 feared, option 2 lands below the model's R0, and groups
as §11.2 has them recover little. Groups that hold `#define` lines recover
the nesting: 2.17x, above the model's R1 (1.93x). Decisions 6-9 settle what
the server row is.

## 7. Risks

- **S0's prediction was low for his units as published.** Option 2 gives
  1.54x, below the model's R0 row (1.66x); option 3 with groups as §11.2 has
  them, 1.66x. Only groups that hold `#define` lines (decision 6) bring the
  row above the model's R1 (2.17x against 1.93x). The paper must argue that
  departure, or report the gap as the limit of his units.
- **Restoring may cost much of the gain.** S0 counts it free, and option
  3 + groups\* restores a median 1,013 declarations per compile.
- **Chains as trees.** Chain repair keeps stale but identical declarations on
  a chain's path. S5's IR and declaration checks decide whether that holds.
- **Used bits** cannot be undone without a header change (2.8).
- **The observer fix moves the cache's numbers.** On redis it moves none of
  the model's or S0's.
- **Address space.** Projects larger than redis may not fit two passes in
  2 GiB; each needs measuring before it gets a server row.

## 8. Decisions

1. **Scope:** option 2, then option 3 and fragment groups (recommended), or
   option 2 only.
2. **Rule:** R0 with the checks of 2.6; the model's R1 kept as an estimate
   (recommended).
3. **Known differences to accept:** Used bits are not undone, and the
   diagnostic pragma state of a restored declaration's old header is lost
   (2.3).
4. **Fix the nesting observer** for the cache and the model too, rerunning
   both (recommended).
5. **Order:** S0 first, as a go/no-go, before any server code (recommended).

Decisions 1-5 were taken as recommended. S0 raises four more:

6. **Groups:** let a group hold `#define` and `#undef` lines, replayed in
   order (2.7; 2.17x, recommended), or keep §11.2's groups (1.66x). Both rows
   are reported either way.
7. **2.8:** refuse a fragment that leaves an internal or inline function
   undefined (recommended; the same rows on redis, and no rerun), or the
   fallback's rerun.
8. **Keys:** his location keys for the server row, with per-reading keys as
   a further row (2.64x; recommended), or per-reading keys for the server row.
9. **Uses:** every name an identifier token spells (2.09x, recommended), or
   only what the AST references (2.17x). The AST drops some names that the
   parse looks up, such as `N` in `vector_size(N * sizeof(int))` (test
   `vs`), and an oracle built on the AST cannot see the drop. Token names
   cost 0.08x on redis, nearly all of it from names in other roles.
