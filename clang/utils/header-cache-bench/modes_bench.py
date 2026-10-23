"""Plain clang and several cache modes, same binary, one project, first
builds, one compile at a time, modes interleaved file by file with the order
rotated; each cache mode with its own fresh daemon, build ID and per-copy dump
(CLANG_HEADER_CACHE_DAEMON_DUMP). KIND=syntax runs the front end only; full
compares each mode's objects with plain clang's.
usage: modes_bench.py <project> <out.json>
env: KIND, CORE, DCORES (comma list, one per cache mode), BIN, LEVEL, DUMP, MODES,
SRC_ROOT, PLAIN_MODULES.

env, required: BIN (a release build's bin directory), BENCH_LOGS, BENCH_ROOT
(see record_builds.sh).

Sources are read from DRAM: SRC_ROOT (default /dev/shm/bench-src, filled by
dram_sources.sh) stands for BENCH_ROOT; SRC_ROOT=in-place reads them there. Plain clang runs as a user runs it, without
the -fmodules -fno-implicit-module-maps only the cache needs (front end);
PLAIN_MODULES=1 gives it them. Full builds keep them on plain clang, whose
objects are compared with the cache's."""
import json, os, subprocess, sys, tempfile, time
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
    "link": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_LINK": "1"},
    "miss1": {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1", "CLANG_HEADER_CACHE_SECOND_MISS": "1"},
    # Headers inside a copy held as sections (REPORT.md T17), skipped by guard.
    "sections": {"CLANG_HEADER_CACHE_SECTIONS": "1"},
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
}
CM = os.environ.get("MODES", "top link miss1").split()
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


def daemon(core, dump):
    w = tempfile.mkdtemp(prefix="mb-")
    region = f"/dev/shm/{os.path.basename(w)}.region"
    p = subprocess.Popen(["taskset", "-c", core, f"{BIN}/clang-header-cache-daemon", "-socket", f"{w}/sock",
                          "-region", region, "-v"], stdout=open(f"{w}/daemon.log", "w"), stderr=subprocess.STDOUT,
                         env=dict(os.environ, **({"CLANG_HEADER_CACHE_DAEMON_DUMP": dump} if dump else {})))
    while not os.path.exists(f"{w}/sock"):
        time.sleep(0.01)
    p.log, p.region = f"{w}/daemon.log", region
    return p, f"{w}/sock"


CMDS = commands()
print(f"{proj} {KIND}: {len(CMDS)} compiles, modes {' '.join(MODES)}", flush=True)
objdir = tempfile.mkdtemp(prefix=f"mbo-{proj}-")
DUMP = os.environ.get("DUMP")
cores = os.environ.get("DCORES", "8,9,10").split(",")
daemons = {m: daemon(cores[i % len(cores)], DUMP and f"{DUMP}-{m}.tsv") for i, m in enumerate(CM)}
base = {k: v for k, v in os.environ.items() if not k.startswith("CLANG_HEADER_CACHE")}
env = {"plain": dict(base)}
# EXTRA="K=V ...": more settings for every cache mode (measurement switches).
extra = dict(kv.split("=", 1) for kv in os.environ.get("EXTRA", "").split())
for m in CM:
    env[m] = dict(base, CLANG_HEADER_CACHE=daemons[m][1], CLANG_HEADER_CACHE_STATS=os.environ.get("LEVEL", "1"),
                  CLANG_HEADER_CACHE_BUILD=f"{proj}-{m}-{time.time()}", **ALL[m], **extra)
T = {m: [] for m in MODES}; S = {m: [] for m in CM}; W = {m: [] for m in MODES}; failed = []
for i, (cwd, args, src) in enumerate(CMDS):
    for m in MODES[i % len(MODES):] + MODES[:i % len(MODES)]:
        obj = f"{objdir}/{m}/{i}.o"
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        t = time.perf_counter()
        a = replay_args(args, obj)
        if m == "plain" and not PLAIN_MODULES:
            a = a[2:]
        r = subprocess.run(["taskset", "-c", CORE, f"{BIN}/clang"] + a, cwd=cwd, env=env[m],
                           capture_output=True, text=True)
        T[m].append((time.perf_counter() - t) * 1000)
        if r.returncode:
            failed.append((m, src, r.stderr[-600:]))
        W[m].append("\n".join(l for l in r.stderr.splitlines() if "header cache" not in l and not l.startswith("  ")))
        if m != "plain":
            S[m].append(r.stderr if os.environ.get("LEVEL", "1") != "1" else
                        "\n".join(l for l in r.stderr.splitlines() if "header cache" in l or l.startswith("  rejected:")))
stored = {}
for m, (p, _) in daemons.items():
    p.terminate(); p.wait()
    if os.path.exists(p.region):
        os.remove(p.region)
    stored[m] = open(p.log).read()
differ = {}
if KIND == "full":
    for m in CM:
        differ[m] = [CMDS[i][2] for i in range(len(CMDS))
                     if not (os.path.exists(f"{objdir}/plain/{i}.o") and os.path.exists(f"{objdir}/{m}/{i}.o"))
                     or open(f"{objdir}/plain/{i}.o", "rb").read() != open(f"{objdir}/{m}/{i}.o", "rb").read()]
json.dump({"cmds": [c[2] for c in CMDS], "modes": MODES, "T": T, "S": S, "W": W, "failed": failed, "differ": differ,
           "daemon": stored}, open(out, "w"))
tot = {m: sum(T[m]) / 1000 for m in MODES}
nf = {m: sum(1 for f in failed if f[0] == m) for m in MODES}
print(f"{proj} {KIND}: " + ", ".join(f"{m} {tot[m]:.2f} s ({tot['plain'] / tot[m]:.2f}x, {nf[m]} failed"
                                     + (f", {len(differ[m])} objects differ" if m in differ else "") + ")" for m in MODES), flush=True)
