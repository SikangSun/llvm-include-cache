# Pre-parsed headers (PPH) as a baseline: implementation plan

Branch `pph-baseline`, from `header-cache` at `cda4042be`. Written
2026-10-27. This file is the plan to check against N3426. Section 2 maps each
PPH behaviour to its source and to the code that will implement it, and
section 9 lists the decisions still open.

## 1. What PPH is, in one paragraph

Google's PPH (Crowl and Novillo, WG21 N3426, 2012; GCC branch
`refs/dead/heads/pph`, last commit `f86b1b24b0d9`, 2012-04-26) compiles each
*modular* header once, in isolation, into an *image*. A compile that reaches
`#include` of a header with an image loads the image instead of parsing the
text. Before using it, the compile checks that no identifier the header used
has a different macro meaning here, then replays the macros the header
defines. A header's image references the images of the mapped headers it
includes and embeds the text of the unmapped ones, so two images can carry
the same textual header, and loading merges the duplicates. Images are build
products made before the compiles that use them. The compiler checks nothing
about staleness or flags; that is left to the build system.

N3426 is a retrospective with no option names, file format or failure
behaviour. Those come from the branch source and the `[pph]` gcc-patches posts
(2010-11 to 2012-05). Section 2 says which source each behaviour comes from.

## 2. PPH behaviours, their sources, and how we implement them

| # | PPH behaviour | Source | Our implementation |
|---|---|---|---|
| P1 | One image per modular header, built by a separate compile of the header alone with the TU's own flags (`-fpph-gen`) | N3426 "Implementation of the Approach"; branch `cp/lang-specs.h:43-53`, `c-opts.c:405-409` | **Generation compile** (section 4): a wrapper TU whose only line is the header's `#include`, compiled with the configuration's flags and cwd. The fork's PartExporter writes the header's part as the image. |
| P2 | A map from `#include` to image (`-fpph-map`, "spelled name TAB image path", exact match) | PDF version of N3426 "Approach Constraints"; branch `c-opts.c:174-256` | **Image directory** keyed by (configuration, header real path, opened name) instead of the spelled name. The real path is what the fork's hook already looks up; the opened name keeps `__FILE__` and debug names identical. **Deviation**, see D5. |
| P3 | An `#include` of a mapped header loads the image instead of the text; no image means text | N3426 "Approach Constraints"; branch `pph-core.c:883-945` | The fork's `HeaderCacheHook::tryServe`, with a **directory backend** for `CacheClient` (section 5). No image means "not listed", so the header is read as text. |
| P4 | Modular means: (1) guarded, (2) parses in isolation, (3) always included at global scope | N3426 "Approach Constraints" | (1) the generation compile refuses a header with no controlling macro and no `#pragma once` (`HeaderSearch` file info); (2) a generation compile with any error writes nothing (the fork's Q35 rule); (3) at load, the fork's declaration-boundary gate refuses an `#include` that is not at file scope, and discovery leaves out any header seen included elsewhere. |
| P5 | `#include_next` in the header disables its image | branch `pph-core.c:913-921` | The generation compile declines a header whose text runs `#include_next`. |
| P6 | The image stores the identifiers the header used; at inclusion none may have a different macro meaning | N3426 "Implementation of the Approach" para 2 | The fork's dependency record, recorded in isolation: **NotAMacro** / **ExpandsTo** for every identifier considered for expansion, and **Conditional** for every definedness test. In isolation the only outside macros are predefines and `-D`. |
| P7 | The exact check is three-way: unknown now → fail only if it was a macro; was not a macro but is now → pass only if now equal to the header's *end* value; was a macro → must be the same macro now | branch `libcpp/symtab.c:667-764` | **New check policy `pph`** (section 6), replacing the fork's conditional and macro steps: the same three cases over the record's entries, with the *after* value taken from the header's macro effects. |
| P8 | On a failed check: the 2011 design refused the image (error); the 2012 tip only warned and used it | branch `pph-in.c:3222-3227`; gcc-patches 2011-October/325188 | **Refuse and read the text** (the 2011 rule). Using an image the check rejects can change the program. See D1. |
| P9 | The header's macros are replayed after the check; identical redefinitions are skipped; this also defines the include guard | N3426 para 1; branch `symtab.c:848-920`, gcc-patches 2012-March/336180 | The fork's macro-effect replay, unchanged. It already skips effects already in force (I8), which covers identical redefinitions. |
| P10 | Nested mapped headers are *references*: a.pph points to b.pph, b is loaded first (depth-first, in `#include` order), and is not loaded twice | branch `pph-out.c:358-415`, `pph-in.c:312-468`; N3426 "Implement Export Control" | The fork's **linked copies** (`CLANG_HEADER_CACHE_LINK`, commit `55ac1aef`): in the generation compile, a nested header that has an image is served from it, and the part records a link. At load, links load first and are skipped when their guard says the header was already read. |
| P11 | Nested unmapped (non-modular, typically system) headers are embedded as text, so two images can contain the same declarations, which are merged at load | N3426 "Approach Constraints", "Inherent Implementation Problems" | In the generation compile, an unmapped header is not listed, so it is parsed as text inside the part and folded in. Duplicates across images are merged by Clang's module reader (`ASTReaderDecl` merging). Risky for C; see section 7. |
| P12 | Images are loaded at the `#include` (GCC loaded all images before parsing, a GCC artefact) | branch `cp/parser.c:628-650` | At the `#include`, as the fork already does. This is closer to N3426's "reinterpret the #include" than GCC's eager load. |
| P13 | No staleness, flag or version check; images are build products, rebuilt by the build system in include-graph order | N3426 "The Pre-Parsed Header Approach", "Google Build Problems"; branch `pph-in.c:74-129` | The harness rebuilds every image for each measured build, in dependency order. Configurations key the image directory, so a compile never sees another configuration's images. The fork's file-stamp check is off in `pph` mode (D6). |
| P14 | Codegen order matches a textual compile (GCC needed a replay table) | branch `pph-out.c:2451-2549` | Clang hands loaded top-level declarations to CodeGen through the module reader; the fork's byte-identity work (Q7 to Q35 in REPORT.md) already covers this for its own copies. Verified by comparing objects, not assumed. |

## 3. The pieces, and where each one lives

New code goes in new files where possible. The `header-cache` branch on summer
has uncommitted changes to `CacheClient`, `HeaderCacheServer`, `PartExporter`
and the AST reader and writer, so edits to those files are kept to small,
clearly marked hook points to keep the later merge easy.

| Piece | Files | Size |
|---|---|---|
| Directory backend: find, read and atomically write images; one file per image | new `clang/lib/ASTCache/PPHStore.{h,cpp}`; small branch in `CacheClient.cpp` (`lookup`, `fetch`, `isListed`, `push`, and early returns where it uses the socket) | ~300 lines |
| `pph` check policy (the three-way rule, P7) | new `clang/lib/ASTCache/PPHCheck.cpp`; a policy switch in `matchesLiveState` | ~200 lines |
| Mode wiring: `CLANG_PPH_DIR`, `CLANG_PPH_GEN`, forcing linked-copies mode | `HeaderCacheServer.cpp` (`installHeaderCacheHook`, constructor), `PartExporter.cpp` (link-mode switch) | ~60 lines |
| Generation rules (P4 guard, P5 `#include_next`, export only the target) | `PPHStore.cpp` plus the exporter's write decision | ~100 lines |
| Discovery: every `#include`, with includer, spelling, search directory, opened name, real path, at-boundary | new `clang/lib/ASTCache/PPHDiscover.cpp`, installed by `CLANG_PPH_DISCOVER=<dir>` | ~150 lines |
| Unit tests | new `clang/unittests/ASTCache/PPHTest.cpp`, in the style of `HitTest.cpp`'s ExportTest | ~600 lines |
| Harness: discover, choose modular headers, build images in order, replay with K workers, compare objects | new `clang/utils/header-cache-bench/pph/` (`discover.py`, `build_images.py`, `replay.py`, `compare.py`) | ~800 lines |

## 4. Generating an image (P1, P4, P5, P10, P11)

For one header H in configuration K (flags and cwd), with opened name N:

1. Write a wrapper TU containing only H's `#include`, spelled so the compile
   opens H under the same name N. For a quoted include found next to its
   includer, the wrapper sits (through a VFS overlay, not on disk) in the
   includer's directory under the includer's naming; for an angled include,
   `#include <spelling>` with K's search path does it.
2. Compile it with K's flags in K's cwd, `-fsyntax-only -fmodules
   -fno-implicit-module-maps`, with:
   - `CLANG_PPH_DIR=<images>/<K>`, so nested headers that already have
     images are served from them and become links (P10);
   - `CLANG_PPH_GEN=<H's real path>`, so H is the only header listed for
     export: H misses, is parsed as text, and the exporter writes its part.
     Nested headers without images are not listed, so they are parsed as text
     inside H and folded in (P11).
3. The part, with its metadata, is written to a temporary file in the image
   directory and renamed into place, so a parallel compile never reads half
   an image.
4. Refuse, writing nothing, when: H has no controlling macro and no
   `#pragma once` (P4.1); the compile has an error (P4.2); H's text runs
   `#include_next` (P5); H was not entered (its guard was already defined by
   K's `-include` files); or the exporter declines for its own reasons
   (`__COUNTER__`, unbalanced pragmas, push_macro reaching outside).

Images are generated children first, in the include-graph order from
discovery, with independent headers in parallel. A cycle is broken by leaving
its members without images, as GCC forced circular includes to text.

## 5. Serving an `#include` (P3, P6 to P9, P12)

`CLANG_PPH_DIR=<images>/<K>` in a normal compile installs the fork's hook with
the directory backend, linked-copies mode on, and the `pph` policy. At an
`#include` the order is the fork's, unchanged except where noted:

1. Include guards, `#pragma once` and `#import` are settled by the
   preprocessor before the hook runs, so a header already read never reaches
   it (this is where a repeat include of a loaded image becomes a no-op).
2. Not listed (no image for this header, configuration and opened name):
   read the text.
3. Not at a declaration boundary: read the text (P4.3).
4. Read the image's metadata in place and run the `pph` check (section 6).
   Fail: read the text (P8).
5. Load the linked images first, depth-first, skipping any whose guard is
   already defined (P10); then the image itself, through the fork's
   `load()`.
6. Replay the header's macro effects (P9), its pragma and warning state, and
   its saved diagnostics; report its files for `-MD`.

## 6. The `pph` check policy (P6, P7)

Each image's record lists, per identifier, its state at first sighting
("before") and, through the macro effects, the state the header left
("after"). For every recorded identifier, with "now" the state at the
`#include`:

- **before: not a macro.** Pass if it is not a macro now. If it is a macro now,
  pass only if now equals the header's *after* definition (how GCC let an
  already-defined guard of an embedded header through); otherwise fail.
- **before: a macro with tokens T.** Pass only if it is a macro now with the
  same definition.
- **Definedness tests** (`#ifdef`, `defined()`) are the same rule restricted to
  defined or not.

Not part of PPH's rule, and so off in `pph` mode: symbol checks (`T * p;`),
include-search and `__FILE__` checks, pragma entry state, warning state,
outside-declaration fingerprints and file stamps. Two of them are kept, by
necessity, and reported as such:

- **Declaration lookups.** A part's references to outside declarations must
  resolve, or the reader stops with a fatal error. In isolation the only
  outside declarations come from linked images, so this costs little.
- **Opened name and search.** The image directory is keyed by the opened name,
  so `__FILE__` and debug names match by construction; the check is kept as an
  assertion.

A second policy, `safe` (`CLANG_PPH_POLICY=safe`), is the answer to "what
would PPH need to be correct in C", reported beside `pph` (decision D3). It
turns the fork's other checks back on, and applies the end-value exception
only to include guards: those of the headers the image holds as text, and of
the images it links. Section 10 shows why: GCC's rule applied to every name
passes a header that tested a name before it defined it.

## 7. Risks

- **Merging duplicates in C (P11).** Two images that both embed `<stdio.h>`
  hand Clang two copies of `FILE`, `fpos_t` and so on, and a textual
  `<stdio.h>` earlier in the TU adds a third. Clang merges C tag types across
  modules by structural equivalence, but the fork's history (DECISIONS.md §3:
  `fd_set`, the `fscanf` asm label) shows failures. First test case. If
  merging fails often, the fallback is that PPH's rule rejects an image whose
  embedded header was already read, which is safe and costs reuse.
- **Few headers are modular in C.** Feature-test macros (`_GNU_SOURCE`) usually
  come from project headers, so a header that includes system headers often
  fails P6 where it is used. That is PPH's real behaviour; we report the counts.
- **Byte identity under `-g`.** Images must open every file under the same name
  as the textual compile. Section 4 step 1 is built for this; the first `-g`
  test decides whether more is needed.
- **Linked-copies gaps** found by the code reading: a linked image's files are
  not reported for `-MD`, and its outside names are not pinned (Q8). Both
  are fixed in the shared code, since they also affect `CLANG_HEADER_CACHE_LINK`.
- **The machine.** Turbo off, 4-CPU container; runs resume after a crash. Local
  timings are for development only.

## 8. Order of work

| Step | What | Done when |
|---|---|---|
| M1 | Directory backend and `CLANG_PPH_DIR` serving, with images made by hand through the existing export path | A unit test serves one header from a file and the object matches plain clang |
| M2 | Generation compile (`CLANG_PPH_GEN`), atomic writes, guard and `#include_next` refusals | Unit tests: guarded header gets an image; unguarded, failing and `#include_next` headers get none |
| M3 | Nested images (links) and embedded textual headers | Unit tests: a.h includes mapped b.h (link) and unmapped c.h (folded); two images both embedding c.h load into one TU |
| M4 | The `pph` policy | Unit tests for every case of section 6, including the guard of an embedded header already defined |
| M5 | Discovery and the harness on redis | redis builds with PPH, every object identical to plain clang's, at 1 and 8 workers |
| M6 | The other five projects, fixing what breaks | All six, or every remaining difference named |

## 9. Decisions to confirm

- **D1. Failure policy.** Refuse and read the text (2011 design, correct) rather
  than warn and use (2012 tip). Recommended: refuse.
- **D2. Nested headers.** References to child images (GCC branch) through linked
  copies, rather than folding everything into each image. Recommended:
  references. The fork measured linked copies at 1.00x, so expect PPH to be
  load-bound; that is a finding, not a reason to change it.
- **D3. Checks beyond PPH's rule.** Report `pph` (PPH's rule plus the two kept
  checks) as the baseline, and `pph-safe` beside it. If `pph` changes any object,
  that is reported as PPH's unsoundness in C, and the timing comparison uses
  `pph-safe`. Alternative: demote the offending headers from the map.
- **D4. Which headers get images.** Project headers only, with system headers
  embedded as text, as GCC's maps did. Option `+sys` to map guarded system
  headers that pass.
- **D5. Map key.** Configuration, real path and opened name, instead of GCC's
  spelled `#include` text. Same effect for a fixed build, more robust, and it
  keeps file names identical.
- **D6. Staleness.** No check in the compiler (faithful); the harness rebuilds
  images for every build. The rebuild experiment rebuilds only images on an
  include path from the edited file, as N3426 describes.
- **D7. Reference for byte identity.** Plain clang with `-fmodules
  -fno-implicit-module-maps` (as the cache's published claim), with plain clang
  without them also reported.

## 10. Status (2026-10-28)

Built, not committed. M1 to M4 done; M5 running on redis.

| Piece | Where |
|---|---|
| Directory backend (images as files, written atomically, offsets in place of the region's) | `CacheClientDirectory.cpp`; routing in `CacheClient.cpp` (`lookup`, `fetch`, `isListed`, `push`, `seenBefore`, `reportTimings`) |
| `CLANG_PPH_DIR`, `CLANG_PPH_GEN`, `CLANG_PPH_POLICY` | `HeaderCacheServer.cpp` (`installHeaderCacheHook`); only the target is exported in a generating compile (`CacheClient::mayWrite`) |
| Generation rules (no guard, `#include_next`, never entered) and the "after" table | `PPHGeneration.{h,cpp}` |
| PPH's three-way rule and the switches for checks PPH did not make | `CheckPolicy` in `CacheClient.h`; `matchesLiveState` |
| Smoke tests (refusals, links, guard of a linked image, embedded header merged with its text, `-g`) | `clang/utils/header-cache-bench/pph/smoke/run.sh`: all pass |
| Harness (discover with `-H`, generate in include order, replay three ways) | `clang/utils/header-cache-bench/pph/pph_run.py` |

Changes from sections 3 and 4 made while building:

- **Discovery needs no compiler change.** `-H` gives every header a compile
  enters, nested, under the name it opened it by; `PPHDiscover.cpp` was not
  written. Global scope is left to the load-time gate, as GCC checked it at
  the `#include`.
- **The generation wrapper sits in the header's directory**, named as the
  compile names that directory, and includes the header's base name: the
  header is then opened by exactly the compile's name (`./fmacros.h`,
  `../deps/hiredis/hiredis.h`). One image per name a header was opened by.

### redis (189 compiles, `-O3 -flto -g`), first results

| | Images | Hits / lookups | Objects identical to plain clang | Differ |
|---|---|---|---|---|
| `pph` (GCC's rule) | 76 of 167 header names | 553 / 2,065 | 182 | 5: `sds.c`, `rax.c`, `listpack.c`, `module.c`, `redis-benchmark.c` |
| `safe` | same images | 454 / 2,078 | 185 | 2: `module.c`, `redis-benchmark.c` |

2 compiles (`foo.c`, test modules) fail with and without PPH. Headers without
images: 24 have no include guard, 32 do not compile alone, 6 are in include
cycles; the rest are system headers, which PPH does not map (D4).

### Findings

1. **PPH's three-way rule gives wrong code in C.** `zmalloc.h` tests
   `defined(__GLIBC__)` (line 59) before it includes any libc header; in
   isolation the test is false, and `zmalloc_usable_size` becomes a function.
   Where it is included `__GLIBC__` is already 2, which is also its value at
   the header's end, so GCC's rule passes the image: `rax.c` calls
   `zmalloc_usable_size` where plain clang calls `malloc_usable_size`. The
   rule cannot tell a test made before the header defined a name from one
   made after. `safe` applies the end-value exception to include guards only
   (of headers the image holds as text, and of linked images), and those
   three files become identical.
2. **A variable defined in a served header gets its debug attachment twice
   under `-g`** (`!dbg !0, !dbg !0`), in `module.c` and `redis-benchmark.c`
   (`crc16_slottable.h`'s `crc16_slot_table`). Not PPH's: the cache at
   `cda4042be` does the same, with the daemon, in its default mode and in
   linked mode (three-line reproduction: a header defining `int e_count = 2;`).
   Likely cause, from reading the code: the exporter makes each such variable
   a module initializer (`PartExporter.cpp:1525-1540`), and the reader also
   hands it to CodeGen as an interesting declaration, because
   `isConsumerInterestedIn` skips only variables owned by a module-map module
   (`ASTReaderDecl.cpp:3258`). Not fixed here: both files are among the ones
   changed on summer.
