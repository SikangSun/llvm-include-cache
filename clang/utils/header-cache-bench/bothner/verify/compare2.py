"""Compare reimpl2.py's per-compile results with simulate.py's --server-out,
for every run label, pass, compile and field.
usage: compare2.py mine.json theirs.json [labels,comma,separated] [--show N]"""
import collections, json, sys

mine = json.load(open(sys.argv[1]))
theirs = json.load(open(sys.argv[2]))
show = 5
rest = sys.argv[3:]
if "--show" in rest:
    k = rest.index("--show")
    show = int(rest[k + 1])
    rest = rest[:k] + rest[k + 2:]
labels = rest[0].split(",") if rest else list(theirs)
FIELDS = ("i", "why", "why_ns", "saved_ns", "saved_def_ns", "restored", "unsafe2")
total_exact = total = 0
for v in labels:
    if v not in mine or v not in theirs:
        print(v, "missing: mine", v in mine, "theirs", v in theirs)
        continue
    shown = 0
    per_pass = []
    nexact_v = 0
    for p, (pm, pt) in enumerate(zip(mine[v], theirs[v])):
        assert len(pm) == len(pt), (v, p, len(pm), len(pt))
        bad_fields = collections.Counter()
        nexact = 0
        for cm, ct in zip(pm, pt):
            extra = set(cm) ^ set(ct)
            diff = [f for f in sorted(set(cm) | set(ct)) if cm.get(f) != ct.get(f)]
            if not diff and not extra:
                nexact += 1
                continue
            bad_fields.update(diff)
            if shown < show:
                shown += 1
                msg = []
                for f in diff:
                    if isinstance(cm.get(f), dict) or isinstance(ct.get(f), dict):
                        a, b = cm.get(f) or {}, ct.get(f) or {}
                        msg.append("%s %s" % (f, {k: (a.get(k, 0), b.get(k, 0)) for k in set(a) | set(b)
                                                  if a.get(k, 0) != b.get(k, 0)}))
                    else:
                        msg.append("%s %s/%s" % (f, cm.get(f), ct.get(f)))
                print("   %s pass %d compile %s (mine/theirs): %s" % (v, p, ct.get("i"), "; ".join(msg)))
        nexact_v += nexact
        total_exact += nexact
        total += len(pt)
        per_pass.append("p%d %d/%d%s" % (p, nexact, len(pt), (" " + dict(bad_fields).__repr__()) if bad_fields else ""))
    print("%-18s %s" % (v, " | ".join(per_pass)))
print("TOTAL exact compiles: %d / %d" % (total_exact, total))
