# Live copies: the cache's copies kept as declarations in one process

**Ruled out, 2026-11-06.** The user requires parallel compiles in separate
processes that share one store, with no forking. Kept declarations hold the
memory addresses of one process, so other processes cannot use them.

Written 2026-11-06. The user approved writing this design ("yes to both",
2026-11-06). Nothing in it is built. COMPILE_SERVER_IMPL.md describes the
server code that it reuses, and DECISIONS.md (header-cache branch) describes
the cache.

The idea: keep the cache's unit, a copy of one header for one context, and
the cache's checks. Store each copy as the declarations its parse made, in
one long-lived process, as the compile server stores fragments. A miss then
writes nothing, and a hit loads nothing. The process links the same
declaration objects into the next compile.

## 1. Why

**Loading and writing PCMs are a large part of the cache's time.** FFmpeg,
first build, `-fsyntax-only`, one compile at a time, from the compiles' own
timers (results-2026-11-06/server):

| Cache work | Time (s) |
|---|---|
| Reading headers as text, outermost headers | 26.4 |
| Loading copies (Clang's reader) | 10.5 |
| Writing copies (Clang's writer) | 10.1 |
| Checking candidates | 6.1 |
| Replaying hits' macro and pragma effects | 4.6 |
| Whole build, wall: cache / server / plain clang | 127.1 / 112.6 / 187.2 |

Without its loading and writing, the cache would reach about 2.44x wall
time on first builds, against 2.07x for the server. Both are geometric means
of git, CPython, redis and FFmpeg. That is an upper bound: it counts a
restore as free.

**The cache already wins the same build again**, by instructions: 2.65x
against 2.37x for the server. In redis, loading is now the cache's largest
cost in that build. With eager writes, loading takes 0.87 s. Replaying
macro and pragma effects takes 0.65 s, checking 0.35 s, and reading text
0.27 s (results-2026-11-06/firstread).

**History.** DECISIONS.md 2.24 (2026-10-06) listed a compile server as
option E. In redis's 8.3 s front end, it removes about 2 s of loading and
1.2 s of writing. The user ruled it out on 2026-10-07 because it was "not
build system transparent". The same day, the sources showed that both
earlier servers were drop-in compilers. The server in this branch is
drop-in too. The driver sends each cc1 job to it (`CLANG_COMPILE_SERVER`),
and compiles the job itself when the server refuses.

## 2. The main alternative designs

| Design | Unit of reuse | Storage | Measured or expected | Gives up |
|---|---|---|---|---|
| A. Today's cache (linkc) | a header's copy for one context, linked to the copies of the headers inside it | PCMs in a shared region, read by separate compile processes | first builds 2.09x, the same build again 2.65x (geometric mean, instructions). -j64: 1.22x (wall, 2026-11-03) | nothing |
| B. A faster PCM (DECISIONS.md 2.24, options A to C) | as A | PCMs with one name index, macros outside the PCM, checks outside the PCM | at most about 0.9 s of redis's 2 s of loading | nothing |
| C. The compile server, as built (COMPILE_SERVER_DESIGN.md) | the text between two directives | declarations kept in one process | first builds 2.26x, the same build again 2.37x (geometric mean, instructions) | parallel compiles that share one store. Failures that repeat without the compiles before them |
| D. Live copies (this design) | as A | declarations kept in one process | at most 2.44x wall on first builds (A less loading and writing) | as C |

Prior systems (DECISIONS.md 2.24):
- Bothner's GCC server and Zapcc kept parsed headers in one process. Zapcc
  turned its caching off for C.
- PPH streamed per-header trees into files and merged them, as B does.
- GCC and MSVC precompiled headers are memory images, one per compile.

## 3. The design

### 3.1 One process

The server's process model stays as it is:

- one `clang -cc1` with the plugin action, a socket, and one compile at a
  time (no concurrency: user, 2026-11-06)
- the driver forwarding, the refusals, and the JSON reply
- for each configuration (working directory, language options,
  `-fno-builtin` names, target), memory spaces side by side, each with its
  own FileManager, SourceManager, ASTContext and store. A main file stays in
  the space of its first compile (`space-share`)
- at each compile start, the reset and the undo (`Server::reset`)
- the instruction counter (`instructions()`), for each request.

The cache's daemon goes away: its store and its ask decisions move into the
server process. Lookups and keeps are calls, not socket messages.

### 3.2 What a copy holds

| Part | From | Job |
|---|---|---|
| Metadata: what the header read (macros, definedness tests, outside declarations, include searches, pragma state) | the cache (`HeaderDependencySet`), unchanged | the check |
| Outside objects: for each declaration it names from outside its own text, the object that the name found then | new | the check (3.3) |
| Its own declarations in text order, with the place of each `#include` inside it | the server's `Version`: `Binds`, `TopLevel` | the restore |
| Links: the copy used at each of those `#include` lines, or none where the include was skipped | the cache's links (linkc) | the restore, in text order |
| State its parse left that later code can change: attributes and types of its bindings, Sema's lists, sections, Used and referenced bits, the token count | the server's `Version` | the restore and the undo |
| Its macro and pragma effects, and its diagnostics | the cache's replay | the hit |

**Keep, in place of a write.** At a miss, the compile reads the header as
text, as now. Where the cache now writes a PCM (the first declaration after
the header ends), the store keeps the record above. Nothing is serialized.
A keep costs no write, so every header that a compile reads as text is
kept, as eager writes do now (`CLANG_HEADER_CACHE_FIRST_READ=1`). The
recording of the metadata still costs time while the header is parsed. In
redis, the median read took 362 us with recording, and 239 us without it
(2026-11-04).

### 3.3 A lookup

At each `#include`, the hook asks the store for the copies of the header.
For each candidate, in order:

1. The cache's checks, unchanged (`matchesLiveState`).
2. The provider rule: each outside declaration that the copy names must
   be, in this compile, the same object as when the copy was kept. The
   check looks up each name, as the check of fingerprints does now, and
   compares two pointers. Example where it holds: compile A read `stdio.h`
   from copy 12, then kept `server.h` as copy 40, whose functions take a
   `FILE *`. Compile B restored copy 12, so the name `FILE` finds the same
   object in B, and copy 40 fits. Example where it fails: compile C read
   `stdio.h` as text, so `FILE` finds another object in C. Copy 40 does not
   fit in C, though every fingerprint matches.

The same object has the same fingerprint, so the provider rule makes the
fingerprint check of those declarations redundant. P1 (section 5) keeps
both. L1 (section 6) can drop the fingerprints.

Today's copies need no provider rule: they name outside declarations by
name (DECISIONS.md 2.18), and the reader binds each name to the compile's
own declaration. A kept declaration points at fixed objects. This rule is
the main risk of the design (section 5).

**A header without an include guard, included twice.** Each reading is its
own copy, with its own objects, as the server's `keys=reading` keeps each
reading. A compile can restore both, in order. Today a PCM copy loads only
once in a compile. That is the cause of 47 of the 48 misses in redis's
second build with eager writes (`loaded-copy-not-newest`).

### 3.4 A hit

1. The hook skips the header's text, as a hit does now.
2. In place of a PCM module, it queues the server's restore token
   (`__bothner_restore`, through `ServerLoader`).
3. The restore links the copy's declarations into the compile in text
   order, with each link's declarations at its `#include`. The server's
   `restoreNext` does each step: redeclaration chains (`repairChain`), the
   scope, Sema's lists, the special typedefs, the extern "C" context,
   sections, and the Used and referenced bits.
4. The cache's replay makes the macro and pragma effects, and gives the
   diagnostics. The kept macro definitions go back into the preprocessor's
   table, as the server's option 3 does (`MacroEffect`). Nothing decodes a
   macro history.

### 3.5 What goes, and what stays

| Goes | Stays |
|---|---|
| The PCM writer for copies (`PartExporter`'s write), and name references (2.18) | The metadata and every check |
| The PCM reader for copies (`load`, `loadTree`), and its macro histories | The ask decisions, with costs measured in the process |
| The shared region, the daemon process, and the lookup and push messages | The replay of macro and pragma effects, from kept definitions |
| The ask-list's fixed cost of a round trip | The statistics lines and the benchmark modes |

## 4. Limits

- **One compile at a time.** A parallel build needs one server for each
  job, and each server has its own store. This design does not try to share
  them.
- **The store lives only as long as the process.** The same build again uses
  the same process, as the benchmark does now.
- **2 GiB of source locations for each memory space.** FFmpeg needs 4
  spaces for two builds, git needs 2.
- **Repeatable failures.** A failure can depend on the compiles before it.
  The server's request log keeps their order, so a fresh server can run the
  same compiles again.
- **Private members of Clang classes.** The server reaches them with
  explicit template instantiation of member pointers (`Rob`). A Clang update
  can break this.

## 5. The riskiest assumption, and a prototype

**Assumption.** With the provider rule, copies serve about as many headers
as today's copies do.

**Why it could fail.** One miss makes new objects. Each copy that names
them then misses too, and makes more new objects. Today, by-name references
stop this chain: a copy fits wherever the names match.

**Prototype P1: the provider rule in today's cache.**
- A switch adds the provider rule to today's checks, as one more reason to
  reject a candidate:
  - at a write, the copy records where each outside declaration came from.
    That is the loaded copy that owns it, or the copy this compile wrote
    for the header that declares it, or none
  - at a check, the rule compares each of them with this compile's own
    source of that declaration.
- A rejected candidate gives a miss, so the chain of misses happens as it
  would in the design.
- The runs: the four workbench projects, the first build and the same
  build again. Every header that a compile reads as text gets a copy
  (`-ask-cutoff-us 0` and eager writes). Counts only, no timing.

**Gate.** From P1's hits and misses and the measured costs, estimate the
build time of the design. Go on only if the estimate beats the server's
measured speed on the geometric mean of the four projects. The user
decides at the gate.

**Prototype P2 (lower risk): the cost of a restore.** A timer around the
server's `restoreNext` gives the restore time for each declaration, beside
the cache's load time for each hit in the same project. The gate's estimate
needs it.

## 6. Steps after the gate

| Step | What | Check |
|---|---|---|
| L1 | The cache's hook, checks and store inside the server process. Misses keep, hits restore. No links | redis: every object and every diagnostic the same as plain clang's |
| L2 | Links: a hit restores its tree in text order. Every text read kept | redis and git: objects and diagnostics |
| L3 | The four workbench projects, the first build and the same build again, memory spaces | objects and diagnostics |
| L4 | The benchmark against today's cache and the server: instructions while the host is loaded, wall time on idle cores | the same objects check on the benchmarked binary |
