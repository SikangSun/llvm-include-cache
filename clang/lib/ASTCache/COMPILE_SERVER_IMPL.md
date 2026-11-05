# Bothner's compile server: how it is built

Written 2026-11-05. COMPILE_SERVER_DESIGN.md has the design and the
results, and COMPILE_SERVER_LOG.md has the order of events. Names below are
in `clang/utils/header-cache-bench/bothner/BothnerServer.cpp` unless a file
is named.

## 1. The pieces

| Piece | Where | Job |
|---|---|---|
| The server | `BothnerServer.cpp`, a plugin with the action `bothner-server` | One `clang -cc1` process that serves compiles one at a time |
| Fragment hook | `HeaderCacheHook.h` (`WantsFragments`, `fragmentEnd`, `fragmentStart`), `PPDirectives.cpp` (`HandleDirective`), `PPLexerChange.cpp` (`HandleEndOfFile`) | Tells the server where each fragment ends and starts, and skips a reused one |
| Location order | `SourceManager.cpp` (`isBeforeInTranslationUnit`) | Orders the locations of two compiles' main files by compile |
| Driver forwarding | `driver.cpp` (`forwardCC1`), `CacheProtocol.h` (message kinds 11 and 12) | Sends each cc1 job to the server's socket, and compiles locally when the server refuses |
| Checks | `tests/server_reuse.py`, `server_s1.py`, `server_objects.py`, `diagtext.py`, `server_model.py`, `server_compare.py`, `server_space.py` | Section 5 of the design, and step S4 |

The server needs no other change inside Clang. It reaches private members
with explicit template instantiation of member pointers (`Rob`). Examples
are record layouts, the token count, the FileManager's maps, and a
declaration context's last declaration.

## 2. What lives how long (design 2.2)

- **The process** (`Server`):
  - the parsed invocations (`Invocations`), keyed by the working directory
    and the arguments less the input, the output and the main file's name
  - the configurations (`Configs`).
- **A configuration** (`Config`), keyed by `configKey`: the working
  directory, every language option, the `-fno-builtin` names and the
  target. It holds:
  - the DiagnosticsEngine, FileManager, SourceManager and TargetInfo
  - the identifier, selector and builtin tables
  - one ASTContext, made with `TU_Incremental` so that each compile can add
    a translation unit
  - the fragment store (`Store`).
- **A compile** has:
  - its HeaderSearch, Preprocessor, Sema and parser
  - its consumers (`ServerConsumer` and the code generator)
  - a new TranslationUnitDecl in the shared ASTContext
  - the fragment state (`Compile`) and the diagnostic text (`DiagWatch`).

  The Preprocessor finds every name in the configuration's table
  (`PersistentLookup`).

## 3. One request (`Server::serve`)

1. **Read the job.** It has the working directory, the arguments after
   `-cc1`, an optional dump path and a build ID. The server parses an
   invocation once per key and then changes only its input, output and main
   file's name.
2. **Refuse what the server cannot do as cc1 does** (status 2):
   - `-mllvm` options, macro debug information, coverage mapping, time
     traces, plugins, header-include traces, serialized diagnostics
   - modules, a PCH, output to standard output, a BPF target
   - actions other than `-fsyntax-only`, `-emit-llvm`, `-emit-llvm-bc`, `-S`
     and `-c`.
3. **Reset (2.3) and undo (2.8)** (`Server::reset`):
   - the identifiers: Sema's bindings (`FETokenInfo`), the macro flags
     (final, deprecated, restrict_expansion, poisoned, has a macro), and
     keywords turned back into identifiers
   - the ASTContext's caches: the `FILE` and `jmp_buf` typedefs, the
     extern "C" context, type and alignment information, record layouts,
     unknown documentation commands, and the section table
   - what later code changed in place on stored declarations: attributes
     from `#pragma weak` and `redefine_extname`, and a tentative `T[]` that
     became `T[1]`
   - the Used bits the last compile could have set (`findUsedBit` finds the
     private bit once, `clearUsed` clears it).
4. **Check files, once per build ID (2.10)** (`checkFiles`). The
   FileManager forgets a file whose size, modification time or identity
   changed, and the store drops that file's fragments.
5. **Set up the compile.** `DiagWatch` takes the diagnostics: the text as
   cc1 prints it, the count line, and the fragment each diagnostic belongs
   to. The Preprocessor gets:
   - the fragment hook (`FragmentHook`), the macro-expansion observer
     (`Candidates`) and a token watcher
   - `DiagPragmas`, which follows the `#pragma diagnostic` stack
   - `MacroLines`, for option 3 and groups
   - `EntryNames`, which gives each file the name this compile spells (a
     restored declaration then names its file as a parse would).
6. **Parse** with ParseAST. The server decides each fragment when the
   preprocessor reaches it (section 4).
7. **Store** (`Compile::finalize`), only when the compile had no error.
   Each new version gets:
   - for each name it binds, references or spells, the declarations of that
     name that the compile made before it (`Lists`, `TokenLists`)
   - the 2.8 rule: a version that declares an internal or inline function
     and does not define it is never reused (`undefined`)
   - its bindings' attributes and types as its parse left them, Sema's
     lists it added to, and its sections.

   It then replaces the last version of its location (`Store::Latest`).
8. **Generate code** for `-emit-llvm`, `-S` and `-c` with
   `emitBackendOutput`, as plain clang's BackendConsumer does, but without
   clearing the AST.
9. **Reply** with one JSON object:
   - the status: 0, 1 for errors, 2 for a refusal
   - the diagnostics, and the count line ("2 warnings generated.")
   - the count of each decision reason, times, the source-location space
     used, and memory.

## 4. Fragments and the rule (class `Compile`)

- **A fragment** is a header's text between two directives (option 2).
  `fragmentEnd` closes the open fragment at a directive's `#` or at the
  file's end. `fragmentStart` opens the next one after a directive, or
  where an included file returns.
- **Option 3 and groups** (2.7, `joins`):
  - with `option=3`, a `#define` or `#undef` line joins the fragment. A
    reuse makes the same definitions again (`MacroEffect`, `replay`)
  - with `groups=1`, `#if`, `#ifdef`, `#ifndef`, `#else`, `#elif` and
    `#endif` lines join it while the parser is inside a declaration, or
    while a conditional the fragment opened is still open.
- **The key** (decision 8) is the file entry and the offset where the
  fragment starts (`keyOf`). R0 keeps one version a key: the last parse's.
- **The decision** (`decide`) runs these checks in order. The first that
  fails names the reason:
  1. A stored empty fragment is skipped (`empty`). The lexer must be able
     to skip there (`no skip`), and no parser pragma may wait (`pending`).
  2. The parser must be between declarations (`nested`). This is the fixed
     observer of 2.9: the parser's own answer, the last token it got, and
     Sema's context (`atBoundary`).
  3. This compile must not have processed the key yet (`repeat`), and a
     version must be stored (`new`).
  4. The version's own parse must have found no reason against reuse
     (`undefined`, `macro args`, `builtin`, `param`, `main`, `diagnostics`,
     `nested end`).
  5. The line table must give the start the same place: system header or
     not, and the `#line` entry before it. A fragment that spelled
     `__FILE__` also needs the same presumed name (`file`). The diagnostic
     state must be the parse's (`diagnostics`).
  6. The fragments whose declarations it used must hold, in this compile,
     the versions they held at its parse (`uses`).
  7. Each name that reached the macro-expansion decision must have the same
     state: a hash of the definition and the pragma flags (`macro`). In
     option 3, each name its lines define or remove first must have an
     earlier definition that gives the line the same output (`samePrior`).
     No such name may be final, watched by `-Wunused-macros`, or
     predefined.
  8. The pragma state 2.6 lists, such as `pack`, must be the same
     (`pragma`).
  9. For each name it binds or references, the declarations before it must
     be the same (`negative`, `deps`, `predecessor`). The same holds for the
     names its tokens spell (`token names`, decision 9).
- **A reuse:**
  - the lexer seeks to the directive that ends the fragment
    (`FragmentSkip::End`)
  - the multiple-include optimization hears that the fragment had tokens
    and expanded a macro, so include guards still work
  - option 3's lines are made again, and the token count grows by the
    parse's count
  - an `annot_module_include` token for the module `__bothner_restore` is
    queued for the parser.
- **A parse** records a new `Version` while it runs:
  - its bindings (`bindings`): what the translation unit gained, and the
    tags and enumerators inside its structs. Members it added to a struct
    body that an earlier fragment opened count too
  - its uses and deps (`uses`, with `UseVisitor` on each top-level
    declaration). These are every declaration it looked up and the earlier
    declarations it redeclares. They include the tag a typedef names, the
    struct a member access reaches, and the entities it odr-used and
    referenced
  - its macro list (`candidate`): each identifier that reached the
    expansion decision, with its state then
  - reasons never to reuse it:
    - `__COUNTER__`, `__DATE__`, `_Pragma`, `__builtin_FILE`
    - an `alias` or `ifunc` attribute, a tag first declared in a parameter
      list, a reference to a main-file declaration
    - a diagnostic, or an active `#pragma diagnostic`.

## 5. Restore (design 2.5)

The queued token reaches the parser at file scope. Clang then asks the
module loader to make the module visible, and `ServerLoader` calls
`restoreNext`. For the version it restores, it:
- sets each binding's redeclaration chain right: `repairChain` sets the
  latest pointer in the first declaration
- clears each binding's referenced bit, and puts back its attributes and
  type
- pushes each binding on the translation unit's scope chains, without
  adding it to the context again
- sets the `FILE`, `jmp_buf`, `sigjmp_buf` and `ucontext_t` typedefs it
  binds
- registers its block-scope externs in the extern "C" context, and enters
  its sections in the section table
- marks the Used and referenced bits its parse marked
- adds to Sema's lists: tentative definitions, unused file-scope
  declarations, ext-vector typedefs
- hands each top-level declaration to the code generator, tag definitions
  first (`completed`). It holds the last one until the parser's next token
  (`Held`), as a parse hands it over only after the next directive.

## 6. Diagnostics as cc1 prints them

A reuse prints nothing, so the server keeps every diagnostic exact:
- a fragment whose parse printed a diagnostic is parsed every time
  (`diagnosed`, `fragAt`)
- each version keeps the diagnostic state at its start: the warning
  options and the `#pragma diagnostic` stack (`Store::DiagStates`)
- nothing parsed under an active `#pragma diagnostic` is reused, because a
  later diagnostic into a restored declaration gets the warning options'
  state
- record layouts and unknown documentation commands are per compile, for
  `-Wpadded` and `-Wdocumentation`
- Sema's unused-warning list is read as it grows (`readUnused`), and a
  restore marks the referenced bits, for the `-Wunused-function` family
- the macro state includes the pragma flags, and option 3 checks the
  earlier definitions, for `-Wmacro-redefined`
- a reuse adds its parse's token count, for `-Wmax-tokens`
- the reply carries all diagnostics and the count line.

Two differences in text only remain (design section 6):
- a diagnostic that points into a restored declaration can name the
  include chain of the compile that parsed it
- some diagnostics can come in another order.

## 7. The driver path (step S6)

- `CLANG_COMPILE_SERVER=<socket>` makes the driver send each in-process
  cc1 job to the server (`forwardCC1`), with `CLANG_COMPILE_SERVER_BUILD`
  as the build ID. A direct `clang -cc1` is never sent.
- The server, started with `socket=<path>`, answers one connection a job,
  one job at a time.
- The driver prints the reply's diagnostics and count line and exits with
  its status. It compiles the job itself, with a note on standard error,
  when:
  - the socket cannot be reached
  - the reply is missing or malformed
  - the status is 2.

## 8. Limits

- **Source-location space.** A server never frees the 2 GiB it has. git
  (3.22 GiB) and ffmpeg (6.09 GiB) do not fit a fill and the same build
  again in one server. ffmpeg needs 3.05 GiB for one pass. When the space
  ends, the server refuses the compile and the driver compiles it. A later
  compile still runs if its files fit in what is left. In a macro
  expansion, Clang ends the process instead, and the driver compiles the
  file itself.
- **One compile at a time.** Step S9 (`-j4`) runs several servers.
- **What is not served** (step 2 of section 3) goes to cc1 unchanged.
