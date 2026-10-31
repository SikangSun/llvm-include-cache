"""Measured misses of top-level folding (modes4, the real runs), by cause:
no copy of the header yet (nothing to check), every copy rejected on an
include guard (the first difference each rejected copy showed), or rejected
on something else. Also rejections, to set beside misses.
usage: GUARDRULE=anywhere measured_misses.py"""
import collections, json, re, sys
sys.path.insert(0, ".")
from ddg_classes import file_info
R = "/home/sikang/c/code/scratch/e2e/multi/results-2026-10-21/modes4"
Q = re.compile(r"^  query (\S+) (hit|miss) total=\d+ ask=\d+ check=\d+ checked=(\d+)")
J = re.compile(r"^    (\S+): (\S+) was (not )?defined, is")
X = re.compile(r"^    (\S+): ")
for p in ["postgres", "git", "cpython", "curl", "redis", "nginx"]:
    j = json.load(open(f"{R}/{p}.json"))
    c = collections.Counter(); guards = {}
    def is_guard(name):
        if name not in guards:
            guards[name] = False
        return guards[name]
    import os
    D = f"/home/sikang/c/code/scratch/e2e/multi/results-2026-10-22/ddg3/{p}"; files = set()
    for fn in os.listdir(D):
        if fn.endswith(".ddg"):
            for l in open(f"{D}/{fn}", errors="replace"):
                if l[0] == "R": files.add(l.split("\t", 2)[1])
    G = {g for g in (file_info(f)[0] for f in files) if g}
    G |= {g.replace("bin-ddg3", "bin-linkfix") for g in G}
    for e in j["S"]["top"]:
        rej = collections.defaultdict(list)
        lines = e.splitlines()
        for l in lines:
            m = J.match(l)
            if m: rej[m.group(1)].append("guard" if m.group(2) in G else "macro")
            elif X.match(l) and not l.startswith("    rejected"):
                t = l.split(": ", 1)[1]
                rej[X.match(l).group(1)].append("path" if ("opened as" in t or "finds" in t or "file" in t) else
                                                 "declaration" if ("declared" in t or "declaration" in t or "type" in t) else "other")
        for l in lines:
            m = Q.match(l)
            if not m: continue
            h, kind, n = m.group(1), m.group(2), int(m.group(3))
            c["requests asked"] += 1; c["rejections"] += n if kind == "miss" else max(0, n - 1)
            if kind == "hit":
                c["hit"] += 1; c["rejections on hits"] += max(0, n - 1); continue
            if n == 0: c["miss: no copy of the header yet"] += 1; continue
            rs = rej.get(h, [])
            if rs and all(x == "guard" for x in rs): c["miss: every copy rejected on a guard"] += 1
            elif rs and all(x in ("guard", "macro") for x in rs): c["miss: rejected on a macro (some guards)"] += 1
            elif "path" in rs and all(x in ("guard", "macro", "path") for x in rs): c["miss: path"] += 1
            elif "declaration" in rs and all(x in ("guard", "macro", "path", "declaration") for x in rs): c["miss: declaration"] += 1
            else: c["miss: rejected on something else"] += 1
            for x in rs: c["rej " + x] += 1
    n = c["requests asked"]; misses = n - c["hit"]
    print(f"{p:9s} requests asked {n:6d} | hit {100*c['hit']/n:4.1f}% | misses {100*misses/n:4.1f}%: no copy yet {100*c['miss: no copy of the header yet']/n:4.1f}%, guard {100*c['miss: every copy rejected on a guard']/n:4.1f}%, other macro {100*c['miss: rejected on a macro (some guards)']/n:4.1f}%, path {100*c['miss: path']/n:4.1f}%, declaration {100*c['miss: declaration']/n:4.1f}%, other {100*c['miss: rejected on something else']/n:4.1f}% | rejections {c['rejections']} ({c['rejections']/max(1,misses):.1f} per miss, {100*c['rejections on hits']/max(1,c['rejections']):.0f}% on hits; on misses: guard {100*c['rej guard']/max(1,c['rej guard']+c['rej macro']+c['rej other']):.0f}%, macro {100*c['rej macro']/max(1,c['rej guard']+c['rej macro']+c['rej other']):.0f}%)")
