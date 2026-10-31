"""Merge count study (bin-u30, results-2026-11-01/u30/sweep-<project>.json).

1. Sweep: per merge count k, instructions with merges included (one core),
   speedup over plain clang, merges started.
2. Distribution (linkcount: combinations counted, never merged): hits per
   combination of copies, the instructions of each hit, links per hit.
3. Cost of a merged copy's hit (linkm2h): hits on the copies the merge
   lines name, by combination; each merge's instructions.
4. Predicted saving per merge count k from 2 and 3:
     sum over combinations with N >= k of
       (N - k) x (linked hit - merged hit) - merge - fork
   and the expected later hits of a combination that reached k hits,
   E[N - k | N >= k], from the distribution alone.
usage: mergestudy.py <u30 dir> [projects...]"""
import json, math, re, sys, collections, statistics

D = sys.argv[1]
projects = sys.argv[2:] or ["git", "redis", "cpython"]
HIT = re.compile(r"header cache hit: (\S+) links=(\d+) combo=([0-9A-F]+) instr=(\d+) us=(\d+) bytes=(\d+) link_bytes=(\d+)")
MERGE = re.compile(r"header cache merge: (\S+) sent (\S+) combo=([0-9A-F]+) merge_instr=(\d+)")
MERGE_ANY = re.compile(r"header cache merge: (\S+) (.*?) combo=([0-9A-F]+) merge_instr=(\d+)")


def hits(j, m):
    """Each hit: (compile index, module, links, combo, instr, bytes, link bytes)."""
    out = []
    for i, s in enumerate(j["S"][m]):
        for g in HIT.finditer(s):
            out.append((i, g[1], int(g[2]), g[3], int(g[4]), int(g[6]), int(g[7])))
    return out


def merges(j, m):
    out = []
    for i, s in enumerate(j["S"][m]):
        for g in MERGE_ANY.finditer(s):
            sent = g[2].startswith("sent ")
            out.append((i, g[1], g[2].split()[1] if sent else None, g[3], int(g[4]), sent))
    return out


for p in projects:
    try:
        j = json.load(open(f"{D}/sweep-{p}.json"))
    except FileNotFoundError:
        continue
    I = j["I"]
    plain = sum(I["plain"])
    print(f"## {p}: {len(j['cmds'])} compiles, plain {plain / 1e9:.1f} G")
    print("| Mode | Merge count | Instructions, merges included (G) | Speedup | Merges started | Merges stored |")
    print("|---|---|---|---|---|---|")
    for m in j["modes"][1:]:
        ms = merges(j, m)
        k = {"linkc": "never", "linkcount": "never (counted)", "linkm": "2", "linkm2h": "2 (logged)"}.get(m, m[5:])
        print(f"| {m} | {k} | {sum(I[m]) / 1e9:.1f} | {plain / sum(I[m]):.3f}x | {len(ms)} | {sum(1 for x in ms if x[5])} |")

    # 2. Distribution from linkcount.
    H = hits(j, "linkcount")
    byc = collections.defaultdict(list)
    for h in H:
        if h[3] != "0":
            byc[h[3]].append(h)
    N = {c: len(v) for c, v in byc.items()}
    L = {c: statistics.mean(h[2] for h in v) for c, v in byc.items()}
    cost = {c: statistics.mean(h[4] for h in v) for c, v in byc.items()}
    print(f"\nlinkcount: {len(H)} hits, {sum(N.values())} loaded links ({len(N)} combinations), "
          f"{len(H) - sum(N.values())} loaded none")
    buckets = [(1, 1), (2, 2), (3, 4), (5, 8), (9, 16), (17, 32), (33, 64), (65, 128), (129, 10**9)]
    print("| Hits per combination | Combinations | Hits | Share of linked hits | Mean links per hit | Mean instructions per hit (k) |")
    print("|---|---|---|---|---|---|")
    tot = sum(N.values())
    for lo, hi in buckets:
        cs = [c for c in N if lo <= N[c] <= hi]
        if not cs:
            continue
        nh = sum(N[c] for c in cs)
        print(f"| {lo}-{hi if hi < 10**9 else '...'} | {len(cs)} | {nh} | {100 * nh / tot:.1f}% | "
              f"{statistics.mean(L[c] for c in cs):.1f} | {statistics.mean(cost[c] for c in cs) / 1e3:.0f} |")

    # E[N - k | N >= k]: later hits expected of a combination with k hits.
    print("\n| k | Combinations reaching k | Mean later hits E[N-k | N>=k] | Median later hits |")
    print("|---|---|---|---|")
    for k in (1, 2, 3, 4, 6, 8, 12, 16, 32):
        r = [N[c] - k for c in N if N[c] >= k]
        if r:
            print(f"| {k} | {len(r)} | {statistics.mean(r):.1f} | {statistics.median(r):.0f} |")

    # 3. Merged hits (linkm2h).
    if "linkm2h" in j["modes"]:
        M = merges(j, "linkm2h")
        sent = {x[2]: x for x in M if x[5]}
        Hm = hits(j, "linkm2h")
        mh = collections.defaultdict(list)
        for h in Hm:
            if h[1] in sent:
                mh[sent[h[1]][3]].append(h)
        mcost = {c: statistics.mean(h[4] for h in v) for c, v in mh.items()}
        mbytes = {c: statistics.mean(h[5] for h in v) for c, v in mh.items()}
        minstr = {x[3]: x[4] for x in M if x[5]}
        print(f"\nlinkm2h: {len(M)} merges started, {len(sent)} stored; {sum(len(v) for v in mh.values())} hits on merged copies")
        both = [c for c in mcost if c in cost]
        if both:
            sav = [cost[c] - mcost[c] for c in both]
            print(f"combinations seen both ways: {len(both)}; per hit: linked {statistics.mean(cost[c] for c in both) / 1e3:.0f} k, "
                  f"merged {statistics.mean(mcost[c] for c in both) / 1e3:.0f} k, saving {statistics.mean(sav) / 1e3:.0f} k "
                  f"(median {statistics.median(sav) / 1e3:.0f} k) instructions")
            # Saving per hit against links: least squares through the origin.
            xs = [L[c] for c in both]
            print(f"saving per link per hit: {sum(s * x for s, x in zip(sav, xs)) / max(1, sum(x * x for x in xs)) / 1e3:.0f} k instructions")
        if minstr:
            print(f"merge instructions: mean {statistics.mean(minstr.values()) / 1e6:.1f} M, median {statistics.median(minstr.values()) / 1e6:.1f} M")
        # 4. Predicted net saving per merge count.
        fork = 5e6
        def mc(c):
            return minstr.get(c, 17.5e6 + 257 * mbytes.get(c, 0))
        print("\n| Merge count k | Combinations merged | Predicted saving (G) | Hits after the merge |")
        print("|---|---|---|---|")
        for k in (2, 3, 4, 5, 6, 8, 12, 16, 32):
            net = 0.0; n = 0; later = 0
            for c in N:
                if N[c] < k:
                    continue
                n += 1
                s = cost[c] - mcost[c] if c in mcost else None
                if s is None:
                    continue
                net += (N[c] - k) * s - mc(c) - fork
                later += N[c] - k
            print(f"| {k} | {n} | {net / 1e9:.2f} | {later} |")
    print()
