"""Header-graph measurement (CLANG_HEADER_CACHE_DDG): every compile of a
project, front end only, all headers read as text, run in parallel (no
timing). usage: ddg_run.py <project> <outdir> [jobs]"""
import os, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(__file__))
BIN = os.environ["BIN"]
LOGS = os.environ["BENCH_LOGS"]
proj, out = sys.argv[1], sys.argv[2]
jobs = int(sys.argv[3]) if len(sys.argv) > 3 else 16
os.makedirs(out, exist_ok=True)
d = f"{LOGS}/log-{proj}"
cmds = []
for name in sorted(os.listdir(d), key=lambda n: int(n.split(".")[0])):
    parts = open(f"{d}/{name}", "rb").read().decode(errors="replace").split("\0")[:-1]
    cwd, args = parts[0], parts[1:]
    if "-c" not in args or "-E" in args:
        continue
    srcs = [a for i, a in enumerate(args) if a.endswith(".c") and args[i - 1] not in ("-o", "-MF", "-MT", "-MQ")]
    if len(srcs) != 1 or "conftest" in srcs[0]:
        continue
    a = list(args)
    if "-o" in a:
        i = a.index("-o"); del a[i:i + 2]
    a[a.index("-c")] = "-fsyntax-only"
    cmds.append((cwd, ["-fmodules", "-fno-implicit-module-maps"] + a))
w = tempfile.mkdtemp(prefix="ddg-")
region = f"/dev/shm/{os.path.basename(w)}.region"
dp = subprocess.Popen([f"{BIN}/clang-header-cache-daemon", "-socket", f"{w}/sock", "-region", region],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
while not os.path.exists(f"{w}/sock"):
    time.sleep(0.01)
env = {k: v for k, v in os.environ.items() if not k.startswith("CLANG_HEADER_CACHE")}
env.update(CLANG_HEADER_CACHE=f"{w}/sock", CLANG_HEADER_CACHE_DDG=out, CLANG_HEADER_CACHE_NO_SUBMODULES="1",
           CLANG_HEADER_CACHE_BUILD=f"ddg-{proj}")
def run(c):
    cwd, args = c
    r = subprocess.run([f"{BIN}/clang"] + args, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode:
        with open(f"{out}/failed.txt", "a") as f:
            f.write(f"== {cwd} {args[-1]} {r.returncode}\n{r.stderr[:12000]}\n")
    return r.returncode
with ThreadPoolExecutor(jobs) as ex:
    rcs = list(ex.map(run, cmds))
dp.terminate(); dp.wait()
if os.path.exists(region):
    os.remove(region)
print(proj, len(cmds), "compiles,", sum(1 for r in rcs if r), "failed", flush=True)
