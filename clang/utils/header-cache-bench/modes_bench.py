"""Plain clang and several cache modes, same binary, one project, first
builds, one compile at a time, modes interleaved file by file with the order
rotated; each cache mode with its own fresh daemon, build ID and per-copy dump
(CLANG_HEADER_CACHE_DAEMON_DUMP). KIND=syntax runs the front end only; full
compares each mode's objects with plain clang's.
usage: modes_bench.py <project> <out.json>
env: KIND, CORE, DCORES (comma list, one per cache mode), BIN, LEVEL, DUMP, MODES,
SRC_ROOT, PLAIN_MODULES, PASSES, CHANGE_CMD, CHANGE_UNDO, INSTR, PERFC, SAMPLE, FILTER,
REGION_SIZE, EXTRA, EXTRA_<mode>.
env, required: BIN (a release build's bin directory), BENCH_LOGS, BENCH_ROOT
(see record_builds.sh).

PASSES=N (default 1) builds N times: each later pass is a rebuild with no
source changed, every compile again against the same daemons (nothing
cleared) under a new build ID, plain clang timed beside it again. Pass 1 is
in T, S, W, failed, differ; passes 2..N in "passes".

Sources are read from DRAM: SRC_ROOT (default /dev/shm/bench-src, filled by
dram_sources.sh) stands for BENCH_ROOT; SRC_ROOT=in-place reads them there. Plain clang runs as a user runs it, without
the -fmodules -fno-implicit-module-maps only the cache needs (front end);
PLAIN_MODULES=1 gives it them. Full builds keep them on plain clang, whose
objects are compared with the cache's."""
import json, os, re, shutil, subprocess, sys, tempfile, time
BIN = os.environ["BIN"]
# Where record_builds.sh put the compile logs, and the directory the
# projects were built in when it recorded them.
LOGS = os.environ["BENCH_LOGS"]
ROOT = os.environ["BENCH_ROOT"].rstrip("/") + "/"
proj, out = sys.argv[1], sys.argv[2]
KIND = os.environ.get("KIND", "syntax")
SRC = os.environ.get("SRC_ROOT", "/dev/shm/bench-src")
SRC = ROOT if SRC == "in-place" else SRC.rstrip("/") + "/"
if not os.path.isdir(SRC + proj):
    sys.exit(f"{SRC}{proj} is missing: run dram_sources.sh (or SRC_ROOT=in-place)")
PLAIN_MODULES = KIND == "full" or os.environ.get("PLAIN_MODULES") == "1"
CORE = os.environ.get("CORE", "7")
ALL = {
    "top": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1"},
    # Top-level folding, and a header read before any compile listed it is
    # written if it was slow and nothing inside it was served.
    # A copy whose header expanded __COUNTER__ is written, checked, never
    # served; each miss where it fit but for the counter is classed
    # (DECISIONS.md 2.7).
    "probe": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER_PROBE": "1"},
    # A copy whose header expanded __COUNTER__ is served where the counter
    # starts at the value it started at (DECISIONS.md 2.7); with fold, a
    # header inside one being written is asked for too, and a copy served
    # there is held by the one being written.
    "counter": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1"},
    "counterfold": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                    "CLANG_HEADER_CACHE_FOLD_SERVED": "1"},
    "firstread": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_FIRST_READ": "1"},
    # Top-level folding with every name compared, as before 2026-10-24.
    "strictnames": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_STRICT_NAMES": "1"},
    "link": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_LINK": "1"},
    # Linked units (2026-10-31 plan): link mode on top of COUNTER=1.
    "linkc": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
              "CLANG_HEADER_CACHE_LINK": "1"},
    # bin-u27+: linkc, and after a copy with links served two hits, a process
    # forked from the compile writes the tree as one copy (step 5). The
    # compile waits for it at its exit, so that its instructions are counted
    # with the compile's; the merge line on stderr gives them apart.
    "linkm": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
              "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "2",
              "CLANG_HEADER_CACHE_MERGE_WAIT": "1"},
    # linkm, merged after five hits (the merge count is a meta-parameter).
    "linkm5": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
               "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "5",
               "CLANG_HEADER_CACHE_MERGE_WAIT": "1"},
    # bin-u30: merge count sweep (the user's 2026-11-01 plan: find where
    # merging pays most, per codebase, on one core). linkm<k>: merged once
    # hits loaded a combination k times.
    **{f"linkm{k}": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                    "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": str(k),
                    "CLANG_HEADER_CACHE_MERGE_WAIT": "1"} for k in (3, 4, 6, 8, 12, 16, 32, 64, 128, 256)},
    # Folding threshold sweep (user, 2026-10-31): linkc with the daemon's
    # -ask-cutoff-us at <c> microseconds (DAEMON_ARGS): a header under the
    # cutoff is never asked for, so the copy around it holds it. A line per
    # hit (links, instructions) for the per-header factors.
    **{f"linkcut{c}": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                       "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_HIT_LOG": "1"}
       for c in (0, 100, 226, 500, 1000, 2000, 5000, 10000, 20000, 50000)},
    # bin-u32: the history-based merge rule (CLANG_HEADER_CACHE_MERGE=auto,
    # user 2026-10-31): a combination is merged once its hits so far pay for
    # the merge. linkma at the default cutoff, linkcut<c>ma at <c> us.
    # The merge rule with a safety factor k (user, 2026-10-31: "the gains is
    # just as important as zero loss"): merge once n x s >= k x cost, the
    # same as MERGE_SHARE = 0.4 / k. A line per hit.
    **{f"linkmak{k}": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                       "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "auto",
                       "CLANG_HEADER_CACHE_MERGE_WAIT": "1", "CLANG_HEADER_CACHE_HIT_LOG": "1",
                       "CLANG_HEADER_CACHE_MERGE_SHARE": str(round(0.4 / k, 4))} for k in (1, 2, 3, 4, 6)},
    "linkch": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
               "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_HIT_LOG": "1"},
    # bin-u40: the fold rule (user 2026-11-01: run the lower-bound rule on
    # the workbench and the Linux sample), linkc with the daemon's -fold
    # (DAEMON_ARGS): a header is listed at its first read if its own time
    # plus an ask per header it includes is at least an ask and a load, and
    # folded once its losses pass the budget. linkf<b>: a budget of b
    # writes; linkfl: the floor alone; linkf<b>w<us>: a write costs <us>;
    # linkfn<b>: no floor, every header listed at its first read.
    **{m: {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
           "CLANG_HEADER_CACHE_LINK": "1"}
       for m in ("linkfl", "linkf1", "linkf2", "linkf4", "linkf8", "linkf16", "linkf32", "linkf8w2000",
                 "linkf2w2000", "linkfn8")},
    # bin-u41: what the floor reads (-fold-list). linkfi<b>: today's rule
    # (226 us with the headers inside it) to list, folded once its losses
    # pass b writes; linkfo<b>: the own time alone against an ask and a
    # load; linkfol: that floor alone.
    **{m: {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
           "CLANG_HEADER_CACHE_LINK": "1"}
       for m in ("linkfi1", "linkfi2", "linkfi4", "linkfi8", "linkfi16", "linkfi32", "linkfo2", "linkfo8",
                 "linkfo32", "linkfol")},
    # bin-u42: linkc, a context's declarations scanned once for unnamed
    # ones (CLANG_HEADER_CACHE_SCAN_ONCE=1), not again at each completion.
    "linkcscan": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                  "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_SCAN_ONCE": "1"},
    # bin-u43+: linkc with the measured cutoff (daemon -measured-cutoff): a
    # header is asked for while the parse time of the lines its copy holds is
    # at least one request and one load, as the build measured them.
    "linkcm": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
               "CLANG_HEADER_CACHE_LINK": "1"},
    # linkcm, each compile recording what the fold and merge rules read
    # (CLANG_HEADER_CACHE_RECORD) to RECORD_DIR.
    "linkcmr": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_RECORD": os.environ.get("RECORD_DIR", "")},
    # bin-u46+: linkc whose daemon runs the offline step (-offline): its
    # compiles record to RECORD_DIR, named by the daemon; after the build
    # the harness sends the hint a link would, waits for the step, and
    # keeps its time ("offline"). DECISIONS is the file the step writes.
    # With PASSES=2 (the user's recompilation evaluation, 2026-11-02), the
    # step runs between pass 1 and pass 2, on the same daemon and region:
    # pass 2 takes its decisions and its merged copies.
    "linkco": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
               "CLANG_HEADER_CACHE_LINK": "1"},
    # linkc whose daemon picks headers by five counters in the first build
    # (-merge-picks, DECISIONS.md 2.26). A compile that starts after a pick
    # merges the tree of a picked header at its next hit.
    "linkcp": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
               "CLANG_HEADER_CACHE_LINK": "1"},
    # linkc whose daemon hands its compiles the decisions in DECISIONS
    # (-offline-decisions): folds, tops and merges, no variable set.
    "linkcd": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
               "CLANG_HEADER_CACHE_LINK": "1"},
    # linkcm with what an earlier build's record decided
    # (CLANG_HEADER_CACHE_DECISIONS=DECISIONS), merges included.
    "linkcmd": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_DECISIONS": os.environ.get("DECISIONS", ""),
                "CLANG_HEADER_CACHE_MERGE": "decided"},
    # The same with -fold-link-us=0 (suffix n): loads of a header's copy as
    # another header's link cost it nothing.
    **{m + "n": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                 "CLANG_HEADER_CACHE_LINK": "1"}
       for m in ("linkfi2", "linkfi4", "linkfi8", "linkfi16", "linkfi32", "linkf8", "linkf32", "linkfo8", "linkfo32",
                 "linkfi6", "linkfi12", "linkfo4", "linkfo16")},
    "linkma": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
               "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "auto",
               "CLANG_HEADER_CACHE_MERGE_WAIT": "1"},
    # bin-u39+: linkma merges in the compile itself (task 10, user
    # 2026-11-01); linkmaf forks a process for each merge, as before.
    "linkmaf": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "auto",
                "CLANG_HEADER_CACHE_MERGE_WAIT": "1", "CLANG_HEADER_CACHE_MERGE_FORK": "1"},
    "linkmah": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "auto",
                "CLANG_HEADER_CACHE_HIT_LOG": "1"},
    **{f"linkcut{c}ma": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                         "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "auto",
                         "CLANG_HEADER_CACHE_MERGE_WAIT": "1"} for c in (2000, 5000)},
    # Measurement: linkc, combinations counted and never merged, and a line
    # per hit (CLANG_HEADER_CACHE_HIT_LOG): links, combination, instructions.
    "linkcount": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                  "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "1000000",
                  "CLANG_HEADER_CACHE_HIT_LOG": "1"},
    # Measurement: linkm with a line per hit, for what a merged copy's hits cost.
    "linkm2h": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_MERGE": "2",
                "CLANG_HEADER_CACHE_MERGE_WAIT": "1", "CLANG_HEADER_CACHE_HIT_LOG": "1"},
    # bin-u30: guard+ tolerance (CLANG_HEADER_CACHE_GUARD_PLUS): a copy
    # holding headers the compile read already serves where they read
    # nothing outside them and are as the compile read them.
    "counterg": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                 "CLANG_HEADER_CACHE_GUARD_PLUS": "1"},
    "linkcg": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
               "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_GUARD_PLUS": "1"},
    # bin-u2+: linkc as before 2026-11-01, each link loaded by a reader call of
    # its own, before the copy linking it.
    # Every name a copy takes from a link entered at the hit (as before
    # 2026-11-01), not when the reader needs it.
    "linknames": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                  "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_LINK_NAMES": "all"},
    "linkeach": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
                 "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_LINK_LOADS": "each"},
    # linkc, and a copy linked inside another may name a declaration of the
    # lines around it (CLANG_HEADER_CACHE_LINK_TAKES_PARENTS).
    "linkp": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1",
              "CLANG_HEADER_CACHE_LINK": "1", "CLANG_HEADER_CACHE_LINK_TAKES_PARENTS": "1"},
    "miss1": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_SECOND_MISS": "1"},
    # Headers inside a copy held as sections (REPORT.md T17), skipped by guard.
    "sections": {"CLANG_HEADER_CACHE_SECTIONS": "1"},
    # Sections that may read what the copy's earlier lines set (bin-sec2+),
    # each dependency tagged with its innermost section, counter on.
    "sections2": {"CLANG_HEADER_CACHE_SECTIONS": "2", "CLANG_HEADER_CACHE_COUNTER": "1"},
    # Measurement: sections2 without checking what skipped sections provide.
    # bin-lay: counter mode with function fingerprints that ignore names
    # made from __COUNTER__ values switched off (measurement).
    "counternomask": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1", "CLANG_HEADER_CACHE_NO_MASK": "1"},
    # bin-nolay, measurement only (unsafe): declaration checks compare no
    # struct layouts, the upper bound of not computing them.
    "counternolay": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER": "1", "CLANG_HEADER_CACHE_NO_LAYOUT_CHECK": "1"},
    "sections2nolay": {"CLANG_HEADER_CACHE_SECTIONS": "2", "CLANG_HEADER_CACHE_COUNTER": "1", "CLANG_HEADER_CACHE_NO_LAYOUT_CHECK": "1"},
    # sections2 with partial hits: a copy that left out headers the compile
    # has not read serves once they are read first ("-" direction).
    "sections2partial": {"CLANG_HEADER_CACHE_SECTIONS": "2", "CLANG_HEADER_CACHE_COUNTER": "1", "CLANG_HEADER_CACHE_PARTIAL": "1"},
    "sections2np": {"CLANG_HEADER_CACHE_SECTIONS": "2", "CLANG_HEADER_CACHE_COUNTER": "1", "CLANG_HEADER_CACHE_NO_PROVIDED_CHECK": "1"},
    # Top-level folding with partial hits: a copy that left out headers the
    # compile has not read serves once they are read first.
    "partial": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_PARTIAL": "1"},
    # Bases and second-level pieces: a section needing another version is
    # read first and the base serves (B), or the header is parsed with it (A).
    "piecesB": {"CLANG_HEADER_CACHE_PIECES": "1"},
    "piecesA": {"CLANG_HEADER_CACHE_PIECES": "A"},
    # Policy B with each version holding its headers as sections too.
    "piecesBv": {"CLANG_HEADER_CACHE_PIECES": "1", "CLANG_HEADER_CACHE_VERSION_SECTIONS": "1"},
    # Measurement: at each version miss, whether the section header's own
    # copies, or its versions in other bases, would fit there.
    "piecesProbe": {"CLANG_HEADER_CACHE_PIECES": "1", "CLANG_HEADER_CACHE_PROBE_REUSE": "1"},
    # Measurement: a header expanding __COUNTER__ is written and never
    # served; each miss notes whether such a copy fit but for the counter,
    # with the same start value, renamable, or neither (DECISIONS.md 2.7).
    "counterprobe": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_COUNTER_PROBE": "1"},
    # Measurement: the cache asked, nothing written, so nothing recorded.
    "noexport": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_EXPORT_PARTS": "0"},
}
# Bothner's compile server (COMPILE_SERVER_DESIGN.md), the user's choices of
# 2026-11-06: option 3 with groups that hold #define lines (decision 6b), 2.8's
# rule (7a), one key per reading (8b), token names as uses (9a, built in).
# One server per mode, pinned to CORE, serves every compile of the mode through
# the driver (CLANG_COMPILE_SERVER); the project's own flags, without the
# cache's -fmodules. "serverloc": the same with Bothner's location keys (8a).
# Each compile's instructions add the server's own for its request.
SERVER_ARGS = {"server": ["reuse=1", "option=3", "groups=1", "keys=reading"],
               "serverloc": ["reuse=1", "option=3", "groups=1"]}
# server2b: the server with memory spaces side by side sized for two builds
# (space-share=0.5), so that the same build again finds every store.
SERVER_ARGS["server2b"] = SERVER_ARGS["server"] + ["space-share=0.5"]
ALL.update({m: {} for m in SERVER_ARGS})
SERVER_SO = os.environ.get("SERVER_SO", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bothner", "BothnerServer.so"))


def is_server(m):
    return basemode(m) in SERVER_ARGS
CM = os.environ.get("MODES", "top link miss1").split()
def binof(m):
    # <mode>@<name>: the binaries in BIN_<name> (a bin directory), to compare
    # two builds in one run.
    return os.environ[f"BIN_{m.split('@')[1]}"] if m and "@" in m else BIN
def basemode(m):
    return m.split("@")[0] if m else m
MODES = ["plain"] + CM


def commands():
    d = f"{LOGS}/log-{proj}"
    cmds = []
    for name in sorted(os.listdir(d), key=lambda n: int(n.split(".")[0])):
        parts = open(f"{d}/{name}", "rb").read().decode(errors="replace").split("\0")[:-1]
        cwd, args = parts[0], parts[1:]
        if SRC != ROOT:
            cwd = cwd.replace(ROOT, SRC)
            args = [a.replace(ROOT, SRC) for a in args]
        if "-c" not in args or "-E" in args:
            continue
        srcs = [a for i, a in enumerate(args) if a.endswith(".c") and args[i - 1] not in ("-o", "-MF", "-MT", "-MQ")]
        if len(srcs) != 1 or "conftest" in srcs[0]:
            continue
        cmds.append((cwd, args, srcs[0]))
    return cmds


def replay_args(args, obj):
    a = list(args)
    if "-o" in a:
        a[a.index("-o") + 1] = obj
    else:
        a += ["-o", obj]
    if KIND == "syntax":
        i = a.index("-o")
        del a[i:i + 2]
        a[a.index("-c")] = "-fsyntax-only"
    return ["-fmodules", "-fno-implicit-module-maps"] + a


# Daemon options per mode (folding threshold sweep, 2026-10-31).
DAEMON_ARGS = {f"linkcut{c}": ["-ask-cutoff-us", str(c)] for c in (0, 100, 226, 500, 1000, 2000, 5000, 10000, 20000, 50000)}
DAEMON_ARGS.update({f"linkcut{c}ma": ["-ask-cutoff-us", str(c)] for c in (2000, 5000)})
DAEMON_ARGS.update({"linkfl": ["-fold", "-fold-budget=1e15"], "linkfn8": ["-fold", "-fold-floor=0", "-fold-budget=8"]})
DAEMON_ARGS.update({f"linkf{b}": ["-fold", f"-fold-budget={b}"] for b in (1, 2, 4, 8, 16, 32)})
DAEMON_ARGS.update({f"linkf{b}w2000": ["-fold", f"-fold-budget={b}", "-fold-write-us=2000"] for b in (2, 8)})
DAEMON_ARGS.update({f"linkfi{b}": ["-fold", "-fold-list=inclusive", f"-fold-budget={b}"] for b in (1, 2, 4, 8, 16, 32)})
DAEMON_ARGS.update({f"linkfo{b}": ["-fold", "-fold-list=own", f"-fold-budget={b}"] for b in (2, 8, 32)})
DAEMON_ARGS.update({"linkfol": ["-fold", "-fold-list=own", "-fold-budget=1e15"]})
DAEMON_ARGS.update({f"linkfi{b}": ["-fold", "-fold-list=inclusive", f"-fold-budget={b}"] for b in (6, 12)})
DAEMON_ARGS.update({f"linkfo{b}": ["-fold", "-fold-list=own", f"-fold-budget={b}"] for b in (4, 16)})
DAEMON_ARGS.update({m: ["-measured-cutoff"] for m in ("linkcm", "linkcmr", "linkcmd")})
DAEMON_ARGS.update({"linkcp": ["-merge-picks"]})
OFFLINE_PY = os.environ.get("OFFLINE_PY", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "header-cache",
                                                       "offline", "offline.py"))
DAEMON_ARGS.update({"linkco": ["-offline", f"{sys.executable} -I {OFFLINE_PY}", "-offline-record", os.environ.get("RECORD_DIR", ""),
                               "-offline-decisions", os.environ.get("DECISIONS", "")],
                    "linkcd": ["-offline-decisions", os.environ.get("DECISIONS", "")]})
DAEMON_ARGS.update({m + "n": DAEMON_ARGS[m] + ["-fold-link-us=0"]
                    for m in ("linkfi2", "linkfi4", "linkfi8", "linkfi16", "linkfi32", "linkf8", "linkf32", "linkfo8", "linkfo32",
                              "linkfi6", "linkfi12", "linkfo4", "linkfo16")})


def daemon(core, dump, mode=None):
    w = tempfile.mkdtemp(prefix="mb-")
    region = f"/dev/shm/{os.path.basename(w)}.region"
    # REGION_SIZE=<GiB>: a region larger than the daemon's 4 GiB default; a
    # full region drops every later copy without a word.
    size = ["-region-size", str(int(os.environ["REGION_SIZE"]) << 30)] if os.environ.get("REGION_SIZE") else []
    p = subprocess.Popen(["taskset", "-c", core, f"{binof(mode)}/clang-header-cache-daemon", "-socket", f"{w}/sock",
                          "-region", region, "-v"] + size + DAEMON_ARGS.get(basemode(mode), []), stdout=open(f"{w}/daemon.log", "w"), stderr=subprocess.STDOUT,
                         env=dict(os.environ, **({"CLANG_HEADER_CACHE_DAEMON_DUMP": dump} if dump else {})))
    while not os.path.exists(f"{w}/sock"):
        time.sleep(0.01)
    p.log, p.region = f"{w}/daemon.log", region
    return p, f"{w}/sock"


if "linkco" in CM or "linkcd" in CM:
    assert os.environ.get("DECISIONS") and not ("linkco" in CM and "linkcd" in CM), "linkco/linkcd: DECISIONS, one of them"
if "linkco" in CM:
    # The step runs while its decisions file is missing, on this build's record.
    assert os.environ.get("RECORD_DIR"), "linkco: RECORD_DIR"
    subprocess.run(["rm", "-rf", os.environ["RECORD_DIR"]], check=True)
    for f in (os.environ["DECISIONS"], os.environ["DECISIONS"] + ".log"):
        if os.path.exists(f):
            os.remove(f)
if "linkcd" in CM:
    assert os.path.exists(os.environ["DECISIONS"]), "linkcd: DECISIONS must exist"


def build_hint(sock, output):
    """The message clang's driver sends after a link (BuildHint), and the
    daemon's acknowledgment."""
    import socket, struct
    body = struct.pack("<BI", 15, len(output.encode())) + output.encode()
    with socket.socket(socket.AF_UNIX) as c:
        c.connect(sock)
        c.sendall(struct.pack("<I", len(body)) + body)
        n = struct.unpack("<I", c.recv(4))[0]
        return c.recv(n)[:1] == bytes([16])


def run_offline():
    """The offline step: the hint a link sends, then the step's run to its end."""
    for m, (p, sock) in daemons.items():
        if "-offline" not in DAEMON_ARGS.get(basemode(m), []):
            continue
        t = time.perf_counter()
        ok = build_hint(sock, f"{proj}-{m}")
        g = None
        while ok and time.perf_counter() - t < 1800:
            g = re.search(r"offline: run \d+ (finished|failed) in (\d+) ms", open(p.log).read())
            if g:
                break
            time.sleep(0.05)
        log = os.environ["DECISIONS"] + ".log"
        offline[m] = {"hint": ok, "status": g[1] if g else "none", "ms": int(g[2]) if g else None,
                      "log": open(log).read() if os.path.exists(log) else ""}
        print(f"{proj} {KIND} offline: {m} {offline[m]['status']} in {offline[m]['ms']} ms: "
              + " ".join(l for l in offline[m]["log"].splitlines() if l.startswith("{")), flush=True)


def server(mode):
    """A compile server for the mode on CORE, with its socket and results file."""
    w = tempfile.mkdtemp(prefix="mbs-")
    args = sum((["-plugin-arg-bothner-server", a] for a in
                [f"socket={w}/sock", f"results={w}/results.jsonl"] + SERVER_ARGS[basemode(mode)]), [])
    p = subprocess.Popen(["taskset", "-c", CORE, f"{binof(mode)}/clang", "-cc1", "-load", SERVER_SO,
                          "-plugin", "bothner-server"] + args + ["-x", "c", "/dev/null"],
                         stdout=open(f"{w}/server.log", "w"), stderr=subprocess.STDOUT,
                         env={k: v for k, v in os.environ.items()
                              if not k.startswith(("CLANG_HEADER_CACHE", "CLANG_COMPILE_SERVER"))})
    while not os.path.exists(f"{w}/sock"):
        if p.poll() is not None:
            sys.exit(f"compile server for {mode} exited: {open(w + '/server.log').read()[-2000:]}")
        time.sleep(0.01)
    p.log, p.results, p.read_at = f"{w}/server.log", f"{w}/results.jsonl", 0
    return p, f"{w}/sock"


def server_results(p):
    """The results lines the server wrote since the last call."""
    if not os.path.exists(p.results):
        return []
    with open(p.results) as f:
        f.seek(p.read_at)
        text = f.read()
    end = text.rfind("\n") + 1
    p.read_at += len(text[:end].encode())
    return [json.loads(l) for l in text[:end].splitlines() if l.strip()]


def quit_server(sock):
    """{"quit": true} in CacheProtocol.h's framing (MessageKind::CompileRequest)."""
    import socket, struct
    body = bytes([17]) + b'{"quit": true}'
    with socket.socket(socket.AF_UNIX) as c:
        c.connect(sock)
        c.sendall(struct.pack("<I", len(body)) + body)
        try:
            c.recv(4)
        except OSError:
            pass


offline = {}
CMDS = commands()
# SAMPLE=N: every Nth compile only. FILTER=<regex>: only the compiles of
# the files it finds.
CMDS = CMDS[::int(os.environ.get("SAMPLE", "1"))]
if os.environ.get("FILTER"):
    CMDS = [c for c in CMDS if re.search(os.environ["FILTER"], c[2])]
print(f"{proj} {KIND}: {len(CMDS)} compiles, modes {' '.join(MODES)}", flush=True)
objdir = tempfile.mkdtemp(prefix=f"mbo-{proj}-")
DUMP = os.environ.get("DUMP")
cores = os.environ.get("DCORES", "8,9,10").split(",")
daemons = {m: daemon(cores[i % len(cores)], DUMP and f"{DUMP}-{m}.tsv", m) for i, m in enumerate(CM)
           if not is_server(m)}
servers = {m: server(m) for m in CM if is_server(m)}
SV = {m: [] for m in servers}
base = {k: v for k, v in os.environ.items() if not k.startswith(("CLANG_HEADER_CACHE", "CLANG_COMPILE_SERVER"))}
# One __DATE__ and __TIME__ for every compile whose object is compared
# (cpython's getbuildinfo.c records them).
if KIND == "full":
    base["SOURCE_DATE_EPOCH"] = "1700000000"
env = {"plain": dict(base)}
# EXTRA="K=V ...": more settings for every cache mode (measurement switches).
extra = dict(kv.split("=", 1) for kv in os.environ.get("EXTRA", "").split())
for m in CM:
    if is_server(m):
        env[m] = dict(base, CLANG_COMPILE_SERVER=servers[m][1], CLANG_COMPILE_SERVER_BUILD=f"{proj}-{m}-{time.time()}")
        continue
    # EXTRA_<mode> ('@' written as '_'): more settings for that mode alone.
    own = dict(kv.split("=", 1) for kv in os.environ.get("EXTRA_" + m.replace("@", "_"), "").split())
    env[m] = dict(base, CLANG_HEADER_CACHE=daemons[m][1], CLANG_HEADER_CACHE_STATS=os.environ.get("LEVEL", "1"),
                  CLANG_HEADER_CACHE_BUILD=f"{proj}-{m}-{time.time()}", **{**ALL[basemode(m)], **extra, **own})
PASSES = int(os.environ.get("PASSES", "1"))
# INSTR=1: count each compile's instructions (perfc), stable where the host
# is loaded; kept as "I" in the trace and summed in the summary line.
PERFC = [os.environ.get("PERFC", os.path.join(os.path.dirname(os.path.abspath(__file__)), "perf", "perfc"))] if os.environ.get("INSTR") == "1" else []
passes = []
for k in range(1, PASSES + 1):
  if k == 2:
    # A rebuild on the same daemons: the offline step between the builds.
    run_offline()
  if k > 1:
    passes.append({"T": T, "S": S, "W": W, "failed": failed, "I": I})
    for m in CM:
        if is_server(m):
            env[m]["CLANG_COMPILE_SERVER_BUILD"] = f"{proj}-{m}-pass{k}-{time.time()}"
            continue
        env[m]["CLANG_HEADER_CACHE_BUILD"] = f"{proj}-{m}-pass{k}-{time.time()}"
    # CHANGE_CMD: run before each later pass, to change the sources the
    # rebuild reads (invalidation test); CHANGE_UNDO at the end.
    if os.environ.get("CHANGE_CMD"):
        subprocess.run(os.environ["CHANGE_CMD"], shell=True, check=True)
  T = {m: [] for m in MODES}; S = {m: [] for m in CM}; W = {m: [] for m in MODES}; failed = []
  I = {m: [] for m in MODES}
  for i, (cwd, args, src) in enumerate(CMDS):
    for m in MODES[i % len(MODES):] + MODES[:i % len(MODES)]:
        obj = f"{objdir}/{m}/{i}.o" if k == 1 else f"{objdir}/{m}/pass{k}-{i}.o"
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        t = time.perf_counter()
        a = replay_args(args, obj)
        if (m == "plain" and not PLAIN_MODULES) or is_server(m):
            a = a[2:]
        r = subprocess.run(["taskset", "-c", CORE] + PERFC + [f"{binof(m)}/clang"] + a, cwd=cwd, env=env[m],
                           capture_output=True, text=True)
        T[m].append((time.perf_counter() - t) * 1000)
        got = server_results(servers[m][0]) if is_server(m) else []
        if is_server(m):
            SV[m].append({"pass": k, "i": i, **{f: g.get(f) for g in got[-1:] for f in
                          ("status", "error", "reused", "parsed", "instructions", "cpu_ms", "wall_ms", "parse_ms",
                           "reset_ms", "sloc_bytes", "new_space", "spaces_replaced", "rss_kib", "restored_decls")},
                          "requests": len(got)})
        if PERFC:
            g = re.search(r"instructions=(\d+)", r.stderr)
            I[m].append((int(g[1]) if g else 0) + sum(x.get("instructions", 0) for x in got))
            r.stderr = "\n".join(l for l in r.stderr.splitlines() if "instructions=" not in l) + "\n"
        if r.returncode:
            # The start says why (an assertion), the end where.
            err = r.stderr if len(r.stderr) <= 7000 else r.stderr[:6400] + "\n...\n" + r.stderr[-600:]
            failed.append((m, src, err))
        W[m].append("\n".join(l for l in r.stderr.splitlines() if "header cache" not in l and not l.startswith("  ")))
        if m != "plain":
            S[m].append(r.stderr if os.environ.get("LEVEL", "1") != "1" else
                        "\n".join(l for l in r.stderr.splitlines() if "header cache" in l or l.startswith("  rejected:")))
  if k > 1:
    print(f"{proj} {KIND} pass {k}: " + ", ".join(
        f"{m} {sum(T[m]) / 1000:.2f} s ({sum(T['plain']) / sum(T[m]):.2f}x, {sum(1 for f in failed if f[0] == m)} failed)"
        for m in MODES), flush=True)
if os.environ.get("CHANGE_UNDO"):
    subprocess.run(os.environ["CHANGE_UNDO"], shell=True, check=True)
I1 = None
if PASSES > 1:
    passes.append({"T": T, "S": S, "W": W, "failed": failed, "I": I})
    T, S, W, failed = passes[0]["T"], passes[0]["S"], passes[0]["W"], passes[0]["failed"]
    # "I" stays the last pass's; "I1" is pass 1's, each pass's "I" its own.
    I1 = passes[0]["I"]
    passes = passes[1:]
if PASSES == 1:
    run_offline()
stored, picks = {}, {}
for m, (p, _) in daemons.items():
    p.terminate(); p.wait()
    if os.path.exists(p.region):
        os.remove(p.region)
    stored[m] = open(p.log).read()
    # -merge-picks: the picks and why each was picked, beside the region.
    if os.path.exists(p.region + ".picks.log"):
        picks[m] = {"log": open(p.region + ".picks.log").read(),
                    "picks": open(p.region + ".picks").read() if os.path.exists(p.region + ".picks") else ""}
    for f in (p.region + ".picks", p.region + ".picks.log", p.region + ".picks.tmp"):
        if os.path.exists(f):
            os.remove(f)
for m, (p, sock) in servers.items():
    try:
        quit_server(sock)
        p.wait(timeout=60)
    except Exception:
        p.kill(); p.wait()
    stored[m] = open(p.log).read()[-20000:]
# Against plain clang's object from the same pass: cpython's getbuildinfo.c
# records __TIME__.
def differs(m, pre=""):
    return [CMDS[i][2] for i in range(len(CMDS))
            if not (os.path.exists(f"{objdir}/plain/{pre}{i}.o") and os.path.exists(f"{objdir}/{m}/{pre}{i}.o"))
            or open(f"{objdir}/plain/{pre}{i}.o", "rb").read() != open(f"{objdir}/{m}/{pre}{i}.o", "rb").read()]
differ = {}
if KIND == "full":
    for m in CM:
        differ[m] = differs(m)
        for k, P in enumerate(passes, 2):
            P["differ"] = dict(P.get("differ", {}), **{m: differs(m, f"pass{k}-")})
json.dump({"cmds": [c[2] for c in CMDS], "modes": MODES, "T": T, "S": S, "W": W, "failed": failed, "differ": differ,
           "daemon": stored, "passes": passes, "I": I, "I1": I1, "offline": offline, "server": SV, "picks": picks}, open(out, "w"))
# The trace keeps what the objects and the daemons' folders gave: they go,
# as each run would leave 0.4-4.4 GB in /tmp. KEEP_OBJECTS=1 keeps the
# objects, the differing ones only, to look into: of every pass, a compile
# whose object differs in any of them.
if os.environ.get("KEEP_OBJECTS") == "1":
    keep = {i for i, c in enumerate(CMDS)
            if any(c[2] in v for d in [differ] + [P.get("differ", {}) for P in passes] for v in d.values())}
    for m in MODES:
        for f in os.listdir(f"{objdir}/{m}") if os.path.isdir(f"{objdir}/{m}") else []:
            if int(f.split(".")[0].split("-")[-1]) not in keep:
                os.remove(f"{objdir}/{m}/{f}")
    print(f"objects kept in {objdir}", flush=True)
else:
    shutil.rmtree(objdir, ignore_errors=True)
for p, _ in list(daemons.values()) + list(servers.values()):
    shutil.rmtree(os.path.dirname(p.log), ignore_errors=True)
for m, rows in SV.items():
    st = [r for r in rows if r.get("pass") == 1]
    print(f"{proj} {KIND} {m}: {sum(1 for r in st if r.get('status') == 2)} refused, "
          f"{sum(1 for r in st if r.get('requests') != 1)} compiles without exactly one request, "
          f"{max((r.get('spaces_replaced') or 0) for r in st) if st else 0} memory spaces replaced, "
          f"{sum(r.get('reused') or 0 for r in st)} fragments reused, {sum(r.get('parsed') or 0 for r in st)} parsed",
          flush=True)
tot = {m: sum(T[m]) / 1000 for m in MODES}
nf = {m: sum(1 for f in failed if f[0] == m) for m in MODES}
print(f"{proj} {KIND}: " + ", ".join(f"{m} {tot[m]:.2f} s ({tot['plain'] / tot[m]:.2f}x, {nf[m]} failed"
                                     + (f", {len(differ[m])} objects differ" if m in differ else "") + ")" for m in MODES), flush=True)
if PERFC and I1:
    gi = {m: sum(I1[m]) / 1e9 for m in MODES}
    print(f"{proj} {KIND} pass 1 instructions: " + ", ".join(f"{m} {gi[m]:.1f} G ({gi['plain'] / gi[m]:.2f}x)" for m in MODES), flush=True)
if PERFC:
    gi = {m: sum(I[m]) / 1e9 for m in MODES}
    print(f"{proj} {KIND} instructions: " + ", ".join(f"{m} {gi[m]:.1f} G ({gi['plain'] / gi[m]:.2f}x)" for m in MODES), flush=True)
