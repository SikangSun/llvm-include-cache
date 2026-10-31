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
