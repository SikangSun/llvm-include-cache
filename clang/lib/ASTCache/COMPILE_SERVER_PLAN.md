# Bothner's compile server as a recompilation baseline

Written 2026-10-29, against `header-cache` at `cda4042be`. This replaces two
earlier versions of this file:

- a single-process server, estimated at 25-40 person-days;
- fragments stored in the header cache's module files. Measurements the same
  day ruled that out (section 1).

## 0. Scope

- **What is built.** Only what changes the evaluation's numbers or its
  correctness. The comparison is on **recompilation**: a first build fills the
  state and is not timed.
- **Source.** Only Bothner, "Gcc Compile Server", GCC Developers' Summit 2003,
  proceedings pp. 21-34 ("p." below).
- **The technique.** Headers are always read as text, and their directives
  always run. The text between two directives is a *fragment*. At each
  fragment start:
  - if the stored fragment is valid, its declarations are restored and the
    preprocessor skips to the next directive (p.26-27);
  - otherwise the fragment is parsed and becomes the stored one, one per
    location (p.26).
- **When a fragment is valid.** It has not been processed yet in this
  compile, and every fragment it took declarations from was reused in this
  compile and has not been re-parsed since (p.27-28).

## 1. Why not on the header cache's module files

Measured on 2026-10-29 with `build_rel` clang. Scripts and outputs are in the
session scratchpad under `cs-check/`.

**Hidden fragments are unsafe.** Consider a module file loaded with only some
of its submodules visible. That is the earlier plan's single file per parse,
one submodule per fragment. 37 cases were tested in C with `-fmodules`.

- **Hard errors.** Hidden declarations with linkage still take part in
  redeclaration lookup. A changed `extern` type, a changed prototype, a
  changed struct layout, or a second external definition (even an identical
  one) is an error.
- **Silent wrong objects** (no diagnostic, object differs from plain clang):
  - a hidden `__asm__` label is inherited: the call goes to `foo2`, where
    plain clang calls `foo`;
  - a hidden `weak` or `visibility` attribute is inherited;
  - `static int sv;` hidden, against `static long sv;` in the text, gives
    `sv` 4 bytes instead of 8;
  - an `extern` declaration binds to a hidden `static`.
- **Code emitted from hidden fragments.** Their tentative definitions are
  emitted. Must-emit functions reach CodeGen at load time, so they come out
  in a different order from the text.

**One module file per fragment is too slow.**

| What | Measured cost |
|---|---|
| Loading one module file | 80-90 us, more than parsing the same small header as text (74 us) |
| Each identifier the compile has not seen yet | A probe of every loaded file, nothing indexes them: 37 / 97 / 140 ns per file at 100 / 1,000 / 3,000 files |
| redis `server.c`, about 1,570 fragments, one module file each | 1.2-1.8 s of extra front end, against 229 ms for the whole plain compile |

The cache's linked copies, one file per header, already measured 1.00x
against 1.95x (HISTORY.md).

**What Bothner's server would handle on redis.** All 187 compiles were
counted with a plugin, checked against `-H` on every compile.

| Per redis-server compile (median) | Count |
|---|---|
| Fragments, option 2 (any directive ends one; what was built, p.26) | 1,453 |
| Fragments, option 3 (`#define`/`#undef` inside fragments) | 887 |
| Directives in headers, always run | about 10,000 |
| Share of processed tokens in header fragments (the most reuse can save) | 50% |

- **Where they come from.** About 90% of fragments are in system headers.
  The whole build has only 3,402 distinct fragment locations.
- **Fragments ending inside a declaration.** Under option 2, 32% end inside
  `(`, `[` or `{`: enum and initializer lists with a `#define` after each
  item, mostly `bits/confname.h`. Under option 3 it is 8%. The paper never
  reuses these (§11.2, p.33).
- **Locations met twice.** 7.6% of fragment visits come back to a location
  already processed in the same compile, mostly `bits/mathcalls.h` and
  Clang's `__stddef_*.h`. The p.28 rule re-parses them.

**Conclusion.** A real realization in Clang needs either:

- Bothner's own architecture: one long-lived process keeping parsed trees,
  with a full reset of the compiler state between requests;
- or a module reader that releases a module file's fragments one at a time
  after loading. The cache's T17 skip set is fixed before loading.

Neither is likely by 2026-11-10 (section 2). This is the "vastly different
implementation" case: the cache's daemon and module files cannot carry
fragment reuse, either correctly or cheaply.

## 2. Options

Two independent judges (a skeptical reviewer and the engineer who has to
deliver) ranked the model first.

| | **A. Model (recommended)** | B. Persistent server | C. Releasing reader |
|---|---|---|---|
| What it is | Real compiles run as plain clang. Instrumentation records each fragment, and Bothner's rule is replayed over the build to decide what his server would reuse. | One long-lived clang per configuration, keeping fragments in memory, as Bothner built it | The cache's daemon and module files, with a reader that keeps fragments unread until each is reused |
| Exact | His decisions: fragments, the p.28 rule, macro checks, §7.2, §11.2 | His decisions and his machinery | His decisions |
| Idealized or changed | Skip, restore and undo cost nothing (in his favor) | Nothing in principle; must reset Sema, ASTContext and the preprocessor between requests | Restore is a module-file load (against him) |
| Realistic effort | 8-11 days | 20-30 days | 18-28 days |
| Chance for redis by 11-10 | About 80% | 10-20% | About 20% |
| What the paper can claim | An upper bound on what his rule saves | A measured baseline | A measured baseline that mostly measures module-file costs |

## 3. The recommended model

### 3.1 What is exact and what is idealized

| Paper | In the model |
|---|---|
| Fragments between any two directives, option 2 (p.25-26) | **Exact.** Boundaries from the verified counting plugin. Option 3 is one extra row. |
| Directives always run; include guards need nothing special (p.26) | **Exact**: the real compile runs them |
| One stored fragment per location (p.26) | **Exact**, kept per configuration (3.3) |
| Depends-on sets of fragments; the timestamp rule with read, include and main timestamps (p.27-28) | **Exact.** Uses are the fragments whose declarations the fragment's identifiers bound to. As in GCC, which merges a redeclaration into the first declaration (p.31), a use points to the fragment of the binding's first declaration. |
| A location already processed in this compile is re-parsed (p.28) | **Exact** in rule R0. R1 and R2 are variants (3.3). |
| §7.2: a tag first declared in a parameter list (p.28-29) | **Exact**: such a fragment is never reused |
| §8 macro dependencies | Three variants, all on the same records: none (the prototype, "does not yet check", p.29); the per-fragment not-a-macro list, the paper's "obvious solution" (p.30), plus our expands-to list; §8.2's two bits (p.30) |
| §11.2: a fragment entered or left inside a declaration is not reused (p.33) | **Exact**, read from the cache's `DeclBoundaryObserver` |
| §11.4: losses from include order (p.33-34) | Not assumed; they come out of the replay |
| §9 restore, the skip, §10 undo buffer (p.30-32) | **Idealized at zero cost**, in his favor. A second row charges an in-memory restore (about 0.1-0.3 us per fragment plus 50-100 ns per binding); it moves results by under 0.5%. |
| One request at a time (p.23; the paper is silent on concurrency) | The simulated server is serial, in the build log's order |
| One flag set per server (p.22) | State kept per configuration, as the cache's configuration check defines it, not per command line |

### 3.2 Instrumentation: a plugin, no clang rebuild

The plugin is built against `build_rel` in about 11 s and loaded with
`-fplugin=`. Every compile stays plain clang.

- **Boundaries.** Taken from `FragCount.cpp`, which matched `-H` and the
  preprocessor's own callbacks on all 187 redis compiles.
- **Record, per fragment instance:**
  - location: configuration, file as opened, offset;
  - a hash of the expanded tokens the parser received;
  - the macro state of every identifier that reached the expansion decision.
    This uses the cache's `ExpansionCandidateObserver` and
    `serializeMacroTokens`;
  - the file-scope ordinary and tag binding of every identifier, mapped to
    the fragment that declared it (`Sema::IdResolver`);
  - whether the parser was nested at its start and end
    (`DeclBoundaryObserver`);
  - the §7.2 case, pragma state, and `__FILE__` text;
  - the reasons a server could not store it: a diagnostic, `_Pragma`,
    `__COUNTER__` and the like.
- **Timing.** The plugin reads the time-stamp counter at every delivered
  token, every directive and every top-level declaration. It attributes time
  to fragments, directives and the main file, and corrects for the parser's
  one-token lookahead.

### 3.3 The simulator

A Python program replays the build twice in log order: a fill, then the
timed recompilation. It uses Bothner's state machine:

- the global clock `c_timestamp`;
- per fragment, `read_timestamp` and `include_timestamp`;
- `main_timestamp` per compile;
- one stored version per location and configuration.

**Rules for a location met again in the same compile:**

- **R0, the paper's rule.** Expected to collapse under Clang: the modular
  `__stddef_size_t.h` runs `typedef __SIZE_TYPE__ size_t;` on every entry
  (repeats in 181 of 187 compiles). Its second parse then makes the fragment
  depend on itself, so every user of `size_t` is invalid in every compile.
  This is inferred from the rule; day 2 confirms it.
- **R1, ours (headline).** The repeat is parsed, but an identical re-parse
  keeps the stored version.
- **R2.** Locations are numbered by occurrence; the variant most favorable to
  him.
- **A variant of p.28's own alternative.** A use is valid if the name's
  global binding is still that declaration.

**The oracle.** Every reuse decision is checked against the real parse of
the same location in the same compile. The fragment's input hash must equal
the stored version's: tokens, macro states, the versions its identifiers
bound to, pragma state, configuration and file name. A decision the oracle
rejects is a reuse that would have produced different code. It is counted,
and charged as a parse.

**Outputs per compile:** reused; parsed, by reason; never stored, by reason;
unsafe.

### 3.4 Time model and validation

**Baseline time of compile c:**

    T_plainM(c) − Σ_reused t(f) + charge(c) − credit(c)

- `T_plainM` is plain clang with the cache's modules flags, timed in the same
  run as the cache.
- `t(f)` is the fragment's attributed time.
- `charge` is zero for the upper bound.
- `credit` is the work Bothner's persistent process skips: cc1
  initialization (about 3 ms; his driver still ran) and re-reading header
  files.

With the credit, the bound is strict. Without it, it is a secondary row.

**Validation.**

- **Time.** On about 15 compiles, reused fragments that nothing else
  references are blanked through `-ivfsoverlay`, with newlines kept. The
  measured drop is compared with the attributed sum. The error bound found is
  added to the saved time.
- **Objects.** Objects compiled with `-c` and the plugin loaded are compared
  with plain clang's. They should be identical by construction. That proves
  the plugin is passive, and nothing about the technique: the per-decision
  oracle is the correctness argument, and the paper says so.

### 3.5 Recompilation protocol

There is no recompilation harness in this checkout. README.md names
`scratch/e2e/build_bench.py`, which is not here. One is added by giving
`modes_bench.py` a fill-then-time mode:

1. A first build fills the cache's daemon and the simulator, untimed.
2. The recompilation is timed for plain clang with modules and for the
   cache, interleaved compile by compile as in the cache's own runs.
3. The model's baseline is computed from those same plain-clang times.

### 3.6 Which way each departure leans

| Departure | Effect on the baseline |
|---|---|
| Restore, skip and undo free; server memory growth not modeled | Faster than his server |
| Persistent-process credit | Faster |
| Serial server, with no queueing under `make -j` | Faster in wall time |
| R1 instead of R0 | Faster |
| Unsafe reuses charged as parses; macro checks on | Slower than the 2003 prototype, but needed for the same objects |
| Uses from Clang's bindings instead of GCC's lookups | About the same |
| Parse times are Clang 22's, not GCC 3's | Neutral: the cache is measured on the same Clang |

## 4. Schedule

| Day | Work | Done when |
|---|---|---|
| D1, 10-30 | Timing plugin, from `FragCount.cpp` | Parts sum to the compile's total; overhead under 2% |
| **D2, 10-31** | **Go/no-go.** Simulator v0: location, token and macro hash, no uses; fill then recompilation | The upper bound for redis sits beside the cache's recompilation number |
| D3-D4 | Record plugin: macro lists, bindings to fragments, nesting, §7.2, pragma state | Counts match `per_compile_mod.csv`; hashes identical between fill and recompilation; section 5's cases flip as expected |
| D5 | Full simulator: R0/R1/R2, the three macro variants, option 3 | Per-compile reports; three compiles checked by hand |
| D6 | Blanking validation; object check | Attribution error bounded (target ±5% of saved time) |
| D7 | Recompilation harness and redis tables | Table ready for the paper |
| D8, 11-06 | Buffer and the paper's baseline section | |
| 11-07 to 11-08 | The other five projects, only if redis is done | |

**Effort.** 8 days nominal, 9-11 realistic. The tight spots are D1's
attribution and D3-D4's mapping of bindings to fragments.

**Rebuilds.** None: clang is not rebuilt, and nothing in `clang/lib` changes.
The plugin, simulator and harness go in
`clang/utils/header-cache-bench/bothner/`, beside `pph/`.

## 5. Tests

Each case must give the expected decision, and its oracle hash must change
where the parse changes:

| Case | Page | Expected |
|---|---|---|
| A guarded header in two compiles | p.26 | All reused the second time |
| `D1` under `#if M1`, `D2` under `#if M2`, M2 changed | p.26 | `D1` reused, `D2` parsed |
| `h1.h`: `word` under `#if M1`, then `extern word flags;`, M1 alternating | p.27 | `flags` parsed each time M1 changes |
| A header without guards, twice in one compile | p.28 | Parsed the second time |
| `extern int do_something(struct x *);` | p.28-29 | Never reused |
| `char buffer[BUFSIZ];`, `BUFSIZ` changed | p.29 | Unsafe without macro checks; parsed with them |
| `a.h`, `b.h`, with `#define int size_t` and `#define i j` between them | p.29 | The same |
| `#define strcmp strcmp` | p.30 | Reused where the definition is the same |
| siginfo-style enum with `#define` lines inside; `struct timestamp` with `#if` inside | p.32-33 | Not reused (nested) |

The 37 hidden-fragment cases from section 1 (asm label, `weak`, visibility,
`static` against `extern`, a typedef or struct layout changed) are each also
run as a case whose hash must change.

## 6. Risks

- **The bound may beat the cache's recompilation.** The rough estimate is
  1.5-1.8x on large redis files under R1. If so, do not build a realization
  to rescue the claim. Instead, put the paper's comparison on what differs:
  - separate compile processes;
  - `make -j` with no warm-up per job;
  - several versions per header;
  - first builds.

  Day 2 exists to learn this early.
- **"A model, not a baseline."** It is labelled an idealized upper bound
  everywhere. It is shown beside measured baselines: plain clang, PPH, Clang
  modules. Section 1's measurements explain why it is not built.
- **Attribution error.** Bounded by the blanking check, and added to the
  saved time.
- **Oracle gaps.** A missed input would overstate safety, and hashing too much
  would understate reuse. Section 5's cases guard the first. Identical hashes
  between fill and recompilation guard the second.

## 7. Decisions

- **D1. Approach:** A, the model (recommended); B, the persistent server; or
  C, the releasing reader (section 2).
- **D2. Headline rule:** R1 (recommended). R0 is reported as a finding and R2
  as the most favorable bound.
- **D3. Headline macro checks:** the "obvious solution" plus expands-to
  (recommended). The prototype's none and §8.2's bits are reported, with the
  count of unsafe reuses the prototype would have made.
- **D4. Recompilation harness:** is there one already (README.md's
  `scratch/e2e/build_bench.py`)? If not, D7 builds it.
- **D5. Branch:** the PPH work is still uncommitted on `pph-baseline`. Commit
  it before this work starts.

Decided 2026-10-30:

- D1: the model;
- D2: R1 as the headline rule;
- D3: the "obvious solution" plus expands-to as the headline macro checks;
- D5: PPH committed as `6841e8764`; this work is on branch `bothner-model`.

## 8. Status

**D1, done 2026-10-30.** The code is in `clang/utils/header-cache-bench/bothner/`.

- **Plugins.**
  - `BothnerTrace.cpp`: the timing plugin.
  - `fragcount/`: the counting plugin, with its verified counts.
  - `build.sh` builds both against `build_rel` in about 25 s.
- **Attribution.** The time-stamp counter is read at every directive, file
  change and top-level declaration, and at the first token after each. Every
  token between two such events lies in one fragment, so no timestamp per
  token is needed. Each interval goes to a fragment, the directives or the main
  file. A declaration's lookahead tail goes back to its own fragment.
- **`tests/attribution.py`**, all passing:
  - the plugin finds the fragments FragCount counts;
  - fragments plus the other parts add up to the in-process span, to 1 ns;
  - the lookahead tail is charged to the right fragment;
  - a large fragment is charged 30.2-30.9 ms. Blanking it, with newlines
    kept, saves 30.0-30.5 ms when measured (ratio 1.006-1.046 over four runs).
- **Overhead (`overhead.py`, 10 redis compiles).** Inside the timed span: a
  median of 1.3%, at most 2.1%, which is within run-to-run noise. Including
  the fixed cost of loading the plugin, which falls before the span: a median
  of 2.1%. Timestamping every token cost 2.4%; that is why only the first
  token after each event is timed.
- **All 187 redis compiles (`trace_run.py`, best of 3).** Fragment counts equal
  FragCount's on every compile, and no token falls outside a fragment. 0.8% of
  the span is estimated by calibration rather than measured.

| | Plain front end | In header fragments | Every fragment reused at no cost |
|---|---|---|---|
| All compiles | 15.86 s | 9.59 s (60.5%) | 2.53x in total, 2.12x geometric mean per compile |
| redis-server (`src/`) | 13.74 s | 8.75 s (63.7%) | 2.75x in total, 2.42x geometric mean per compile |

The last column is a ceiling, before Bothner's rule rejects anything. D2's
simulator finds how much of it his rule keeps.
