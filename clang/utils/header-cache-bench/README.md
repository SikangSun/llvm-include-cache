# Header cache benchmarks

Records six projects' builds once, then replays their compiles with plain
clang and with the header cache, interleaved compile by compile, and reports
each project's front-end speed-up and the geometric mean.

- `cclog`: compiler wrapper that logs each call while recording.
- `record_builds.sh`: configures and builds the six projects through it.
- `dram_sources.sh`: copies the built trees to `/dev/shm`, where they are read.
- `modes_bench.py`: one project, plain clang and the cache modes.
- `run_six.sh`: the six projects at once, one core each, and the summary.

Steps, project versions and settings: `clang/lib/ASTCache/HISTORY.md`,
"Running the benchmarks".
