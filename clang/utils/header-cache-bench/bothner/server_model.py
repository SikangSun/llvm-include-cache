"""Step S4 of COMPILE_SERVER_DESIGN.md, the model's side: simulate.py's
server rule (simulate_server) for every compile and pass, saved as JSON,
without the rest of simulate.py. server_compare.py compares it with the
server's results.

usage: server_model.py <records dir> <out.json>
env: PHASES (default 3, the server run's passes)

simulate.py runs everything at import. This runs its text up to "# ---
Scoring" (the records, the units, the server rule) and then only the runs
named below. The records serve as the timing input too: decisions do not
read times.
"""
import json, os, sys

BOTH = os.path.dirname(os.path.abspath(__file__))
rec, out = sys.argv[1], sys.argv[2]
os.environ.setdefault("PHASES", "3")
src = open(os.path.join(BOTH, "simulate.py")).read()
cut = src.index("# --- Scoring")
sys.argv = [os.path.join(BOTH, "simulate.py"), rec, rec]
sys.path.insert(0, BOTH)
ns = {"__name__": "simulate_head", "__file__": os.path.join(BOTH, "simulate.py")}
exec(compile(src[:cut], "simulate.py", "exec"), ns)
RUNS = {
    "S3G* tok": ("S3G*", dict(tokrefs=True)),
    "S3G*": ("S3G*", dict()),
    "S2 tok": ("S2", dict(tokrefs=True)),
    "S2": ("S2", dict()),
}
res = {"compiles": [{"i": b["i"], "src": b["src"]} for b in ns["builds"]]}
for label, (units, kw) in RUNS.items():
    r = ns["simulate_server"](ns["SERVER"][units], **kw)
    res[label] = [[{"i": x["i"], "why": dict(x["why"]), "restored": x["restored"]} for x in p] for p in r]
    print(label, "done", flush=True)
json.dump(res, open(out, "w"))
