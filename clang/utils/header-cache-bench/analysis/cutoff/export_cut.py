"""Folding threshold study: the dashboard's tables from the cutoff runs.
git and Redis: u31cut (run 1, 0-2000 us) and u31cut2 (run 2, 226-50000 us),
bin-u31. CPython and FFmpeg: u32cut (run 1, 0-50000 us), bin-u32, the same
code when merging is off. Every mode is linkc with -ask-cutoff-us <c>,
LEVEL=2 and a line per hit.
usage: export_cut.py <results-2026-11-01 dir> <out dir>"""
import csv, json, os, re, statistics, sys, collections

R, O = sys.argv[1], sys.argv[2]
RUNS = {"git": [(1, "u31cut"), (2, "u31cut2")], "redis": [(1, "u31cut"), (2, "u31cut2")],
        "cpython": [(1, "u32cut")], "ffmpeg": [(1, "u32cut")]}
HIT = re.compile(r"header cache: (\d+) hits / (\d+) lookups")
EXP = re.compile(r"header cache: exported (\d+) parts")
LNK = re.compile(r"header cache hit: (\S+) links=(\d+) combo=([0-9A-F]+) instr=(\d+) us=([\d.]+) bytes=(\d+) link_bytes=(\d+)")
TXT = re.compile(r"header cache text time: ([\d.]+) ms reading (\d+) outermost")
CHK = re.compile(r"header cache time: ([\d.]+) ms in lookups")
WR = re.compile(r"declined \d+, ([\d.]+) ms writing and pushing")
LINE = re.compile(r"^  text (\S+) (\d+) depth (\d+) own (\d+) why (\S+)", re.M)
CUTS = [0, 100, 226, 500, 1000, 2000, 5000, 10000, 20000, 50000]
BANDS = list(zip(CUTS, CUTS[1:] + [None]))
LB = [(0, 0), (1, 1), (2, 3), (4, 7), (8, 15), (16, 31), (32, 63), (64, 127), (128, 10 ** 9)]

sweep, steps, bands, links = [], [], [], []
for p, runs in RUNS.items():
    pts = {}
    lk = collections.defaultdict(list)
    for run, d in runs:
        f = f"{R}/{d}/cut-{p}.json"
        if not os.path.exists(f):
            continue
        j = json.load(open(f))
        plain, plain_t = sum(j["I"]["plain"]), sum(j["T"]["plain"])
        for m in j["modes"][1:]:
            c = int(m[7:])
            tot, tt = sum(j["I"][m]), sum(j["T"][m])
            h = l = e = tr = 0; hg = txt = chk = wr = 0.0; nl = []
            for s in j["S"][m]:
                if x := HIT.search(s): h += int(x[1]); l += int(x[2])
                if x := EXP.search(s): e += int(x[1])
                if x := TXT.search(s): txt += float(x[1]); tr += int(x[2])
                if x := CHK.search(s): chk += float(x[1])
                if x := WR.search(s): wr += float(x[1])
                for g in LNK.finditer(s):
                    hg += int(g[4]); nl.append(int(g[2]))
                    lk[int(g[2])].append((int(g[4]), int(g[6]) + int(g[7])))
            row = dict(project=p, run=run, cutoff_us=c, speedup=round(plain / tot, 4), wall_speedup=round(plain_t / tt, 3),
                       total_g=round(tot / 1e9, 2), hits_g=round(hg / 1e9, 2), other_g=round((tot - hg) / 1e9, 2),
                       hits_pct=round(100 * hg / plain, 2), other_pct=round(100 * (tot - hg) / plain, 2),
                       lookups=l, hits=h, misses=l - h, miss_pct=round(100 * (l - h) / l, 1) if l else 0, written=e,
                       links_per_hit=round(statistics.mean(nl), 2) if nl else 0, text_s=round(txt / 1e3, 2),
                       lookup_s=round(chk / 1e3, 2), write_s=round(wr / 1e3, 2), direct_text_reads=tr)
            sweep.append(row); pts[(run, c)] = row
        # Headers by first read time, from the run's highest cutoff, where a
        # compile asks for almost nothing: each header's first read is the
        # time the daemon lists it by; reads a .c file makes are depth 1.
        if run == runs[-1][0]:
            m = "linkcut50000"
            first, direct = {}, collections.defaultdict(list)
            for s in j["S"][m]:
                for g in LINE.finditer(s):
                    first.setdefault(g[1], int(g[2]))
                    if g[3] == "1":
                        direct[g[1]].append(int(g[2]))
            for lo, hi in BANDS:
                hs = [k for k, t in first.items() if t >= lo and (hi is None or t < hi)]
                dr = [t for k in hs for t in direct[k]]
                bands.append(dict(project=p, band=f"{lo}-{hi}" if hi else f"{lo}+", lo_us=lo, hi_us=hi or "",
                                  headers=len(hs), direct_reads=len(dr), direct_s=round(sum(dr) / 1e6, 2),
                                  direct_pct=round(100 * (sum(dr) / 1e3) / plain_t, 2)))
    # Consecutive cutoffs within one run; run 1 up to 2000 us, the last run beyond.
    for a, b in zip(CUTS, CUTS[1:]):
        run = 1 if b <= 2000 or len(runs) == 1 else 2
        if (run, a) in pts and (run, b) in pts:
            x, y = pts[(run, a)], pts[(run, b)]
            steps.append(dict(project=p, run=run, from_us=a, to_us=b,
                              d_hits_pct=round(y["hits_pct"] - x["hits_pct"], 2),
                              d_other_pct=round(y["other_pct"] - x["other_pct"], 2),
                              d_total_pct=round(y["hits_pct"] + y["other_pct"] - x["hits_pct"] - x["other_pct"], 2)))
    for lo, hi in LB:
        hs = [v for k, vs in lk.items() if lo <= k <= hi for v in vs]
        if hs:
            links.append(dict(project=p, links=f"{lo}" if lo == hi else (f"{lo}-{hi}" if hi < 10 ** 9 else f"{lo}+"),
                              links_lo=lo, hits=len(hs), instr_per_hit_m=round(statistics.mean(v[0] for v in hs) / 1e6, 2),
                              kb_per_hit=round(statistics.mean(v[1] for v in hs) / 1024, 1)))

os.makedirs(O, exist_ok=True)
for name, rows in (("sweep", sweep), ("steps", steps), ("bands", bands), ("links", links)):
    with open(f"{O}/{name}.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)
    print(name, len(rows), "rows")
