"""Parallel first builds: plain clang against the cache, the build's compiles
run N at a time (like make -jN), N in JOBS. Wall time from the first compile
started to the last one finished. Each cache run has a fresh daemon and build
ID. Nothing is pinned: compiles and the daemon go where the scheduler puts
them, as under make. At each N, plain and cache alternate which goes first.
usage: par_bench.py <project> <out.json>
env: BIN, JOBS (default "1 2 4 8 16 32 64"), KIND (syntax|full), SRC_ROOT,
LEVEL, MODES (cache versions, default "top"; each its own fresh daemon, the
order rotated at each N).

Sources are read from DRAM (SRC_ROOT, default /dev/shm/bench-src). Plain
clang runs without -fmodules -fno-implicit-module-maps on the front end."""
import json, os, subprocess, sys, tempfile, time, shutil
from concurrent.futures import ThreadPoolExecutor

BIN = os.environ["BIN"]
# Where record_builds.sh put the compile logs, and the directory the
# projects were built in when it recorded them.
LOGS = os.environ["BENCH_LOGS"]
ROOT = os.environ["BENCH_ROOT"].rstrip("/") + "/"
proj, out = sys.argv[1], sys.argv[2]
KIND = os.environ.get("KIND", "syntax")
SRC = os.environ.get("SRC_ROOT", "/dev/shm/bench-src").rstrip("/") + "/"
JOBS = [int(j) for j in os.environ.get("JOBS", "1 2 4 8 16 32 64").split()]
TOP = {"CLANG_HEADER_CACHE_NO_SUBMODULES": "1"}
ALL = {
    "top": TOP,
    # Copies sent by a thread of the compile's own.
    "send": dict(TOP, CLANG_HEADER_CACHE_SEND_THREAD="1"),
    # ... and only the compile that claims a copy writes it.
    "claim": dict(TOP, CLANG_HEADER_CACHE_SEND_THREAD="1", CLANG_HEADER_CACHE_CLAIM="1"),
    # ... refused copies written anyway, unsent, and compared with the stored.
    "claimcheck": dict(TOP, CLANG_HEADER_CACHE_SEND_THREAD="1", CLANG_HEADER_CACHE_CLAIM="check"),
    # ... and a lookup waits for a copy another compile is writing.
    "wait": dict(TOP, CLANG_HEADER_CACHE_WAIT="1"),
    "firstread": dict(TOP, CLANG_HEADER_CACHE_FIRST_READ="1"),
    # Claims keyed by the copy's name and the outside declarations its text
    # named, as parsing resolved them; refused copies checked as above.
    "deps": dict(TOP, CLANG_HEADER_CACHE_SEND_THREAD="1", CLANG_HEADER_CACHE_CLAIM="deps"),
    "depscheck": dict(TOP, CLANG_HEADER_CACHE_SEND_THREAD="1", CLANG_HEADER_CACHE_CLAIM="depscheck"),
}
# bin-u32 (2026-10-31): linked copies, and with the history-based merge
# rule; with a cutoff, the daemon's -ask-cutoff-us (DAEMON_ARGS).
LINK = dict(TOP, CLANG_HEADER_CACHE_COUNTER="1", CLANG_HEADER_CACHE_LINK="1")
ALL.update({
    "linkc": LINK,
    "linkma": dict(LINK, CLANG_HEADER_CACHE_MERGE="auto", CLANG_HEADER_CACHE_MERGE_WAIT="1"),
    "linkcut2000ma": dict(LINK, CLANG_HEADER_CACHE_MERGE="auto", CLANG_HEADER_CACHE_MERGE_WAIT="1"),
})
DAEMON_ARGS = {"linkcut2000ma": ["-ask-cutoff-us", "2000"]}
CM = os.environ.get("MODES", "top").split()


def commands():
    d = f"{LOGS}/log-{proj}"
    cmds = []
    for name in sorted(os.listdir(d), key=lambda n: int(n.split(".")[0])):
        parts = open(f"{d}/{name}", "rb").read().decode(errors="replace").split("\0")[:-1]
        cwd, args = parts[0], parts[1:]
        cwd = cwd.replace(ROOT, SRC)
        args = [a.replace(ROOT, SRC) for a in args]
        if "-c" not in args or "-E" in args:
            continue
        srcs = [a for i, a in enumerate(args) if a.endswith(".c") and args[i - 1] not in ("-o", "-MF", "-MT", "-MQ")]
        if len(srcs) != 1 or "conftest" in srcs[0]:
            continue
        cmds.append((cwd, args, srcs[0]))
    return cmds


def replay_args(args, obj, plain):
    a = list(args)
    if "-o" in a:
        a[a.index("-o") + 1] = obj
    else:
        a += ["-o", obj]
    if KIND == "syntax":
        i = a.index("-o")
        del a[i:i + 2]
        a[a.index("-c")] = "-fsyntax-only"
        if plain:
            return a
    return ["-fmodules", "-fno-implicit-module-maps"] + a


def daemon(mode=None):
    w = tempfile.mkdtemp(prefix="pb-")
    region = f"/dev/shm/{os.path.basename(w)}.region"
    size = ["-region-size", str(int(os.environ["REGION_SIZE"]) << 30)] if os.environ.get("REGION_SIZE") else []
    p = subprocess.Popen([f"{BIN}/clang-header-cache-daemon", "-socket", f"{w}/sock", "-region", region, "-v"]
                         + size + DAEMON_ARGS.get(mode, []),
                         stdout=open(f"{w}/daemon.log", "w"), stderr=subprocess.STDOUT)
    while not os.path.exists(f"{w}/sock"):
        time.sleep(0.01)
    return p, f"{w}/sock", region, w


def build(cmds, jobs, env, plain, objdir):
    """Runs every compile, jobs at a time, in the recorded order. Each
    result has the compile's start and end, in ms from the build's start."""
    t0 = time.perf_counter()
    def one(i):
        cwd, args, src = cmds[i]
        t = time.perf_counter()
        r = subprocess.run([f"{BIN}/clang"] + replay_args(args, f"{objdir}/{i}.o", plain), cwd=cwd, env=env,
                           capture_output=True, text=True)
        e = time.perf_counter()
        return i, (e - t) * 1000, r.returncode, r.stderr, (t - t0) * 1000, (e - t0) * 1000
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        res = list(ex.map(one, range(len(cmds))))
    return time.perf_counter() - t0, res


CMDS = commands()
print(f"{proj} {KIND}: {len(CMDS)} compiles, jobs {JOBS}", flush=True)
base = {k: v for k, v in os.environ.items() if not k.startswith("CLANG_HEADER_CACHE")}
# Full builds compare objects from runs minutes apart: one fixed __DATE__ and
# __TIME__ for both (cpython's getbuildinfo.c records them).
if KIND == "full":
    base["SOURCE_DATE_EPOCH"] = "1700000000"
runs = []
MODES = ["plain"] + CM
for n, jobs in enumerate(JOBS):
    order = MODES[n % len(MODES):] + MODES[:n % len(MODES)]
    row = {"jobs": jobs}
    for m in order:
        objdir = tempfile.mkdtemp(prefix=f"pbo-{proj}-{m}-")
        if m == "plain":
            wall, res = build(CMDS, jobs, base, True, objdir)
            row[m] = {}
        else:
            p, sock, region, w = daemon(m)
            env = dict(base, CLANG_HEADER_CACHE=sock, CLANG_HEADER_CACHE_STATS=os.environ.get("LEVEL", "1"),
                       CLANG_HEADER_CACHE_BUILD=f"{proj}-par{jobs}-{m}-{time.time()}", **ALL[m])
            wall, res = build(CMDS, jobs, env, False, objdir)
            p.terminate(); p.wait()
            row[m] = {"daemon": open(f"{w}/daemon.log").read()[-2000:]}
            if os.path.exists(region):
                os.remove(region)
            shutil.rmtree(w, ignore_errors=True)
            # LEVEL=2 keeps every line (each request's outcome), and when each
            # compile ran.
            row[m]["stats"] = [r[3] if os.environ.get("LEVEL", "1") != "1" else
                               "\n".join(l for l in r[3].splitlines() if l.startswith("header cache")) for r in res]
            row[m]["spans"] = [(r[4], r[5]) for r in res]
        row[m].update({"wall": wall, "cpu_ms": sum(r[1] for r in res),
                       "failed": [(CMDS[r[0]][2], r[3][-600:]) for r in res if r[2]]})
        # Each compile's diagnostics, the cache's own lines left out.
        row[m]["warnings"] = ["\n".join(l for l in r[3].splitlines()
                                        if "header cache" not in l and not l.startswith("  ")) for r in res]
        if KIND == "full":
            row[m]["objdir"] = objdir
        else:
            shutil.rmtree(objdir, ignore_errors=True)
    line = [f"{proj} -j{jobs}: plain {row['plain']['wall']:.2f} s"]
    for m in CM:
        row[m]["warn_differ"] = [CMDS[i][2] for i in range(len(CMDS))
                                 if row["plain"]["warnings"][i] != row[m]["warnings"][i]]
        if KIND == "full":
            a, b = row["plain"]["objdir"], row[m]["objdir"]
            row[m]["differ"] = [CMDS[i][2] for i in range(len(CMDS))
                                if not (os.path.exists(f"{a}/{i}.o") and os.path.exists(f"{b}/{i}.o"))
                                or open(f"{a}/{i}.o", "rb").read() != open(f"{b}/{i}.o", "rb").read()]
        line.append(f"{m} {row[m]['wall']:.2f} s ({row['plain']['wall'] / row[m]['wall']:.2f}x, "
                    f"{len(row[m]['failed'])} failed, {len(row[m]['warn_differ'])} warnings differ"
                    + (f", {len(row[m]['differ'])} objects differ" if KIND == "full" else "") + ")")
    if KIND == "full":
        for m in MODES:
            shutil.rmtree(row[m].pop("objdir"), ignore_errors=True)
    for m in MODES:
        row[m].pop("warnings", None)
    runs.append(row)
    print(", ".join(line), flush=True)
json.dump({"cmds": [c[2] for c in CMDS], "modes": MODES, "runs": runs}, open(out, "w"))
