# Header cache: correctness checks for the artifact evaluation

These scripts check that the header cache gives the same object files as
plain clang on seven C projects, in a first build and in a rebuild on the
same cache. They need an x86-64 Linux machine with cmake, ninja, git,
Python 3, the projects' build tools (make, autoconf, automake, libtool,
pkg-config), about 16 GB of memory and 20 GB of disk (estimates).

Run them in order:

| Step | Command | What it does | Time on 16 cores (estimate) |
|---|---|---|---|
| 1 | `BUILD=$HOME/hc-build ./build.sh` | Builds clang and the cache daemon from this checkout (release, X86). Prints `BIN`. | about 1 hour |
| 2 | `BENCH_ROOT=$HOME/hc-src ./fetch.sh` | Clones redis, nginx, curl, git, postgres, cpython and FFmpeg at the paper's commits. | about 10 minutes |
| 3 | `BIN=... BENCH_ROOT=... BENCH_LOGS=$HOME/hc-logs ./record.sh` | Builds each project once with plain clang, logs every compile, and copies the built trees to `/dev/shm/bench-src`. | about 30 minutes |
| 4 | `BIN=... BENCH_ROOT=... BENCH_LOGS=... ./check.sh` | Compiles every file with plain clang and with the cache, then with the cache again on the same daemon, and compares every object file byte for byte. One line per project. | about 45 minutes (nginx alone: 1.4 minutes, measured) |
| 5 | `BUILD=$HOME/hc-build ./tests.sh` | The header cache's unit tests, then Clang's Modules and PCH tests. | about 30 minutes |

`check.sh` ends with `all objects the same` when every object of the cache
matches plain clang's and no compile failed. A project's details are in
`check-results/<project>.log` and `.json`.

`/dev/shm` is emptied at a reboot. Run `../dram_sources.sh` with the same
`BENCH_ROOT` again before `check.sh` after one.
