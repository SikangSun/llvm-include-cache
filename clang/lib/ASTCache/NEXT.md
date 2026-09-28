# Header cache — what to build next

What is not built, in what order, and where our storage meets PCM. Reasons live
in `DECISIONS.md`; this file is the worklist. Written 2026-09-18 against
`6996c31c7`.

## Build order

None of this waits on the layout question in §3.

1. **Syntactic dependencies** (`DECISIONS.md` §1.12). Built 2026-09-18:
   collection at the expansion decision through an `ExpansionCandidateObserver`,
   first-sighting state per frame, NotAMacro names in the shared region, and a
   regression test through the real hit path for each known miscompile.
2. **Replay the header's `#define`/`#undef`** into the live macro table after
   the load. Module macro visibility is not the same thing — `lim.h`'s `#undef`
   never took effect, which is a silently different program.
3. **Replay inclusion state.** `#pragma once` and `#import` set
   `HeaderFileInfo::isPragmaOnce`/`isImport` (`HeaderSearch.h:70-74`) and define
   no macro, so without this a second `#include` is not skipped.
4. **Backfill the varying builtins** (`__COUNTER__` and the rest). Needs a fixup
   list written at population, because by AST time only the value survives.
5. **Two bugs.** `warn_ast_file_config_mismatch` is `DefaultError`
   (`DiagnosticSerializationKinds.td:42-47`), so a rejected candidate fails the
   user's build; and a socket path over 108 characters truncates silently.

6. **Ask only about listed headers** (`DECISIONS.md` §2.14). Time each
   header between its `FileChanged` events and report the times at the end of
   the compile; the daemon decides membership and publishes the list in the
   shared region; the compiler checks the list before opening the socket.
7. **Fold by parse time** (`DECISIONS.md` §2.15). A header costing less than
   one PCM load (226 us) folds into its parent's PCM; one costing more gets
   its own. The cutoff is decided; how the producer keeps a header out, and
   the record merge that needs, are still proposals. A dynamic cutoff that
   measures load time too is deferred (§4).

## What changes in what exists

| Where | Change |
|---|---|
| `DependencyRecord.h` | Done: `ValueDependency` became `SyntacticDependency`, with a required state — *not a macro* or *expands to these tokens*. |
| `DependencyRecord.h` | `PragmaSnapshot` gains stack values; the flattened value cannot express a header that pops more than it pushes. |
| `DependencyRecord.h` | New effects struct: macro definitions in order, inclusion marks, builtin fixups. Absorbs `Exit`. |
| `CacheProtocol.h` | Done: `CacheEntryRef` carries offset/length for the NotAMacro names, so they stay out of the socket. |
| `CacheClient.h` | Done: `MismatchReason::Value` became `Syntactic`. |

The observation layer is untouched.

## The PCM seam

Three joints; only the third blocks.

1. **The record** stays in the protocol, not in the saved bytes — the client
   judges before loading. Add only a content hash of it in a
   `ModuleFileExtension` block, so the daemon's index and the bytes cannot drift
   apart.
2. **The effects** stay with the record. The client applies them after
   `loadModuleFile` returns, and reading them back out would mean driving an
   extension reader through `ASTReader`'s load.
3. **Layout** — blocking.

| | Shape | Status |
|---|---|---|
| A | per (header, context), context hidden | rejected: qemu `virt.c` gave 68 PCMs, 460 MiB, 128 s, and fails to load |
| B | per `.c` file, submodule per header | forfeits cross-file reuse |
| C | per header, importing the rest | an import graph over every header |
| D | shared base for the system closure, project headers above it | recommended |

D wins on three facts: system headers are shared by nearly every translation
unit; their variants are governed by a few feature-test macros; and fixed
overhead amortizes (65% of `tiny.pcm` was `BLOCKINFO`). Its cost is duplicate
declaration merging where two copies overlap — the thing that stopped PPH.

Two measurements decide it: how many distinct system-header closures a real
build produces, and what the merge costs when copies overlap.

If D fails, `ASTWriter`/`ASTReader` still stay, used as libraries; only the
container and the validation get replaced (`DECISIONS.md` §3).

## Decisions needed

- Is D the direction, and do the two measurements run now?
- The §2.15 mechanism: separate modules in the generated module map, the
  fallback to folding, and merging a referenced child's record into its
  parent's.
- `DECISIONS.md` §2.1, the separate sub-compile, is still not approved.
  Population cannot start either way until it is.
