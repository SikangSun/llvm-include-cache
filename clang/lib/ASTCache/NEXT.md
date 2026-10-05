# Header cache — what to build next

What is not built, in what order, and where our storage meets PCM. Reasons live
in `DECISIONS.md`; this file is the worklist. Written 2026-09-18 against
`6996c31c7`; statuses updated 2026-10-05.

## Build order

None of this waits on the layout question in §3.

1. **Syntactic dependencies** (`DECISIONS.md` §1.12). Built 2026-09-18:
   collection at the expansion decision through an `ExpansionCandidateObserver`,
   first-sighting state per frame, NotAMacro names in the shared region, and a
   regression test through the real hit path for each known miscompile.
2. **Replay the header's `#define`/`#undef`** (`DECISIONS.md` §2.16). Built
   2026-09-29.
3. **Replay inclusion state.** Needed nothing new: the module file carries
   `#pragma once`, `#import` and included marks, and replayed guard macros
   skip guarded headers. Pinned by four tests. Built 2026-09-29.
4. **Headers that run `__COUNTER__`** (called item 5 in conversation). They
   are declined, and that is common: 21% of qemu's direct includes, among them
   `osdep.h`, and 35% of the kernel's (`DECISIONS.md` §2.7). Decided
   2026-09-30: the counter's starting value is a dependency and its advance
   an effect. Relocating the numbers on load waits for the first benchmark
   round (§4). A hit no longer resets the live counter (fixed 2026-09-29).
5. **Two bugs.** Fixed 2026-09-29. A copy the module reader refuses (built
   with other options) is passed over quietly and the next candidate tried;
   the daemon no longer drops such a copy as a duplicate; a socket path too
   long for a Unix address is refused with an error.

6. **Ask only about listed headers** (`DECISIONS.md` §2.14). Built
   2026-09-29. `CLANG_HEADER_CACHE_STATS=2` also names the macro behind each
   rejected candidate.
7. **Fold by parse time** (`DECISIONS.md` §2.15). A header costing less than
   one PCM load (226 us) folds into its parent's PCM; one costing more gets
   its own. The cutoff is decided; how the producer keeps a header out, and
   the record merge that needs, are still proposals. A dynamic cutoff that
   measures load time too is deferred (§4).

## Built 2026-09-30, not in the numbered list above

- **Item 6 of the conversation list: dependencies face strictly outward**
  (`DECISIONS.md` §1.5 update). Plus the macro-text comparison fix found with
  it.
- **Serving headers included from headers** (conversation item 7;
  `DECISIONS.md` §2.9 update). Context lines along the include chain,
  including #defines; job recording in the compile; no main-file gate.
- Region identity check at connect; reuse of an already loaded copy.

## Built 2026-10-02

- **`__FILE__` and the include path are checked** (`DECISIONS.md` §3): a
  copy records each lookup its header's `#include`s and `__has_include`s
  made and the text each `__FILE__` produced; a hit redoes the lookups.
- **Exporting a header's own part from the real compile** (`DECISIONS.md`
  §2.17), as an experiment behind `CLANG_HEADER_CACHE_EXPORT` and
  `CLANG_HEADER_CACHE_LOAD`: `ASTWriter::setPartFilter`, macro expansions
  outside the part dropped from the source-location table, file names kept
  as opened. Seven redis headers served back into their own `.c` file give
  identical assembly with and without `-g`.

## Blocking a benchmark run

The export at a miss (`DECISIONS.md` §2.17, approved 2026-10-02), which
replaces the separate compile and with it the conflicts between copies
built after different `.c` files (§3). Target: a first debugging version the
week of 2026-10-05. What it needs:

1. Export at a miss, at the first top-level declaration boundary after the
   header ends, and push the part with its metadata.
2. The dependency tracker running in the real compile for the headers it
   exports, with the macro effects, pragma snapshots and the
   `__COUNTER__`-family decline the producer had.
3. Outside declarations as dependencies: name and fingerprint.
4. The name each recorded lookup found, compared at a hit, not only the real
   path: a part holds files under the names its compile opened them by.
5. Included-file marks limited to the header's region.
6. A compile that already loaded a copy writes the part as importing every
   loaded module. First version: export only from a compile that has loaded
   none.

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
