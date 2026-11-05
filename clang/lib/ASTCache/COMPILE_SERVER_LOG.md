# Bothner's compile server as a baseline: work log

Everything done on this baseline from 2026-10-30 to 2026-11-03, with every
decision and why, and where each piece lives. Branch `apple-compile-server`
(from local `bothner-model`).

Companion documents:
- COMPILE_SERVER_PLAN.md: the model (D1-D7), its decisions and status.
- COMPILE_SERVER_DESIGN.md: the real server's design, S0's results
  (section 6) and decisions 1-9 (section 8).
- This log: the order of events, the implementation choices below the
  design's level, the verification history, the data, and what is open.

## 1. Where things are

| Path | What |
|---|---|
| `clang/lib/ASTCache/COMPILE_SERVER_PLAN.md` | The model's plan, decisions D1-D5, status D1-D7 |
| `clang/lib/ASTCache/COMPILE_SERVER_DESIGN.md` | The server's design; S0 results; decisions 1-9 |
| `clang/utils/header-cache-bench/bothner/BothnerTrace.cpp` | Plugin: timing mode and record mode. The record format is documented at the JSON writer (search "in record mode also") and in the enums `SV_*`, `SEP_*`, `VIEW_*`, `PF_*` |
| `.../bothner/simulate.py` | The model (R0/R1/R2, V0/V1/V2, option 3, ideal) and the server's rule (`simulate_server`), tables |
| `.../bothner/trace_run.py` | Runs the plugin over all redis compiles, timing (best of 3) or record mode |
| `.../bothner/compiles.py` | The recorded compiles; `SRC_ROOT`, `MODEL_MOD` |
| `.../bothner/recompile.py` | The header cache's recompilation beside plain clang |
| `.../bothner/blank.py` | Checks the time model by blanking fragments (D6, D7) |
| `.../bothner/objects.py`, `overhead.py` | Plugin changes no object; its overhead |
| `.../bothner/fragcount/` | FragCount plugin and expected counts (with and without modules flags) |
| `.../bothner/tests/` | `attribution.py`, `record.py`, `simulate.py` (model); `server_record.py`, `server_simulate.py` (S0) |
| `.../bothner/verify/` | The second, independent simulator (`reimpl2.py`), its comparer, synthetic builds, sensitivity runs, and the review notes |
| `.../bothner/results/` | The S0 and D7 reports and per-compile decisions (copies of the gitignored `out-*` files) |

Not in git (gitignored `out-*`, about 900 MB): the records and timings.
Section 7 says how to remake them.

## 2. Ground rules followed

- **Machine:** i7-9700, 8 cores, 30 GB. Turbo off
  (`/sys/devices/system/cpu/intel_pstate/no_turbo` = 1, checked). Compiles
  run serially and pinned (`CORE`, default 3); sustained full load has
  crashed this machine.
- **Sources from DRAM** for every timed row: `/dev/shm/bench-src`
  (`SRC_ROOT`), as the cache's runs.
- **Other servers are not used** (another session and other benchmarks run
  there).
- **Nothing committed or pushed without being asked.** This branch is the
  first push of this work.
- **Long runs write notes as they go** (`verify/notes/`), because sessions
  hit usage limits three times.

## 3. Timeline

**D1-D6 (10-30 to 10-31), the model.** Committed on `bothner-model` (6
commits, from `6841e8764` PPH to `9629498a5` D6). COMPILE_SERVER_PLAN.md
section 8 has each step's results: the timing plugin and its attribution
check (D1); go/no-go (D2: go); the record plugin (D3-D4); the full simulator
(D5); blanking and the object check (D6).

**D7 (10-31), recompilation harness and tables.** COMPILE_SERVER_PLAN.md,
"D7". DRAM sources everywhere; the model's compiles as plain clang; one build
ID per repetition; the bootstrap bound `SAVED_SCALE=1.007`; option 3 rows;
the table in `results/d7-table-strict.md`. Headline: the cache 2.69x, the
model (option 2, R1, macro lists) 1.94x, R0 1.66x.

**11-01, the real server instead of the model.** The baseline is to be a
real implementation of his server, run on this machine ("realize what he
did"). COMPILE_SERVER_DESIGN.md was written, reviewed three times
independently (Clang mechanics, fidelity to the paper, evaluation), and
revised. Direction from the user: the per-compile state is reset per
compile, not per server.

**11-01, decisions 1-5 taken as recommended** (design section 8): option 2,
then option 3 and fragment groups; R0 with the checks of 2.6, the model's R1
kept as an estimate; accept Used bits not undone and the lost diagnostic
pragma state of a restored declaration's old header; fix the nesting
observer for the cache and the model, rerunning both; S0 first as a
go/no-go.

**11-01 to 11-03, S0.** Records extended, the server's rule added to the
simulator, three record runs over all 187 compiles, two adversarial review
rounds plus tests, a second simulator. Result: go, with decisions 6-9 open.

## 4. Every decision

### Taken

| Where | Decision | Why |
|---|---|---|
| PLAN D1 | Model first (option A) | A model gives the bound in days; a server takes weeks |
| PLAN D2 | R1 as the model's headline rule; R0 a finding, R2 the favourable bound | R0 as written re-parses whatever a re-parse touched |
| PLAN D3 | Macro checks: "obvious solution" plus expands-to (V1) | §8.2's bits and none reported beside, with unsafe counts |
| PLAN D5 | PPH committed (`6841e8764`) before this work, on `bothner-model` | Clean history |
| PLAN D7 | Model compiles without modules flags | Plain clang is what his server would compile; `-fmodules` changes `__stddef_*.h` |
| PLAN D7 | `SAVED_SCALE` from a bootstrap interval, not the worst compile | One compile's ratio is wall-time noise |
| Credit | 1.65 ms compiler start-up per compile | Measured 1.57-1.68 ms; his driver still ran (p.22-23) |
| Design 1 | Option 2, then option 3 and groups | His own remedies for the largest loss |
| Design 2 | R0 with 2.6's checks in the server; R1 only as an estimate | R1 needs object identity a real server lacks (`struct timestamp` twice) |
| Design 3 | Accept Used bits leaking and the lost diagnostic pragma state | No header change; plain clang prints nothing on redis anyway |
| Design 4 | Fix the nesting observer (2.9), rerun cache and model | It reads two open positions as boundaries |
| Design 5 | S0 before any server code | Go/no-go on numbers |
| User | Per-compile resets, not per server | |

### Open (S0 raises them; design section 8, items 6-9)

| # | Choice | Recommended | Numbers |
|---|---|---|---|
| 6 | Groups may hold `#define`/`#undef` lines, replayed in order | Yes; §11.2's groups reported beside | 2.17x against 1.66x |
| 7 | 2.8: refuse a fragment that leaves an internal/inline function undefined, or reuse and rerun the compile if one stays undefined (fallback) | Refuse, per fragment | Same rows on redis |
| 8 | Keys by location (his) or per reading | Location for the server row; per reading as a further row | 2.17x / 2.64x |
| 9 | Uses from AST references, or every identifier token's name | Token names | 2.17x / 2.09x |

### Implementation choices below the design's level (S0)

- **Fixed observer in the records.** The real fix (2.9) changes the parser;
  the records approximate it from outside. A directive is a boundary when
  the old observer says so and: there was no previous token; or the last
  token was a pragma annotation or on its line; or the last token was `;`
  with Sema's context the TU; or `}` with the context a function. Not before
  a file-scope `{`. Not inside a function prototype's scope, which covers
  K&R parameter declarations (`INFO_PROTO`, from `Sema::getCurScope()`).
  On redis the fixed and old observers give identical decisions.
- **Pending parser pragma.** Marked from the `#pragma` directive itself
  (when its handler queues the annotation token), until the token has been
  handed out; design 2.4 was corrected to say so.
- **Macro arguments.** A fragment that starts or ends inside a function-like
  macro's arguments is refused (`SV_ARGS`). Invocations are recorded where
  written, so `#define CALL FOO` then `CALL(` counts.
- **Never-reusable builtins** count in any member of an option-3 fragment or
  group, even a member with no tokens (a lone `_Pragma("push_macro")`).
- **2.8 per fragment:** a fragment is flagged only if it declares an
  internal or inline function that it does not define itself. Per
  declaration, the rule refused 82 fragments that define the function right
  after its prototype (`keymeta.h`) and cost 0.29x.
- **Fallback variant ("fb"):** reuse such fragments; if a compile ends with a
  restored one still undefined (`SV_UNDEFINED_END`), undo its store changes
  and rerun it without reuse.
- **Main file:** a unit that uses a main-file declaration, or whose earlier
  declarations include one, is refused (`flags & 3`); main-file declarations
  are unique per compile run in the per-name sequences.
- **Per-name declaration sequences:** a version stores, for every name it
  declares or references, the declarations of that name made earlier in the
  compile as (key, version). The negative, deps and predecessor checks
  compare those.
- **Groups (2.7):** a maximal run of one header entry's units joined where
  the parser is nested at the boundary, with only conditionals between;
  reused only when the same members are processed ("conditionals").
  groups\* may also hold `#define` lines; their macro list leaves out names an
  earlier member defines.
- **Option-3 macro lists:** a name the unit's own `#define`/`#undef` sets
  before using it is left out (replayed); names of empty members are kept,
  since where they fall among the `#define` lines is not recorded.
- **Key:** the simulator keys by first-token offset; records also carry the
  post-directive key, which the second simulator uses. They map one to one
  on redis.
- **Time:** as the model's: plain clang less the reused fragments' measured
  time, restoring free, start-up credited. Replayed `#define` lines are
  counted as saved; the "no #define" column counts them as not saved.
- **Oracles:** "unsafe" is a reuse whose oracle (tokens, pragma state,
  dependencies' oracles, recursively) differs from what this compile would
  have; it should be 0. "unsafe2" uses an oracle that also depends on every
  name an identifier token spells; it over-counts (names in other roles).
  Under per-reading keys only deps on the same reading are left out.
- **Check order** (as reported): nested, param, main, builtin, undefined,
  macro args, pending, repeat, new, conditionals, uses, macro, pragma,
  negative, deps, predecessor, unsafe, fallback.

## 5. S0 in detail

**Record mode gained** (BothnerTrace.cpp): per fragment, fields 17-25 (fixed
nesting before/after, full pragma state, `SV_*` flags, declared names,
referenced names, key, `SEP_*` separator, identifier-token names); per
option-3 span, fields 11-16; per compile `full_config`, `pragma_tokens`,
`line_directives`, `main_decls`. References now include cleanup functions,
`__builtin_convertvector`'s destination type, and tags first named at block
scope.

**Simulator** (`simulate.py`): `simulate_server()`, `groups_of()`,
`oracles_full()`, `oracles_tok()`. 20 runs: S2, S3, S2G, S3G, S3G\* each with
the design rule, the old observer (" old") and the fallback (" fb"); plus
S2 fb occ, S2 fb tok, S3G\* fb occ, S3G\* fb tok, S3G\* fb occ tok. Five
passes, the first a fill. `--server-out` writes every compile's decision.

**Three record runs** (all 187 compiles, serial): the first S0 records; a
second after review round 1; a third after round 2 and the K&R fix. The last
differs from the second only in 82 dropped `SV_UNDEFINED` flags and 10
fragments gaining `__v4sf`/`__v8sf` references.

**Results** are in the design, section 6, and `results/s0-sim.log`. In short
(recompilation, all 187 compiles): option 2 1.54x; option 3 1.64x; option 2
+ groups 1.72x; option 3 + groups (§11.2) 1.66x; option 3 + groups\* 2.17x;
with per-reading keys 2.64x; with token names 2.09x; both 2.52x. Cache 2.69x.

## 6. Verification history

**Round 1 (adversarial review workflow, 11-01 to 11-02).** Found: main-file identity
across passes; fragments ending inside macro arguments; K&R parameter
declarations; cleanup-function references; the 2.8 rule missing from the
simulator; `#define` replay counted as free. Fixed and tested. Notes:
`verify/notes/round1-soundness.txt`. Agents hit session limits twice; their
work was salvaged from transcripts, and later agents had to write notes.

**Round 2 (11-02), five reviewers:** record plugin, simulator, soundness,
data, and an independent reimplementation (`verify/notes/round2-*.md`).
Found: stale nesting after directives with no callback (`#`, `#line`,
`#warning`, `#error`); the pragma state after a pragma line; the pending
mark at the pragma directive; macro names from other macros; convertvector
and block-scope tag references; the 2.8 rule too broad (per declaration);
empty members of option-3 spans; the per-reading oracle. All fixed, each
with a test.

**Round 3 (11-03), the tests.** The new tests caught a defect in the K&R fix
itself: it also cleared the boundary before a translation unit's first
declaration (and after a top-level `#pragma` or empty declaration). Replaced
by the prototype-scope check above. One test expectation of mine was wrong
(`pm`); the old simulator code fails the corrected test as it should.

**The second simulator** (`verify/reimpl2.py`), written from the docstrings
and the design without reading `simulate_server()`: 18,700 of 18,700
compile-passes identical, every field, on the final records. Sensitivity
runs (`verify/sens.sh`) show which readings the redis data pins (only the
token oracle's recursion). Synthetic builds (`verify/synth.py`) exercise
paths redis never reaches; the divergences there (empty units inside groups;
a fragment ending in macro arguments with no later event; token names
declared in the main file) are readings redis never exercises, and both
sides refuse in each.

**Tests:** `tests/server_record.py` 34 checks, `tests/server_simulate.py` 50,
`tests/record.py` and `tests/simulate.py` all pass on the final plugin.

## 7. Reproducing

```
cd clang/utils/header-cache-bench/bothner
BUILD=<llvm-project>/build_rel ./build.sh
SRC_ROOT=/dev/shm/bench-src RECORD=1 python3 trace_run.py out-record-s0
SRC_ROOT=/dev/shm/bench-src python3 trace_run.py out-redis-dram       # timing, best of 3
SRC_ROOT=/dev/shm/bench-src python3 recompile.py out-recompile-dram.json
python3 simulate.py out-record-s0 out-redis-dram out-recompile-dram.json \
  --server-out out-server-s0.json --out out-sim-s0.json > out-sim-s0.log
python3 verify/reimpl2.py out-record-s0 out-redis-dram mine.json
python3 verify/compare2.py mine.json out-server-s0.json
python3 tests/server_record.py <workdir>; python3 tests/server_simulate.py <workdir>
```

`/dev/shm/bench-src` is made by the cache's `dram_sources.sh`. The two
`foo.c` compiles are configure probes that fail under plain clang too; every
tool skips them.

## 8. Open

- Decisions 6-9 (design section 8).
- S1 onward (design section 6): the server itself. Nothing of it is built.
- Restoring cost and the start-up credit are assumptions until S1/S4.
- 26 of the 300 token-name refusals are not classified by hand.
- The cache's rerun under the fixed observer (decision 4) belongs to S2.
- The other five projects: no build logs on this machine.
