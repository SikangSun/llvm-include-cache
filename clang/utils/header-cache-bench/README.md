# Header cache benchmarks

Records six projects' builds once, then replays their compiles with plain
clang and with the header cache, interleaved compile by compile, and reports
each project's front-end speed-up and the geometric mean.

- `cclog`: compiler wrapper that logs each call while recording.
- `record_builds.sh`: configures and builds the six projects through it.
- `dram_sources.sh`: copies the built trees to `/dev/shm`, where they are read.
- `modes_bench.py`: one project, plain clang and the cache modes. `INSTR=1`
  counts each compile's instructions with `perf/perfc`.
- `run_six.sh`: the six projects at once, one core each, and the summary.

## Folding and merging on and off

Linked copies (`CLANG_HEADER_CACHE_LINK=1`, mode `linkc`) fold and merge at
two times: during the first build, and in the offline step. The offline step
runs between a build and the next one. Each of the four switches on and off
by itself.

| What | `linkc` | On | Off |
|---|---|---|---|
| First-build folding: a header that takes less than the ask cutoff to read gets no copy of its own. Its lines go into the copy of its includer | on, 226 µs | daemon `-ask-cutoff-us <µs>` | daemon `-ask-cutoff-us 0`, mode `linkcut0`: every header gets its own copy |
| First-build merging: a hit on a header that the five counters pick merges the copies it loads into one copy | off | daemon `-merge-picks`, mode `linkcp` | leave out `-merge-picks` |
| Offline folding: the step's top and fold decisions for the next build | on in `linkco` | `offline.py` as it is | `offline.py --no-folds`, modes `linkcom` and `linkcon` |
| Offline merging: the step merges each tree of copies hit twice or more into one copy | on in `linkco` | `offline.py` as it is | `offline.py --no-merges`, modes `linkcof` and `linkcon` |

The offline step with `modes_bench.py`: run with `PASSES=2`, and set
`RECORD_DIR` (a folder for the first build's record) and `DECISIONS` (a file
for the step's decisions). The run deletes both first. `linkco` uses the two
names as they are. `linkcof`, `linkcom` and `linkcon` add `-<mode>` to them,
so the four modes can run in one run:

```
RECORD_DIR=/tmp/rec DECISIONS=/tmp/dec PASSES=2 \
  MODES="linkco linkcof linkcom linkcon" BIN=<build>/bin BENCH_LOGS=<logs> \
  BENCH_ROOT=<root> python3 modes_bench.py redis redis.json
```

| Mode | Offline folding | Offline merging |
|---|---|---|
| `linkco` | on | on |
| `linkcof` | on | off |
| `linkcom` | off | on |
| `linkcon` | off | off (the step still runs and writes an empty decisions file) |

The offline step by hand: start the daemon with
`-offline "python3 -I <checkout>/clang/utils/header-cache/offline/offline.py"`,
`-offline-record <folder>` and `-offline-decisions <file>`. The daemon adds the
folder and the file at the end of the command, so the switches go inside the
quotes: `-offline "python3 -I <checkout>/clang/utils/header-cache/offline/offline.py --no-merges"`.
With the daemon running, the step also runs by itself:
`offline.py [--no-folds] [--no-merges] <folder> <file>`.

- `--no-folds` turns off the top decisions too. They come from the same rule,
  which runs after the folds.
- In the same build again, a fold decision acts only where a compile writes a
  copy, so few of them act. A rebuild after an edit (`CHANGE_CMD`, below)
  writes more copies.
- The step writes one JSON line to `<file>.log`, and `modes_bench.py` prints
  it: `folding` and `merging` say which were on, then the counts of folds,
  tops and merged trees.

## Correctness

- `modes_bench.py` with `KIND=full`: every compile makes an object, and each
  cache mode's objects are compared with plain clang's.
- Rebuild after a change: `PASSES=2 CHANGE_CMD=invalidation/redis_change.sh
  CHANGE_UNDO=invalidation/redis_undo.sh KIND=full modes_bench.py redis ...`.
  Between the build and the rebuild, `redis_change.sh` changes `sds.h` and
  `server.h` and touches `zmalloc.h`; the rebuild uses the same daemons, and
  its objects are compared with plain clang's too.
- `par_check.py`: a parallel build (`make -jN` order) through one daemon, then
  plain clang; failures and objects compared.
- The unit tests: `AllClangUnitTests --gtest_filter='AskListTest.*:ASTCacheDependencyTrackerTest.*:CacheProtocolTest.*:ConcurrencyTest.*:DaemonTest.*:DeclFingerprintTest.*:DependencySerializationTest.*:EndToEndTest.*:ExportTest.*:HitTest.*:LoadTest.*:PartExportTest.*:ProducerTest.*:SymbolDependencyTest.*'`.

## Profiling

- `perf/perfc.c`: runs a command and prints its hardware counters, children
  included (`cc -O2 -o perf/perfc perf/perfc.c`).
- `perf/psample.c`, `perf/psum.py`, `perf/pstack.py`: samples a compile's
  instruction pointer (and call stack) every N instructions, then a flat or
  call-stack profile by function.
- `par_bench.py`: wall time of parallel first builds at several `-jN`.
- `ddg_run.py`: every compile of a project with `CLANG_HEADER_CACHE_DDG`, which
  writes each header's includer and each macro and declaration it used from
  outside, with the header that provided it.

## Analysis

The scripts behind the numbers in `clang/lib/ASTCache/HISTORY.md`. They read
the result folders named in them.

- `analysis/merge`: merge count sweep, each merged copy's saving, guard+ runs.
- `analysis/cutoff`: the `-ask-cutoff-us` sweep, and its dashboard tables.
- `analysis/misses`: misses per request by cause.
- `analysis/redundancy`: repeated header work and format simulations, from
  `ddg_run.py` traces.

Steps, project versions and settings: `clang/lib/ASTCache/HISTORY.md`,
"Running the benchmarks".
