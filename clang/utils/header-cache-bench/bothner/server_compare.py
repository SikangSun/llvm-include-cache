"""Step S4 of COMPILE_SERVER_DESIGN.md: the real server's decisions against
the model's, compile by compile and pass by pass.

usage: server_compare.py <server_model.py's JSON> <model run label> <server results.jsonl> [out.json]
The server's results must come from the same compiles with the same flags
as the records: server_objects.py's <project>/res0.jsonl (the build's own
flags, one chunk). server_s1.py compiles at -O0, which changes the headers
(glibc's bits/stdio.h inlines need __OPTIMIZE__). The model's passes beyond
the server's are left out. The label is one of server_model.py's runs
("S3G* tok" for option 3 with groups, "S2 tok" for option 2).

The two count fragments differently. The server counts every stretch between
two directives, most of them blank ("empty"). The model's records hold the
stretches with source text, and its "empty" ones hand the parser no tokens
(__BEGIN_DECLS in C), which the server reuses. So "empty" is left out, and
the model's empty count is shown beside the server's surplus of reuses.
"""
import collections, json, os, sys

model_json, label, results = sys.argv[1:4]
out = sys.argv[4] if len(sys.argv) > 4 else None
m = json.load(open(model_json))
model = m[label]
compiles = m["compiles"]
n = len(compiles)
rows = [json.loads(l) for l in open(results)]
model = model[:len(rows) // n]
assert len(rows) == n * len(model), (len(rows), n, len(model))
server = [[None] * n for _ in model]
for r in rows:
    server[r["i"] // n][r["i"] % n] = r

ALIAS = {"token names": "uses"}


def norm(d):
    c = collections.Counter()
    for k, v in d.items():
        if k != "empty":
            c[ALIAS.get(k, k)] += v
    return c


report = []
for p, res in enumerate(model):
    tm, ts = collections.Counter(), collections.Counter()
    memp = 0
    same_all = same_noempty = 0
    diffs = []
    for k, (x, c) in enumerate(zip(res, compiles)):
        assert x["i"] == c["i"]
        s = server[p][k]
        md, sd = norm(x["why"]), norm(s["decisions"])
        e = x["why"].get("empty", 0)
        memp += e
        tm.update(md)
        ts.update(sd)
        # The model's empty units are reuses in the server.
        md2 = collections.Counter(md)
        md2["reused"] += e
        same_all += md2 == sd
        if md2 != sd:
            keys = sorted(set(md2) | set(sd))
            diffs.append({"i": c["i"], "src": os.path.basename(c["src"]),
                          "counts": {y: (md2[y], sd[y]) for y in keys if md2[y] != sd[y]}})
    keys = sorted(set(tm) | set(ts), key=lambda y: -(tm[y] + ts[y]))
    print(f"pass {p}: compiles with every count the same (the model's empty counted as reused) {same_all} of {n}")
    print(f"  reason          model   server   (model empty, not counted: {memp})")
    for y in keys:
        print(f"    {y:12} {tm[y]:8} {ts[y]:8}" + ("" if tm[y] == ts[y] else f"   {ts[y] - tm[y]:+}"))
    diffs.sort(key=lambda d: -sum(abs(a - b) for a, b in d["counts"].values()))
    for d in diffs[:10]:
        print(f"  compile {d['i']} {d['src']}: " + ", ".join(f"{y} {a}/{b}" for y, (a, b) in d["counts"].items()))
    report.append({"same": same_all, "model": tm, "server": ts, "model_empty": memp, "diffs": diffs})
if out:
    json.dump(report, open(out, "w"), indent=1)
