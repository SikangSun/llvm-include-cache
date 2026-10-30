"""Bothner's compile server replayed over a recorded build
(COMPILE_SERVER_PLAN.md 3.3). Version 0, the plan's step D2: no dependency
sets yet.

The build's compiles run in log order twice: a first build fills the server,
then the recompilation is scored. The server holds one stored version per
fragment location (configuration, file as opened, offset), the latest parse
(p.26). A fragment instance is reused when all of these hold:
  - the parser was between declarations where it starts and where it ends
    (p.33, currently_nested);
  - this compile has not processed the location already (p.28; under R1 a
    repeat is parsed, and an identical re-parse keeps the stored version);
  - the stored version's token hash equals this instance's: the parser would
    receive the same tokens, so every macro the fragment uses expands as it
    did (the per-fragment macro checks, D3 of the plan, and the oracle).
Anything else is parsed, and becomes the stored version.

Leaving out the dependency sets (p.27-28) can only add reuse, so v0 is an
upper bound on R1's.

Time: plain clang's front end for each compile, less the measured time of its
reused fragments, at no cost for restoring them. A persistent server also
skips initializing the compiler (p.24's "initially" level), but its driver
still runs for every request (p.22-23): "cc1 credit" takes off CREDIT_MS per
compile, measured as a one-line file's compile less the driver alone (`-###`)
with redis's flags, 1.57-1.68 ms. "Generous credit" takes off everything
outside the compile's own processing instead (the whole process start and
the predefines).
usage: simulate.py <record dir> <timing dir> [recompile json]
env: CREDIT_MS (default 1.65)
The recompile JSON (recompile.py) supplies plain clang's and the cache's
recompilation times; without it, plain clang with the modules flags from the
timing run is used.
"""
import collections, json, os, statistics, sys

rec_dir, time_dir = sys.argv[1], sys.argv[2]
recomp = json.load(open(sys.argv[3])) if len(sys.argv) > 3 else None

builds = []  # in log order
for f in sorted(os.listdir(rec_dir)):
    if not f.endswith(".json"):
        continue
    r, t = json.load(open(os.path.join(rec_dir, f))), json.load(open(os.path.join(time_dir, f)))
    if not r["data"] or not t["data"]:
        continue
    rd, td = r["data"], t["data"]
    frags = []
    for a, b in zip(rd["frags"], td["frags"]):
        name = rd["entries"][a[0]][0]
        assert (name, a[1]) == (td["entries"][b[0]][0], b[1]), f
        frags.append({"loc": (rd["config"], name, a[1]), "hash": a[7], "nested": not (a[8] == 1 and a[9] == 1),
                      "ns": b[5], "sys": rd["entries"][a[0]][2]})
    assert len(rd["frags"]) == len(td["frags"]), f
    builds.append({"i": r["meta"]["i"], "src": r["meta"]["src"], "cwd": r["meta"]["cwd"], "frags": frags,
                   "span": td["span_ns"], "pre": td["own_ns"]["pre"],
                   "plainM": t["meta"].get("plain_wall", 0) * 1e9})


def run(store, score):
    out = []
    for b in builds:
        seen, why, saved = set(), collections.Counter(), 0
        for x in b["frags"]:
            loc = x["loc"]
            if x["nested"]:
                why["nested"] += 1
            elif loc in seen:
                why["repeat"] += 1
                store[loc] = x["hash"]  # R1: identical re-parse keeps it; else the latest
            elif store.get(loc) == x["hash"]:
                why["reused"] += 1
                saved += x["ns"]
            else:
                why["new" if loc not in store else "changed"] += 1
                store[loc] = x["hash"]
            seen.add(loc)
        if score:
            out.append({"b": b, "why": why, "saved": saved})
    return out


store = {}
run(store, False)          # first build: fills the server
res = run(store, True)     # recompilation

why = collections.Counter()
for r in res:
    why.update(r["why"])
n = sum(why.values())
print(f"{len(res)} compiles, {n} fragment instances in the recompilation: " +
      ", ".join(f"{k} {v} ({v / n * 100:.1f}%)" for k, v in why.most_common()))
tot = sum(x["ns"] for r in res for x in r["b"]["frags"])
sav = sum(r["saved"] for r in res)
print(f"time in fragments {tot / 1e9:.2f} s, in reused ones {sav / 1e9:.2f} s ({sav / tot * 100:.1f}%)")

# Per compile times (seconds).
by_i = {str(r["b"]["i"]): r for r in res}
rows = []
for r in res:
    b = r["b"]
    if recomp and str(b["i"]) in recomp["compiles"]:
        c = recomp["compiles"][str(b["i"])]
        plain, cache = c["plain"], c.get("cache")
    else:
        plain, cache = b["plainM"] / 1e9, None
    cc1 = float(os.environ.get("CREDIT_MS", "1.65")) / 1e3
    generous = max(0.0, plain - b["span"] / 1e9) + b["pre"] / 1e9
    every = sum(x["ns"] for x in b["frags"]) / 1e9
    saved = r["saved"] / 1e9
    rows.append({"src": b["src"], "cwd": b["cwd"], "plain": plain, "cache": cache,
                 "bound": plain - saved, "bound_cc1": plain - saved - cc1, "bound_generous": plain - saved - generous,
                 "ceiling": plain - every, "ceiling_cc1": plain - every - cc1,
                 "ceiling_generous": plain - every - generous})


def table(label, rs):
    if not rs:
        return
    P = sum(r["plain"] for r in rs)
    print(f"\n{label}: {len(rs)} compiles; plain clang {P:.2f} s")
    cols = [("Bothner bound, R1 v0", "bound"), ("  with cc1 credit", "bound_cc1"),
            ("  with generous credit", "bound_generous"), ("every fragment reused (ceiling)", "ceiling"),
            ("  with cc1 credit", "ceiling_cc1"), ("  with generous credit", "ceiling_generous")]
    if all(r["cache"] is not None for r in rs):
        cols.insert(0, ("header cache, recompilation", "cache"))
    for name, k in cols:
        T = sum(r[k] for r in rs)
        g = statistics.geometric_mean([r["plain"] / r[k] for r in rs])
        print(f"  {name:34} {T:7.2f} s  {P / T:5.2f}x in total, {g:5.2f}x geometric mean per compile")


table("all", rows)
table("redis-server (src/)", [r for r in rows if "/redis/src" in r["cwd"]])
