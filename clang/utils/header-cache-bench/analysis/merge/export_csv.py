"""Writes the merge study's tables for the dashboard, from
results-2026-11-01/u30 (sweep-, sweep2-<project>.json; permerged.json).
usage: export_csv.py <u30 dir> <out dir>"""
import csv, json, re, statistics, sys, collections

D, O = sys.argv[1], sys.argv[2]
P = ["git", "cpython", "redis"]
MER = re.compile(r"header cache merge: (\S+) (.*?) combo=([0-9A-F]+) merge_instr=(\d+)")
HIT = re.compile(r"header cache hit: (\S+) links=(\d+) combo=([0-9A-F]+) instr=(\d+)")

# 1. Sweep: gain over the same run's linkc, per merge count.
with open(f"{O}/sweep.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["project", "run", "merge_count", "gain_pct", "speedup", "merges_g", "compiles_g", "total_g", "merges"])
    for p in P:
        for run in ("sweep", "sweep2"):
            j = json.load(open(f"{D}/{run}-{p}.json"))
            plain, base = sum(j["I"]["plain"]), sum(j["I"]["linkc"])
            for m in j["modes"][1:]:
                if m in ("linkc", "linkcount", "linkm2h"):
                    continue
                k = 2 if m == "linkm" else int(m[5:])
                tot = sum(j["I"][m])
                ms = [int(g[4]) for s in j["S"][m] for g in MER.finditer(s)]
                w.writerow([p, 1 if run == "sweep" else 2, k, round(100 * (base - tot) / base, 2),
                            round(plain / tot, 3), round(sum(ms) / 1e9, 2), round((tot - sum(ms)) / 1e9, 2),
                            round(tot / 1e9, 2), len(ms)])

# 2. Each merged copy at merge count 2: hits on it and its net saving.
pm = json.load(open(f"{O}/permerged.json"))
with open(f"{O}/merged.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["id", "project", "header", "hits", "links", "linked_m", "merged_m", "merge_m", "net_m"])
    n = 0
    for p in P:
        for r in pm[p]["rows"]:
            n += 1
            w.writerow([n, p, r["header"], r["hits"], round(r["L"]), round(r["linked"] / 1e6, 2),
                        round((r["merged"] or 0) / 1e6, 2), round(r["merge"] / 1e6, 1), round(r["net"] / 1e6, 1)])

# 3 and 4. Hits per combination, never merged (linkcount).
buckets = [(1, 1), (2, 2), (3, 4), (5, 8), (9, 16), (17, 32), (33, 64), (65, 128), (129, 10 ** 9)]
names = ["1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65-128", "129+"]
dist, later = [], []
for p in P:
    j = json.load(open(f"{D}/sweep-{p}.json"))
    by = collections.defaultdict(list)
    for s in j["S"]["linkcount"]:
        for g in HIT.finditer(s):
            if g[3] != "0":
                by[g[3]].append((int(g[2]), int(g[4])))
    N = {c: len(v) for c, v in by.items()}
    instr = {c: sum(x[1] for x in v) for c, v in by.items()}
    tot = sum(instr.values())
    for (lo, hi), nm in zip(buckets, names):
        cs = [c for c in N if lo <= N[c] <= hi]
        dist.append([p, nm, len(cs), sum(N[c] for c in cs), round(100 * sum(instr[c] for c in cs) / tot, 1),
                     round(statistics.mean(statistics.mean(x[0] for x in by[c]) for c in cs), 1) if cs else 0])
    for k in (1, 2, 4, 8, 16, 32, 64):
        r = [N[c] - k for c in N if N[c] >= k]
        later.append([p, k, len(r), round(statistics.mean(r), 1) if r else 0, statistics.median(r) if r else 0])
with open(f"{O}/dist.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["project", "hits_per_combination", "combinations", "hits", "share_of_linked_instr_pct", "links_per_hit"])
    w.writerows(dist)
with open(f"{O}/later.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["project", "k", "combinations_reaching_k", "mean_later_hits", "median_later_hits"])
    w.writerows(later)
print("written")
