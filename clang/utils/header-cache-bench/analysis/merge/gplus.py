"""guard+ runs (bin-u30, results-2026-11-01/u30/gplus-<project>.json): per
project and mode, instructions and wall time against plain clang, hits,
guard+ hits, failures; geometric means; objects differing in the full build.
usage: gplus.py <u30 dir>"""
import json, math, re, sys

D = sys.argv[1]
rows = {}
for p in ["git", "cpython", "redis", "ffmpeg"]:
    try:
        j = json.load(open(f"{D}/gplus-{p}.json"))
    except FileNotFoundError:
        continue
    I, T = j["I"], j["T"]
    plain_i, plain_t = sum(I["plain"]), sum(T["plain"])
    for m in j["modes"][1:]:
        h = l = g = gh = 0
        for s in j["S"][m]:
            if x := re.search(r"header cache: (\d+) hits / (\d+) lookups", s):
                h += int(x[1]); l += int(x[2])
            if x := re.search(r"header cache guard\+: (\d+) hits on copies holding headers read already \((\d+) such", s):
                g += int(x[1]); gh += int(x[2])
        rows[(p, m)] = dict(G=sum(I[m]) / 1e9, xi=plain_i / sum(I[m]), xt=plain_t / sum(T[m]),
                            hits=h, lookups=l, gplus=g, held=gh,
                            failed=sum(1 for f in j["failed"] if f[0] == m), plain=plain_i / 1e9)
modes = sorted({m for _, m in rows}, key=["counter", "counterg", "linkc", "linkcg"].index)
print("| Project | Mode | Instructions (G) | Speedup, instructions | Speedup, wall | Hits / lookups | guard+ hits (headers held again) | Failed |")
print("|---|---|---|---|---|---|---|---|")
for (p, m), r in rows.items():
    print(f"| {p} | {m} | {r['G']:.1f} | {r['xi']:.3f}x | {r['xt']:.2f}x | {r['hits']} / {r['lookups']} | "
          f"{r['gplus']} ({r['held']}) | {r['failed']} |")
for m in modes:
    xs = [r for (p, mm), r in rows.items() if mm == m]
    if xs:
        gi = math.exp(sum(math.log(r["xi"]) for r in xs) / len(xs))
        gt = math.exp(sum(math.log(r["xt"]) for r in xs) / len(xs))
        print(f"geomean {m} ({len(xs)} projects): instructions {gi:.3f}x, wall {gt:.2f}x")
try:
    j = json.load(open(f"{D}/gplus-full-redis.json"))
    print("full Redis build, objects differing from plain clang's:",
          {m: len(v) for m, v in j["differ"].items()}, "failed:", len(j["failed"]))
except FileNotFoundError:
    pass
