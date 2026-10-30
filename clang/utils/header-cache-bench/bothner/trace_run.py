"""Front-end time per header fragment for every recorded compile
(COMPILE_SERVER_PLAN.md 3.2, D1).

Each compile runs REPS times, serially, pinned to CORE, as plain clang's front
end with the cache's modules flags plus the BothnerTrace plugin; the run with
the shortest in-process span is kept. A compile already done is skipped, so a
run cut short starts again where it was.

Checks, per compile: the parts add up to the span; no token is outside every
fragment; the fragments are as many as fragcount/expected_per_compile.csv
says (FragCount on the same compiles).
usage: trace_run.py <outdir> [first index] [count]
env: REPS (default 3), CORE (default 3), LOGS, BIN
"""
import csv, json, os, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from compiles import ENV, commands, front_end

TRACE = os.path.join(HERE, "BothnerTrace.so")
REPS = int(os.environ.get("REPS", "3"))
CORE = os.environ.get("CORE", "3")
out = os.path.abspath(sys.argv[1])
first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
count = int(sys.argv[3]) if len(sys.argv) > 3 else None
os.makedirs(out, exist_ok=True)
expected = {int(r["i"]): int(r["frag2"]) for r in csv.DictReader(open(os.path.join(HERE, "fragcount", "expected_per_compile.csv")))}

cmds = commands()
todo = range(first, len(cmds) if count is None else min(len(cmds), first + count))
problems = []
for i in todo:
    path = os.path.join(out, f"{i:03d}.json")
    if os.path.exists(path):
        continue
    cwd, args, src = cmds[i]
    argv = ["taskset", "-c", CORE] + front_end(cwd, args, src, ["-fplugin=" + TRACE])
    tmp = path + ".tmp"
    runs, walls, rc, err = [], [], 0, ""
    for _ in range(REPS):
        s = time.perf_counter()
        r = subprocess.run(argv, cwd=cwd, env=dict(ENV, BOTHNER_TRACE_OUT=tmp), capture_output=True, text=True)
        walls.append(time.perf_counter() - s)
        rc, err = r.returncode, r.stderr[-2000:]
        if r.returncode or not os.path.exists(tmp):
            break
        runs.append(json.load(open(tmp)))
        os.remove(tmp)
    best = min(runs, key=lambda d: d["span_ns"]) if runs and not rc else None
    note = []
    if best:
        parts = sum(f[5] for f in best["frags"]) + sum(best["own_ns"].values())
        if abs(parts - best["span_ns"]) > max(1000, 1e-4 * best["span_ns"]):
            note.append(f"parts {parts} != span {best['span_ns']}")
        if best["loose_tokens"]:
            note.append(f"{best['loose_tokens']} tokens outside every fragment")
        if i in expected and expected[i] != len(best["frags"]):
            note.append(f"{len(best['frags'])} fragments, FragCount counted {expected[i]}")
    else:
        note.append(f"failed (rc {rc})")
    if note:
        problems.append((i, src, note))
    json.dump({"meta": {"i": i, "cwd": cwd, "src": src, "argv": argv, "rc": rc, "walls": walls,
                        "stderr_tail": err if rc else "", "notes": note},
               "data": best}, open(path, "w"))
    share = best["frag_ns"] / best["span_ns"] if best else 0
    print(f"{i:3d} {src:40} {min(walls) * 1e3:8.1f} ms  fragments {len(best['frags']) if best else '-':>5}"
          f"  in fragments {share * 100:5.1f}%  {'; '.join(note)}", flush=True)

# Summary over everything in the output directory.
rows = []
for f in sorted(os.listdir(out)):
    if f.endswith(".json"):
        d = json.load(open(os.path.join(out, f)))
        if d["data"]:
            rows.append(d)
def med(xs):
    return statistics.median(xs) if xs else float("nan")
def summary(label, rs):
    if not rs:
        return
    sp = [r["data"]["span_ns"] / 1e6 for r in rs]
    fr = [r["data"]["frag_ns"] / r["data"]["span_ns"] for r in rs]
    hd = [r["data"]["own_ns"]["hdr_dir"] / r["data"]["span_ns"] for r in rs]
    mn = [(r["data"]["own_ns"]["main"] + r["data"]["own_ns"]["main_dir"]) / r["data"]["span_ns"] for r in rs]
    ca = [r["data"]["calibrated_ns"] / r["data"]["span_ns"] for r in rs]
    print(f"{label}: {len(rs)} compiles; span median {med(sp):.1f} ms (total {sum(sp) / 1e3:.2f} s); "
          f"share of span in header fragments median {med(fr) * 100:.1f}%, header directives {med(hd) * 100:.1f}%, "
          f"main file {med(mn) * 100:.1f}%, estimated by calibration {med(ca) * 100:.1f}%")
summary("all", rows)
summary("src/", [r for r in rows if "/redis/src" in r["meta"]["cwd"]])
pooled_f = sum(r["data"]["frag_ns"] for r in rows)
pooled_s = sum(r["data"]["span_ns"] for r in rows)
print(f"pooled: {pooled_f / pooled_s * 100:.1f}% of all compiles' span is in header fragments")
print(f"{len(problems)} compiles with problems in this run")
for i, src, note in problems:
    print(f"  {i} {src}: {'; '.join(note)}")
