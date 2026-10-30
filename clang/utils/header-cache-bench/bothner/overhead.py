"""BothnerTrace's cost on real compiles, the front end only, interleaved, the
best of REPS wall times each, pinned to CORE:
  plain     plain clang;
  loaded    the plugin loaded and recording nothing (BOTHNER_TRACE_OFF):
            loading it is a fixed cost, before the span the plugin measures;
  traced    the plugin recording, with no output written.
"In span" is traced against loaded: the part that can distort what the plugin
charges. "Output" is the time to compute and write the output, after the span.
usage: overhead.py [source names...]    env: REPS (default 7), CORE (default 3)
"""
import os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compiles import ENV, commands, front_end

TRACE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "BothnerTrace.so")
REPS = int(os.environ.get("REPS", "7"))
CORE = os.environ.get("CORE", "3")
PICK = sys.argv[1:] or ["server.c", "networking.c", "module.c", "cluster_legacy.c", "redis-cli.c",
                         "sds.c", "adlist.c", "lapi.c", "hiredis.c", "linenoise.c"]


def wall(argv, cwd, env):
    s = time.perf_counter()
    r = subprocess.run(["taskset", "-c", CORE] + argv, cwd=cwd, env=env, capture_output=True)
    if r.returncode:
        sys.exit(f"failed in {cwd}: {' '.join(argv)}\n{r.stderr.decode()[-2000:]}")
    return time.perf_counter() - s


chosen = {}
for cwd, args, src in commands():
    chosen.setdefault(os.path.basename(src), (cwd, args, src))
total, span = [], []
print(f"{'source':18} {'plain ms':>9} {'loaded':>8} {'traced':>8} {'total':>7} {'in span':>8} {'output ms':>9}")
for name in PICK:
    if name not in chosen:
        print(f"{name}: not among the recorded compiles")
        continue
    cwd, args, src = chosen[name]
    plain = front_end(cwd, args, src)
    traced = front_end(cwd, args, src, ["-fplugin=" + TRACE])
    out = os.path.join("/tmp", f"bothner-overhead-{os.getpid()}.json")
    runs = {"plain": (plain, ENV), "loaded": (traced, dict(ENV, BOTHNER_TRACE_OFF="callbacks,tokens")),
            "traced": (traced, ENV), "output": (traced, dict(ENV, BOTHNER_TRACE_OUT=out))}
    best = {k: float("inf") for k in runs}
    for _ in range(REPS):
        for k, (argv, env) in runs.items():
            best[k] = min(best[k], wall(argv, cwd, env))
    os.remove(out)
    p = best["plain"]
    total.append(best["traced"] / p - 1)
    span.append((best["traced"] - best["loaded"]) / p)
    print(f"{name:18} {p * 1e3:9.2f} {best['loaded'] * 1e3:8.2f} {best['traced'] * 1e3:8.2f} "
          f"{total[-1] * 100:6.2f}% {span[-1] * 100:7.2f}% {(best['output'] - best['traced']) * 1e3:9.2f}")
for what, v in (("total", total), ("in span", span)):
    v = sorted(v)
    print(f"{what}: median {v[len(v) // 2] * 100:.2f}%, max {v[-1] * 100:.2f}%")
