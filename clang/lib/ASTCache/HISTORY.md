# Header cache: what was built, why, and what it gained

This is the short history of the header cache on branch `header-cache`, as of
2026-10-23. It covers what the cache does, the decisions that shaped it, the
speed each stage reached, what was tried and dropped, and how to run the
benchmarks. REPORT.md and DECISIONS.md have the full record behind each
point (section numbers below refer to DECISIONS.md).

## What the cache does

A C compile spends most of its front-end time parsing headers that other
compiles of the same build parse again. The cache keeps parsed headers and
serves them to later compiles:

1. A compile asks a daemon, over a socket, for the header it is about to
   include. Only headers on an *ask-list* are asked about (below).
2. The daemon returns the stored *copies* of that header. Each copy is a Clang
   module file (PCM) plus *metadata*: everything the header's parse read from
   outside itself — which macros were defined or how they expanded, which
   file each `#include` found, which outside declarations it used, which
   warning state was in force.
3. The compile checks a copy's metadata against its own state. If everything
   matches (a *hit*), it loads the module instead of parsing the text and
   replays the macro definitions the header left behind. If no copy fits (a
   *miss*), it parses the text, and then writes a copy of what it parsed and
   pushes it to the daemon for later compiles.

A *request* is one compile including one header that is on the ask-list.
Nothing about the build system changes: compiles run as before, with an
environment variable naming the daemon's socket and
`-fmodules -fno-implicit-module-maps` on the command line.

## Where it stands

First builds (empty cache), front end only (`-fsyntax-only`), six projects,
the cache's default mode (top-level folding). Sources read from memory, plain
clang run as a user runs it; plain and cached compiles interleaved file by
file in one run, 2026-10-23.

| Project | Compiles | Plain clang (s) | With the cache (s) | Speed-up | Requests served |
|---|---|---|---|---|---|
| postgres | 1,341 | 138.84 | 88.68 | 1.57x | 72.5% |
| git | 567 | 95.67 | 29.96 | 3.19x | 77.7% |
| cpython | 401 | 71.58 | 44.58 | 1.61x | 73.5% |
| curl | 479 | 32.15 | 14.39 | 2.23x | 90.9% |
| redis | 117 | 12.64 | 8.71 | 1.45x | 62.8% |
| nginx | 126 | 11.04 | 3.93 | 2.81x | 97.0% |
| **Geometric mean** | | | | **2.05x** | |

Full builds with each project's own flags (`-O2`, run beside the front-end
runs, so their times are rough): 1.11x (redis) to 1.57x (git). Every object
file is byte-identical to plain clang's in all six.

Where the cached build's time goes, as a share of plain clang's front-end
time (top-level folding):

| Project | Saved | The .c files' own lines | Parsing missed headers | Never asked | Writing copies later used | Writing copies never used | Serving hits |
|---|---|---|---|---|---|---|---|
| postgres | 36.1% | 20.4% | 24.7% | 2.0% | 3.3% | 7.4% | 5.2% |
| git | 68.7% | 12.4% | 9.5% | 0.5% | 2.4% | 2.1% | 4.1% |
| cpython | 37.7% | 18.6% | 24.1% | 5.5% | 3.3% | 6.2% | 4.4% |
| curl | 55.2% | 31.9% | 4.4% | 0.9% | 1.5% | 0.6% | 5.0% |
| redis | 31.1% | 32.8% | 17.7% | 6.1% | 3.3% | 5.4% | 3.1% |
| nginx | 64.4% | 27.1% | 0.7% | 1.0% | 0.5% | 0.0% | 6.0% |

The .c files' own lines are a floor no header cache removes: they cap redis
at about 3x and git at about 8x.

## How the speed-up grew

Front end, six first builds, as measured at the time.

| Date | What changed | Speed-up over plain clang |
|---|---|---|
| 2026-10-07 | First whole measurement: hits replay macro effects, the ask-list, copies written by the compile that misses, a copy holding every header below it, outside declarations by name | 1.17x to 1.44x per project, about 1.29x (machine shared with other jobs) |
| 2026-10-15 | Replay skips effects already in force (I8); copies written from their own source range (I5); `#include` searches once per build (I9); metadata in shared memory (I3); copies without what only tools read | 1.68x (1.64x in total time) |
| 2026-10-17 | Files checked once per build; names never made macros not looked up (I14); last-used copy checked first (I15); buffers compressed once per compile (I16) | 1.76x in total time |
| 2026-10-21 | Warning pragmas compared by what they change (2026-10-19); measured beside the designs dropped below | 1.95x |
| 2026-10-23 | nginx's main headers asked for again (timing fix); dependency files complete; baseline corrected (below) | 2.05x |

Rows before 2026-10-23 are not exactly comparable with the last. Plain clang
then read the sources over NFS and ran with the cache's modules flags, which
moved the speed-ups of the five projects other than nginx by -5% to +1%. From 2026-10-01 to 2026-10-23 a
timing bug also kept nginx's most included headers off the ask-list (nginx
1.85x then, 2.81x now).

## The decisions, why, and their effect

**Record what the parse actually read, from the real compile.** The tracker
observes the compile through preprocessor callbacks and a few Sema entry
points, keeping a frame per header (§1). Anything it cannot account for
"poisons" the header: it is never cached (§1.6). *Why:* a copy may be served
only where parsing the text would give the same result, and recording what
the parse read is exact without a model of C. *Effect:* the correctness bar
— identical object files — has held in every full build since 2026-10-07.

**Copies are Clang module files.** The compile loads a copy as an explicit
module from memory (§2.2, §2.8). *Why:* Clang's reader already loads
declarations lazily and merges them into a compile that has state of its own;
a heap image, as GCC's and MSVC's precompiled headers are, cannot be merged.
*Effect:* loading is Clang's reader, fast to open; each loaded copy adds about
1 ms of later cost, mostly name lookups searching every loaded copy (I1 would
remove most of it). Cached compiles need `-fmodules
-fno-implicit-module-maps`; plain clang without them is 0.7-1.8% faster, and
the benchmarks charge that to the cache.

**The compile that misses writes the copy (T5).** At a miss the compile
parses the text as usual and writes the copy from its own state (§2.17).
*Why:* the first design built each copy in a separate compile replaying the
includer's context, and that cost too much — 68 module files, 460 MiB and
128 s for one qemu file (DECISIONS.md §3). *Effect:* a first build pays off;
writing costs 0.5% (nginx) to 10.8% (postgres) of plain clang's time, and
copies never used are 0-7.4% of it.

**Ask only where a hit can pay (T7).** The daemon keeps an ask-list in
shared memory: a header goes on it once a compile reports its first read
took at least 226 us, one load's cost (§2.14). *Why:* 77% of header parses
cost less than a load, yet the other 23% hold 98.5% of header parse time;
asking costs a round trip. *Effect:* compiles ask only about the headers
that matter, with no socket traffic for the rest.

**A copy holds its header and every header below it (top-level folding,
T6).** Inside a header being written nothing is asked or served (§2.23).
*Why:* copies per header multiply loads, and every load costs time when made
and later. *Effect:* redis went from 2,687 hits per build to 302 for the same
headers, and from about 1.0x to 1.20x when this was built. A copy needs
another version wherever the headers inside it differ, mostly by which were
already included; the two designs that tried to fix that are below.

**Dependencies are as narrow as the parse allows (T1-T4).** A copy records
how each identifier its parse considered expanded (T1), only definedness
tests whose answer came from outside the header (T2), only the `defined()`
tests an `#if` needed for its result (T3), and outside declarations by name
with a fingerprint of their layout (T4). *Why:* every recorded condition
that does not matter turns a hit into a miss. *Effect:* for example, redis's
rejections over `NULL` went from 210 to 0 (T3), and copies serve compiles
that parsed the includer's declarations as text (T4).

**A hit replays only what loading does not already do (I8).** A hit
re-applies the `#define`s and `#undef`s the header left behind; 75-93% are
already in force and are skipped. *Effect:* 35.1 s less over six full
builds.

**The cache's own costs, cut one measured step at a time.** Each change was
measured against the build before it, side by side:

| Change | What it removed | Measured |
|---|---|---|
| I5: write a copy from its own source range | Scanning the compile's whole state per copy | 5.2 s less over six full builds |
| I9: `#include` searches, then file checks, once per build | Every compile redoing them | 6.3 s less; file checks then -90% |
| I3: metadata in shared memory, read in place | Encoding and decoding metadata per query | 21.5 s less |
| Copies without tool-only records | 13% of stored bytes | 12.4 s less |
| I14: names never made macros not looked up | Macro lookups | git -1.26 s (4% of its front end) |
| I15: the copy used last checked first | Checks of copies then rejected | -30% copies checked on git |
| I16: buffers compressed once per compile | Repeated compression in the writer | git writing -0.93 s |

**Correctness checks, each added after a real failure.** A copy is refused
if a file it holds changed (size and modification time), if any of its
`#include`s would find a different file here, if `__FILE__` would differ,
or if the warning state differs in a way that changes what it emits;
`#pragma push_macro`/`pop_macro` follow a fixed rule; nothing is written
after an error, or in a compile that writes a precompiled file (§3 and
REPORT.md's Q list).

**Fixed on 2026-10-23.** A compile reported a header's time where it first
left it; `ngx_core.h` includes headers that include it back, so its time
read as a few microseconds and it never reached the ask-list (nginx 1.85x to
2.81x). And a served copy's headers were missing from make's dependency files
(`-MD`), so editing one of them rebuilt nothing; they are now listed, file
for file as plain clang lists them.

**How it is measured.** First builds only, each with a fresh daemon. Plain
clang and the cache interleaved compile by compile in one run, the order
alternating. Sources in memory (`/dev/shm`), as is the daemon's store. One
compile at a time per project, on its own core, the daemon on another, their
hyperthread siblings idle. Summaries are geometric means over projects, so
each counts equally. Run-to-run noise is about +-2%.

## Tried and not kept

| Design | Result | Why it stopped |
|---|---|---|
| A separate compile builds each copy, replaying the includer's context | 128 s and 460 MiB for one qemu file | Replaced by writing the copy at a miss |
| Linked copies: one copy per header, loaded together and merged (the shape of Google's pre-parsed headers) | 1.00x against 1.95x | Each extra copy loaded costs more than parsing it |
| Fold served copies into new copies | 1.37x against 1.89x | Writing went from 30 s to 98 s |
| T15: independent headers left out of copies, with copies of their own | 1.60x against 1.70x | More loads and asks; branch `t15-attempt` |
| 1 Miss Folding: write a copy only the second time it would be written | 1.90x against 1.95x | Saved writes in postgres, lost hits elsewhere |
| Sections (T17): headers inside a copy held as parts skipped by include guard | 1.94x for both | No gain; `CLANG_HEADER_CACHE_SECTIONS=1` |
| Bases and pieces: sections swapped for other versions of them | 2.04x for both | postgres's misses fell from 27.5% to 18.5% of requests, but the extra checks and bookkeeping cost as much; `CLANG_HEADER_CACHE_PIECES=1` |
| A per-header ask cutoff from each header's measured hit cost | No faster than the fixed cutoff on redis | Built, off (`-drop-costly`) |
| I11: enter all of a copy's names when it loads | -1.2% to +1.6% | Dropped |

A compile server that keeps parsed headers in memory (Bothner's GCC server,
Zapcc) was ruled out without building it: the build would have to run its
compiles inside one long-lived process.

## Running the benchmarks

The harness is in `clang/utils/header-cache-bench/`. It records each
project's build once, then replays the recorded compiles: plain clang and
each cache mode, interleaved compile by compile.

1. **Build clang** from this branch: release, assertions off, X86 only, as
   ours (`-DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS=clang
   -DLLVM_TARGETS_TO_BUILD=X86`), then
   `ninja clang clang-header-cache-daemon`.
2. **Check out the six projects** in one directory (`BENCH_ROOT`), at:

   | Project | Commit | Date |
   |---|---|---|
   | redis | b1b5c85 | 2026-08-26 |
   | nginx | 5f54125 | 2026-08-26 |
   | curl | 0b5759c | 2026-08-26 |
   | git | 010afd3 | 2026-08-07 |
   | postgres | 086f6f1 | 2026-08-10 |
   | cpython | fe3a26f | 2026-08-26 |

3. **Record the builds**: `BENCH_ROOT=... BENCH_LOGS=... CCLOG_CLANG=<build>/bin/clang ./record_builds.sh`.
   It configures and builds each project through `cclog`, which logs every
   compiler call. Configure flags are in the script.
4. **Copy the built trees to memory**: `BENCH_ROOT=... ./dram_sources.sh`
   (to `/dev/shm/bench-src`; again after any reboot).
5. **Run**: `BIN=<build>/bin BENCH_ROOT=... BENCH_LOGS=... OUT=results/run1 ./run_six.sh`.
   It prints each project's speed-up and the geometric mean. Add
   `FULL_CORES=...` and `FULL_DCORES=...` (six cores each, on cores the
   front-end run does not use) to also build with each project's own flags
   and compare every object file with plain clang's.

Settings that matter:

- **Cores**: `CORES` (one compile core per project) and `DCORES` (one daemon
  core per project), six each. Keep their hyperthread siblings idle, and
  nothing else running.
- **Modes**: `MODES="top"` is the default cache. `sections` and `piecesB`
  are the opt-in designs above; `link` and `miss1` the dropped ones.
- **Timing level**: `LEVEL=1` for timing. `LEVEL=2` logs every request and
  copy, for analysis, at some cost.
- **Baseline**: plain clang runs without the modules flags. To reproduce the
  earlier baseline, set `PLAIN_MODULES=1` and `SRC_ROOT=in-place`.
