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

**D2, the go/no-go, done 2026-10-30: go.** The cache beats Bothner's bound
by a wide margin. Even reusing every fragment at no cost only ties it.

- **`BothnerTrace` record mode.** For each fragment it records a hash of the
  tokens the parser received, which captures every macro's effect on the
  fragment. It also records whether the parser was between declarations
  before and after the fragment, and the compile's configuration, built the
  way the cache's own check builds it. `tests/record.py` checks the paper's
  cases: `BUFSIZ` changed, `#define int size_t`, the enum with `#define`
  lines inside, `__FILE__` under another name.
- **`simulate.py` v0.** The first build fills the server, then the
  recompilation is scored.
  - One version is kept per location, and a fragment inside a declaration is
    never reused.
  - Under R1, a location already processed in this compile is parsed again.
  - A reused fragment's tokens must match the stored version's.
  - There are no dependency sets yet, which can only add reuse.
  - `tests/simulate.py` checks the decisions against a build worked out by
    hand.
- **`recompile.py`.** A fresh daemon; a first build through the cache, not
  timed; then each compile again under a new build ID. Plain clang, plain
  clang with the modules flags, and the cache are interleaved, best of 3.
  The front end only, with sources read in place.
- **Credit for a persistent process.** Bothner's server skips initializing the
  compiler, but his driver still runs for every request (p.22-24). Measured
  with redis's flags: a one-line file takes 8.3-8.4 ms; the driver alone
  (`-###`) 6.6-6.8 ms. So compiler start-up is 1.6-1.7 ms, credited as
  1.65 ms per compile.

**Recompilation decisions.** 52.6% of fragment instances reused; 38.2%
nested; 7.6% repeats within a compile; 1.6% changed. The reused instances
hold 78.4% of the time in fragments.

| redis, recompilation | All 187 compiles | redis-server (125) |
|---|---|---|
| Plain clang | 15.38 s | 13.32 s |
| Header cache (979 hits of 983 lookups) | 5.41 s: 2.84x (2.46x geometric mean per compile) | 4.13 s: 3.23x (3.05x) |
| Bothner bound, R1 v0, compiler start-up credited | 7.55 s: 2.04x (1.83x) | 6.13 s: 2.17x (2.05x) |
| The same, without the credit | 7.86 s: 1.96x (1.72x) | 6.34 s: 2.10x (1.95x) |
| Every fragment reused, compiler start-up credited | 5.48 s: 2.81x (2.39x) | 4.37 s: 3.05x (2.73x) |

**The most generous credit.** Crediting the whole process start, which
Bothner's design did not save, puts the bound at 2.27x and the ceiling at
3.26x. Only that last row beats the cache, and it is unreachable twice over:
the rule rejects 47% of fragment instances, and the credit counts work his
server still did.

**Caveats.**

- This recompilation is the same compiles again with nothing edited, which
  is the best case for both systems.
- Sources were read in place, not from DRAM.
- Fragment times come from runs with the plugin loaded, about 1% slow inside
  the span; that favors Bothner.
- D3-D5 add his dependency sets, which can only lower his numbers.

**D3-D4, the record plugin, done 2026-10-30.** Record mode now gives each
fragment instance everything D5's simulator needs:

- **Processing order.**
- **Pragma state** (`#pragma pack` and floating point) at its start.
- **Macro list.** Every identifier that reached the macro-expansion decision
  in it, with its state: not a macro, or a hash of its definition. This is
  the paper's "obvious solution" (p.30) plus expands-to. It comes from the
  cache's `ExpansionCandidateObserver`, whose slot is empty in a compile
  without the cache, and the cache's `serializeMacroTokens`.
- **Uses.** The fragments whose declarations its names resolved to, as GCC
  resolved them: to an entity's first declaration, which later ones merged
  into (p.27-28, p.31). They come from an AST walk after the parse:
  references, type names, and each redeclaration's earlier declaration.
  Builtins count as present from the start.
- **Flags.** It uses a main-file declaration, so it is never reusable (ours,
  from p.27); it declares a tag in a parameter list (§7.2: Clang moves such a
  tag into the function's context).
- **The oracle.** A hash of its tokens, its pragma state, and the oracles of
  every fragment holding an earlier declaration of an entity it used. Two
  instances with the same oracle parse to the same declarations.
- **For §8.2.** Every `#define` and `#undef` of the compile, in order.

**Checks.**

- `tests/record.py`:
  - every earlier case;
  - p.27's `h1.h`: `extern word flags;` has the same tokens, but uses the
    other `word` fragment and has another oracle;
  - an `__asm__` label or a `weak` attribute on an earlier declaration: same
    tokens, uses the earlier declaration's fragment, another oracle;
  - `#pragma pack(1)`: same tokens, other pragma state, other oracle;
  - §7.2 flagged;
  - `#define int size_t` flagged as using the main file.
- **Determinism.** All 187 redis compiles recorded twice agree in every field
  of all 161,403 fragment instances.
- **Fragment counts** still equal FragCount's on every compile.

**On redis:**

- 0.88 uses per fragment;
- 12.7 names per macro list;
- one fragment uses a main-file declaration;
- no tag is declared in a parameter list.

A recording takes 35 s, serial.

**D5, the full simulator, done 2026-10-30.** `simulate.py`:

- **Order.** Compiles run in log order, a first build and then the scored
  recompilation. Within each compile, instances run in processing order,
  with the compile's `#define`/`#undef` between them.
- **Stored versions.** One per location: the latest parse. A version keeps
  its macro list and its uses (location and version).
- **Reasons, in order, why an instance is not reused:**
  - nested (p.33);
  - a tag declared in a parameter list (p.28-29);
  - uses the main file;
  - repeat (p.28);
  - nothing stored;
  - the macro check fails;
  - uses: p.28's timestamp rule, read through version identity, which one
    version per location makes equivalent;
  - unsafe: the rule allows reuse but the oracle differs. Charged as a parse.

**The oracle, ours** (computed in the simulator from the recorded
dependencies).

- **What it hashes:** the tokens, the pragma state, and the distinct oracles
  of the fragments holding earlier declarations of what the fragment used.
- **Its own location is left out.** A header read twice redeclares its own
  first reading.
- **Each distinct oracle counts once.** Clang's `__stddef_size_t.h` is read a
  varying number of times, giving identical earlier declarations. Counting
  each copy made 2.7% of decisions falsely unsafe.

**Tests.** `tests/simulate.py` checks two builds of three compiles each,
worked out by hand, for every variant. One is p.27's `h1.h` with `M1`
alternating, plus a header read twice that redeclares its own typedef, plus
a user of that typedef. It shows:

- R0's collapse;
- R1's fix;
- R2's gain;
- the single version thrashing between `M1` and not `M1`;
- the ideal cache keeping both versions.

**Results, redis recompilation.** Front end; Bothner rows credited 1.65 ms of
compiler start-up per compile. These are after D6's three fixes to the
uses (below); before them the headline was 1.98x (2.11x).

| | All 187 compiles | redis-server (125) |
|---|---|---|
| Plain clang | 15.38 s | 13.32 s |
| **Header cache** | **5.41 s: 2.84x** (2.46x geometric mean per compile) | **4.13 s: 3.23x** (3.05x) |
| **Bothner, R1 + macro lists (headline)** | **7.79 s: 1.97x** (1.77x) | **6.34 s: 2.10x** (1.97x) |
| R0, the paper's literal rule | 11.30 s: 1.36x (1.28x) | 9.62 s: 1.38x (1.30x) |
| R2, most favourable | 6.53 s: 2.35x (2.04x) | 5.33 s: 2.50x (2.23x) |
| R1, no macro checks (the 2003 prototype) | 1.97x | 2.10x |
| R1, §8.2's two bits | 1.85x | 1.95x |
| Ideal fragment cache: any earlier parse with the same oracle | 6.12 s: 2.51x (2.15x) | 4.95 s: 2.69x (2.40x) |
| Every fragment reused | 5.48 s: 2.81x (2.39x) | 4.37 s: 3.05x (2.73x) |

**Headline decisions**, share of the recompilation's 161,403 fragment
instances:

| Outcome | Instances |
|---|---|
| Reused | 51.8% (75.9% of the time in fragments) |
| Nested | 38.2% |
| Repeat | 7.6% |
| Macro check fails | 1.6% |
| Uses fail | 0.6% |
| Unsafe | 0.1% |

**Findings for the paper.**

- **The literal rule (R0) collapses on Clang.** Its modular `__stddef_size_t.h`
  is read more than once per compile. Its re-parse becomes the stored version
  and depends on its own location, so it is never valid again, and every
  `size_t` user fails with it. In `server.c` that is 303 failed uses under
  R0, against 26 under R1, and the first failure is at that header.
- **Without macro checks (the 2003 prototype)** the rule would reuse 2,276
  fragment instances whose tokens differ. That is miscompiled code: 1,860 in
  `bits/mathcalls.h`, read three times per compile with different macros,
  and 334 in redis's `commands.def`.
- **Under every rule, 193 instances are unsafe** because an earlier
  declaration of something they use differs (most in `stdlib.h` and
  `stdio.h`). The oracle counts these conservatively; they are 0.1%.
- **No variant reaches the cache.** Even an ideal fragment cache that keeps
  every version and restores for free (2.51x, 2.69x) stays below it.

**D6, the time model and the plugin checked, done 2026-10-31.**

**`blank.py`: what the model charges against what removal saves.**

- **Compiles.** 15 redis compiles: five large, five middle-sized, five small
  or from `deps/`.
- **What is removed.** The locations whose every instance the headline
  reuses, and that nothing else in the compile uses or redeclares: no other
  fragment and not the main file. Record mode now also notes which fragments
  the main file uses (flag 8).
- **How.** Their fragments are blanked, newlines kept, in copies of their
  headers. The compile reads the copies through a VFS overlay
  (`-ivfsoverlay`, original names kept, so `__FILE__` and `#pragma once`
  are unchanged). The control is an overlay of unchanged copies; its
  preprocessed output equals the compile's without an overlay. Best of 11
  each, interleaved.

| | Charged by the model | Saved when measured | Model against measured |
|---|---|---|---|
| 15 compiles (540-554 of about 1,500 locations blanked in the large ones) | 264.6 ms | 261.2 ms | +1.3% |
| Per compile | | | median +1.2%, from -3.8% (`redis-cli.c`) to +21.2% (`lapi.c`: 0.44 ms of a 47 ms compile) |

- **The model overstates the saving slightly**, which favours Bothner.
- **A strict bound.** `SAVED_SCALE=1.038` adds the worst underestimate to every
  reused fragment's time. Then:
  - headline: 2.05x (2.19x for redis-server);
  - R2: 2.48x (2.64x);
  - ideal fragment cache: 2.66x (2.87x);
  - every fragment reused: 3.01x (3.30x).

  All but the last, which no rule reaches, stay below the cache's 2.84x
  (3.23x).
- **Limit.** Only fragments nothing uses can be removed this way, about a
  third of the locations. The time charged to used fragments is not checked
  directly.

**Three references record mode missed.** Blanking found them: three compiles
failed to compile with the first selection.

- **An array of a typedef'd struct** needs the struct's definition
  (`connListener` in `server.h`), which no name lookup reaches.
- **A member access** likewise needs its struct's definition.
- **A call to a library builtin** (`memcpy`) is valid in C99 only once a
  header declared it. Record mode had treated Clang's implicit builtin as
  present from the start, as GCC's were (p.23).

Now:

- the first two feed the oracle (Bothner recorded name lookups only);
- the third is a use;
- `tests/record.py` checks all three.

With them the headline is 1.97x (2.10x), and the oracle counts 193 unsafe
instances instead of 92. All results above are from this version.

**`objects.py`: the plugin changes nothing.** All 187 redis compiles were
built to objects three ways: plain clang; with the plugin timing; with it
recording (an AST walk before code generation). They are byte-identical. The
two `foo.c` probes fail in all three ways.

**Not done yet:**

- option 3 (`#define` inside fragments) as a row;
- the other five projects.
