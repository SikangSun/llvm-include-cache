"""Merge count sweep, both runs (sweep-, sweep2-<project>.json): per merge
count, instructions with merges included, the merges' own, the compiles'
alone, and the speedup over plain clang. A run's own linkc is its baseline.
usage: breakdown.py <u30 dir> [projects...]"""
import json, re, sys

D = sys.argv[1]
MER = re.compile(r"header cache merge: (\S+) (.*?) combo=([0-9A-F]+) merge_instr=(\d+)")
rows = {}
for p in sys.argv[2:] or ["git", "redis", "cpython"]:
    for run in ("sweep", "sweep2"):
        try:
            j = json.load(open(f"{D}/{run}-{p}.json"))
        except FileNotFoundError:
            continue
        plain = sum(j["I"]["plain"])
        base = sum(j["I"]["linkc"])
        for m in j["modes"][1:]:
            if m in ("linkcount", "linkm2h"):
                continue
            k = 0 if m == "linkc" else 2 if m == "linkm" else int(m[5:])
            tot = sum(j["I"][m])
            mi = sum(int(g[4]) for s in j["S"][m] for g in MER.finditer(s))
            n = sum(1 for s in j["S"][m] for g in MER.finditer(s))
            rows.setdefault(p, []).append((run, k, tot / 1e9, mi / 1e9, (tot - mi) / 1e9, plain / tot,
                                           (base - tot) / base, n))
for p, rs in rows.items():
    print(f"## {p}")
    print("| Run | Merge count | Total (G) | Merges (G) | Compiles only (G) | Speedup | Gain over the run's linkc | Merges |")
    print("|---|---|---|---|---|---|---|---|")
    for r in rs:
        print(f"| {r[0]} | {'never' if r[1] == 0 else r[1]} | {r[2]:.1f} | {r[3]:.1f} | {r[4]:.1f} | {r[5]:.3f}x | "
              f"{100 * r[6]:+.1f}% | {r[7]} |")
