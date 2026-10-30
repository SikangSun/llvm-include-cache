"""The header cache's recompilation of a recorded build, beside plain clang
(COMPILE_SERVER_PLAN.md 3.5): a fresh daemon; a first build through the cache
fills it (not timed); then every compile again, timed, under a new build ID:
plain clang as a user runs it, plain clang with the modules flags the cache
needs, and the cache in its default mode (top-level folding), interleaved
compile by compile with the order rotated, the best of REPS wall times each.
The front end only. Compiles are pinned to CORE, the daemon to DCORE.
usage: recompile.py <out.json>
env: REPS (default 3), CORE (default 3), DCORE (default 5), LOGS, BIN
"""
import json, os, re, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compiles import BIN, CLANG, ENV, MOD, commands, config

REPS = int(os.environ.get("REPS", "3"))
CORE = os.environ.get("CORE", "3")
DCORE = os.environ.get("DCORE", "5")
out = sys.argv[1]

work = tempfile.mkdtemp(prefix="br-")  # short: a socket path has a length limit
sock, region = os.path.join(work, "sock"), f"/dev/shm/{os.path.basename(work)}.region"
daemon = subprocess.Popen(["taskset", "-c", DCORE, os.path.join(BIN, "clang-header-cache-daemon"),
                           "-socket", sock, "-region", region, "-v"],
                          stdout=open(os.path.join(work, "daemon.log"), "w"), stderr=subprocess.STDOUT)
while not os.path.exists(sock):
    time.sleep(0.01)
stamp = str(time.time())
cache_env = lambda build: dict(ENV, CLANG_HEADER_CACHE=sock, CLANG_HEADER_CACHE_STATS="1",
                               CLANG_HEADER_CACHE_NO_SUBMODULES="1", CLANG_HEADER_CACHE_BUILD=build)
HITS = re.compile(r"header cache: (\d+) hits / (\d+) lookups")


def wall(argv, cwd, env):
    s = time.perf_counter()
    r = subprocess.run(["taskset", "-c", CORE] + argv, cwd=cwd, env=env, capture_output=True, text=True)
    return time.perf_counter() - s, r


try:
    cmds = commands()
    fill = cache_env(f"fill-{stamp}")
    for cwd, args, src in cmds:
        wall([CLANG, "-fsyntax-only"] + MOD + config(args, src) + [src], cwd, fill)
    print(f"first build done: {len(cmds)} compiles", flush=True)

    again = cache_env(f"recompile-{stamp}")
    res = {}
    for i, (cwd, args, src) in enumerate(cmds):
        flags = config(args, src) + [src]
        modes = {"plain": ([CLANG, "-fsyntax-only"] + flags, ENV),
                 "plainM": ([CLANG, "-fsyntax-only"] + MOD + flags, ENV),
                 "cache": ([CLANG, "-fsyntax-only"] + MOD + flags, again)}
        names = list(modes)
        best, rc, hits = {m: float("inf") for m in modes}, {}, None
        for k in range(REPS):
            for m in names[(i + k) % 3:] + names[:(i + k) % 3]:
                argv, env = modes[m]
                t, r = wall(argv, cwd, env)
                best[m] = min(best[m], t)
                rc[m] = r.returncode
                if m == "cache" and (h := HITS.search(r.stderr)):
                    hits = [int(h.group(1)), int(h.group(2))]
        if any(rc.values()):
            print(f"{i} {src}: failed {rc}", flush=True)
            continue
        res[str(i)] = dict(best, src=src, cwd=cwd, hits=hits)
        print(f"{i:3d} {src:40} plain {best['plain'] * 1e3:7.1f}  modules {best['plainM'] * 1e3:7.1f}  "
              f"cache {best['cache'] * 1e3:7.1f} ms  hits {hits}", flush=True)
finally:
    daemon.terminate()
    daemon.wait()
    if os.path.exists(region):
        os.remove(region)

P = sum(r["plain"] for r in res.values())
for m in ("plainM", "cache"):
    T = sum(r[m] for r in res.values())
    print(f"{m}: {T:.2f} s against plain clang's {P:.2f} s: {P / T:.2f}x")
json.dump({"reps": REPS, "compiles": res}, open(out, "w"))
