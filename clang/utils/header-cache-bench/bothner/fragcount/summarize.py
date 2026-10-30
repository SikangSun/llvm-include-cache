"""Summarizes the fragcount outputs. usage: summarize.py <outdir> [csv]"""
import json, os, sys, collections
out = sys.argv[1]
rows, hdr, failed, bad = [], collections.defaultdict(lambda: collections.Counter()), [], 0
hdr_cc = collections.Counter()
locs2, locs3 = collections.defaultdict(set), collections.defaultdict(set)
kinds_tot = collections.Counter(); kinds_sys = collections.Counter(); kinds_proj = collections.Counter()
for n in sorted(os.listdir(out)):
    d = json.load(open(os.path.join(out, n)))
    m, x = d["meta"], d["data"]
    if m["rc"] != 0 or x is None:
        failed.append((m["i"], m["src"], m["rc"])); continue
    bad += x["bad_skips"]
    s, p, mn = x["sys"], x["proj"], x["main"]
    k = collections.Counter(s["by_kind"]) + collections.Counter(p["by_kind"])
    kinds_tot.update(k); kinds_sys.update(s["by_kind"]); kinds_proj.update(p["by_kind"])
    r = {"i": m["i"], "cwd": m["cwd"], "src": m["src"],
         "entries": s["entries"] + p["entries"], "entries_sys": s["entries"], "entries_proj": p["entries"],
         "distinct": x["distinct"],
         "distinct_sys": sum(1 for h in x["headers"] if h["sys"]), "distinct_proj": sum(1 for h in x["headers"] if not h["sys"]),
         "dirs": s["dirs"] + p["dirs"], "dirs_sys": s["dirs"], "dirs_proj": p["dirs"],
         "skipped_dirs": s["skipped_dirs"] + p["skipped_dirs"],
         "boundary_dirs": s.get("boundary", 0) + p.get("boundary", 0),
         "dirs_literal": s["dirs"] + p["dirs"] + s.get("boundary", 0) + p.get("boundary", 0),
         "frag2": s["frag2"] + p["frag2"], "frag2_sys": s["frag2"], "frag2_proj": p["frag2"],
         "frag3": s["frag3"] + p["frag3"], "frag3_sys": s["frag3"], "frag3_proj": p["frag3"],
         "frag_tok": s["frag_tok"] + p["frag_tok"], "frag_tok_sys": s["frag_tok"], "frag_tok_proj": p["frag_tok"],
         "hdr_tok": s["frag_tok"] + s["dir_tok"] + p["frag_tok"] + p["dir_tok"],
         "hdr_tok_sys": s["frag_tok"] + s["dir_tok"], "hdr_tok_proj": p["frag_tok"] + p["dir_tok"],
         "hdr_dir_tok": s["dir_tok"] + p["dir_tok"],
         "hdr_skip_tok": s["skip_tok"] + p["skip_tok"],
         "main_tok": mn["frag_tok"] + mn["dir_tok"], "main_frag_tok": mn["frag_tok"], "main_dirs": mn["dirs"],
         "main_frag2": mn["frag2"],
         "open_depth2": s["open_depth2"] + p["open_depth2"], "open_tail2": s["open_tail2"] + p["open_tail2"],
         "open_depth3": s["open_depth3"] + p["open_depth3"], "open_tail3": s["open_tail3"] + p["open_tail3"]}
    for kk in ("include", "include_next", "define", "undef", "if", "ifdef", "ifndef", "elif", "else", "endif", "pragma", "null"):
        r["d_" + kk] = k.get(kk, 0)
    r["d_other"] = sum(v for kk, v in k.items() if "d_" + kk not in r)
    has_locs = all("s2" in h for h in x["headers"])
    if has_locs:
        r["frag2_distinct_loc"] = sum(len(h["s2"]) for h in x["headers"])
        r["frag3_distinct_loc"] = sum(len(h["s3"]) for h in x["headers"])
        r["frag2_repeat_in_compile"] = r["frag2"] - r["frag2_distinct_loc"]
        r["frag3_repeat_in_compile"] = r["frag3"] - r["frag3_distinct_loc"]
        for h in x["headers"]:
            locs2[h["path"]].update(h["s2"]); locs3[h["path"]].update(h["s3"])
    r["share_frag_of_all"] = r["frag_tok"] / (r["hdr_tok"] + r["main_tok"])
    r["share_frag_of_nondir"] = r["frag_tok"] / (r["frag_tok"] + r["main_frag_tok"])
    r["tok_per_frag2"] = r["frag_tok"] / r["frag2"] if r["frag2"] else 0
    rows.append(r)
    for h in x["headers"]:
        c = hdr[(h["path"], h["sys"])]
        hc = h["c"]
        for kk in ("entries", "frag2", "frag3", "frag_tok", "dir_tok", "dirs", "open_depth2", "open_tail2"):
            c[kk] += hc[kk]
        hdr_cc[(h["path"], h["sys"])] += 1

def q(v, p):
    v = sorted(v); x = (len(v) - 1) * p; lo = int(x); hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (x - lo)

cols = [c for c in rows[0] if c not in ("i", "cwd", "src")]
print(f"compiles summarized: {len(rows)}; failed/excluded: {failed}; skip ranges crossing files: {bad}")
print(f"{'metric':22s} {'median':>10s} {'p90':>10s} {'max':>10s} {'min':>9s} {'mean':>10s} {'total':>11s}")
for c in cols:
    v = [r[c] for r in rows]
    tot = sum(v) if not c.startswith(("share", "tok_per")) else float("nan")
    fmt = (lambda z: f"{z:10.3f}") if isinstance(v[0], float) else (lambda z: f"{z:10.1f}" if z != int(z) else f"{int(z):10d}")
    print(f"{c:22s} {fmt(q(v,.5))} {fmt(q(v,.9))} {fmt(max(v))} {fmt(min(v))[1:]} {sum(v)/len(v):10.1f} {tot:11.0f}")
T = lambda c: sum(r[c] for r in rows)
print(f"pooled share of all compiled tokens (hdr processed + main processed) in header fragments: {T('frag_tok')/(T('hdr_tok')+T('main_tok')):.4f}")
print(f"pooled share of non-directive tokens in header fragments: {T('frag_tok')/(T('frag_tok')+T('main_frag_tok')):.4f}")
print(f"pooled option-2 fragments ending inside brackets: {T('open_depth2')/T('frag2'):.4f}, ending at depth 0 on a token other than ; or }}: {T('open_tail2')/T('frag2'):.4f}")
print(f"pooled option-3 fragments ending inside brackets: {T('open_depth3')/T('frag3'):.4f}, ending at depth 0 on a token other than ; or }}: {T('open_tail3')/T('frag3'):.4f}")
print(f"pooled tokens per option-2 fragment: {T('frag_tok')/T('frag2'):.2f}; per option-3 fragment: {T('frag_tok')/T('frag3'):.2f}")
print("header directive kinds, totals over all compiles:", dict(kinds_tot.most_common()))
print("  system headers: ", dict(kinds_sys.most_common()))
print("  project headers:", dict(kinds_proj.most_common()))
# groups by sub-project
grp = collections.defaultdict(list)
for r in rows:
    rel = os.path.relpath(r["cwd"], "/home/sikang/local_IncludeCache/bench/redis")
    grp[rel.split("/")[0] if not rel.startswith("deps") else "/".join(rel.split("/")[:2])].append(r)
print("\nby working directory group: n, median frag2, p90 frag2, max frag2, median frag3, median entries, median share_frag_of_all")
for g, rs in sorted(grp.items(), key=lambda kv: -len(kv[1])):
    f = lambda c, p: q([r[c] for r in rs], p)
    print(f"  {g:22s} n={len(rs):3d} frag2 med={f('frag2',.5):7.0f} p90={f('frag2',.9):7.0f} max={max(r['frag2'] for r in rs):6d}  frag3 med={f('frag3',.5):6.0f}  entries med={f('entries',.5):5.0f}  share={f('share_frag_of_all',.5):.3f}")
print("\ntop 10 headers by option-2 fragments summed over all compiles (sys, compiles, entries, frag2, frag3, frag tokens, frag2/entry)")
for (path, sysf), c in sorted(hdr.items(), key=lambda kv: -kv[1]["frag2"])[:10]:
    print(f"  {path}  sys={sysf} compiles={hdr_cc[(path,sysf)]} entries={c['entries']} frag2={c['frag2']} frag3={c['frag3']} frag_tok={c['frag_tok']} open_depth2={c['open_depth2']} open_tail2={c['open_tail2']} frag2/entry={c['frag2']/c['entries']:.1f} distinct_locs2={len(locs2.get(path, ()))}")
print("\ntop 10 headers by option-3 fragments")
for (path, sysf), c in sorted(hdr.items(), key=lambda kv: -kv[1]["frag3"])[:10]:
    print(f"  {path}  sys={sysf} compiles={hdr_cc[(path,sysf)]} entries={c['entries']} frag2={c['frag2']} frag3={c['frag3']} frag3/entry={c['frag3']/c['entries']:.1f}")
print(f"\ndistinct header files over all compiles: {len(hdr)} (sys {sum(1 for k in hdr if k[1])}, proj {sum(1 for k in hdr if not k[1])})")
if locs2:
    print(f"stored fragments over the whole build (distinct header file + start offset): option 2: {sum(map(len, locs2.values()))}, option 3: {sum(map(len, locs3.values()))}")
    for sysf in (1, 0):
        ps = {k[0] for k in hdr if k[1] == sysf}
        print(f"  {'sys ' if sysf else 'proj'}: option 2: {sum(len(locs2[p]) for p in ps)}, option 3: {sum(len(locs3[p]) for p in ps)}")
if len(sys.argv) > 2:
    import csv
    with open(sys.argv[2], "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
