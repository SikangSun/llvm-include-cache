"""Correctness of a parallel first build: every logged compile of a project
through one fresh daemon, JOBS at a time (as make -jN would run them), then
the same compiles as plain clang; failures and objects compared.
par_check2 (2026-10-31): the cache's settings from MODE_ENV ("K=V ..."),
the region on /dev/shm (REGION_SIZE GiB), sources from DRAM (SRC_ROOT),
SOURCE_DATE_EPOCH fixed, and each run's wall time.

usage: par_check.py <project> <jobs> <out.json>
env: CORES (compile cores, e.g. 2-9), DCORE, BIN, MODE_ENV, REGION_SIZE, SRC_ROOT,
BENCH_LOGS, BENCH_ROOT."""
import concurrent.futures as cf, json, os, subprocess, sys, tempfile, time

BIN = os.environ["BIN"]
# Where record_builds.sh put the compile logs, and the directory the
# projects were built in when it recorded them.
LOGS = os.environ["BENCH_LOGS"]
ROOT = os.environ["BENCH_ROOT"].rstrip("/") + "/"
proj, jobs, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
SRC = os.environ.get("SRC_ROOT", "/dev/shm/bench-src").rstrip("/") + "/"
MODE_ENV = dict(kv.split("=", 1) for kv in os.environ.get("MODE_ENV", "").split())
CORES, DCORE = os.environ.get("CORES", "2-9"), os.environ.get("DCORE", "10")


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


def run(i, cwd, args, obj, env, cache):
    a = list(args)
    if "-o" in a:
        a[a.index("-o") + 1] = obj
    else:
        a += ["-o", obj]
    flags = ["-fmodules", "-fno-implicit-module-maps"]
    r = subprocess.run(["taskset", "-c", CORES, f"{BIN}/clang"] + flags + a,
                       cwd=cwd, env=env, capture_output=True, text=True)
    return i, r.returncode, r.stderr[-400:]


w = tempfile.mkdtemp(prefix="par-")
region = f"/dev/shm/{os.path.basename(w)}.region"
size = ["-region-size", str(int(os.environ["REGION_SIZE"]) << 30)] if os.environ.get("REGION_SIZE") else []
d = subprocess.Popen(["taskset", "-c", DCORE, f"{BIN}/clang-header-cache-daemon", "-socket", f"{w}/sock",
                      "-region", region, "-v"] + size, stdout=open(f"{w}/daemon.log", "w"), stderr=subprocess.STDOUT)
while not os.path.exists(f"{w}/sock"):
    time.sleep(0.01)
CMDS = commands()
res = {}
base = {k: v for k, v in os.environ.items() if not k.startswith("CLANG_HEADER_CACHE")}
base["SOURCE_DATE_EPOCH"] = "1700000000"
wall = {}
for mode, env in (("cache", dict(base, CLANG_HEADER_CACHE=f"{w}/sock", CLANG_HEADER_CACHE_STATS="1",
                                 CLANG_HEADER_CACHE_BUILD=f"{proj}-par-{time.time()}", **MODE_ENV)),
                  ("plain", dict(base))):
    os.makedirs(f"{w}/{mode}", exist_ok=True)
    t = time.perf_counter()
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = [ex.submit(run, i, cwd, args, f"{w}/{mode}/{i}.o", env, mode == "cache")
                for i, (cwd, args, src) in enumerate(CMDS)]
        res[mode] = sorted(f.result() for f in futs)
    wall[mode] = time.perf_counter() - t
    print(f"{proj} {mode}: {wall[mode]:.1f} s wall, -j{jobs}", flush=True)
d.terminate(); d.wait()
if os.path.exists(region):
    os.remove(region)
failed = {m: [(CMDS[i][2], e) for i, rc, e in res[m] if rc] for m in res}
differ = []
for i in range(len(CMDS)):
    a, b = f"{w}/plain/{i}.o", f"{w}/cache/{i}.o"
    if not (os.path.exists(a) and os.path.exists(b)) or open(a, "rb").read() != open(b, "rb").read():
        differ.append((i, CMDS[i][2]))
json.dump({"failed": failed, "differ": differ, "dir": w, "wall": wall, "jobs": jobs,
           "daemon": open(f"{w}/daemon.log").read()}, open(out, "w"), indent=1)
print(f"{proj}: failed cache {len(failed['cache'])}, plain {len(failed['plain'])}; objects differing {len(differ)}"
      + (f" {differ[:8]}" if differ else ""))
for s, e in failed["cache"][:3]:
    print("  FAILED", s, e[-300:])
