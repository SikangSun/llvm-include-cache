"""Bothner's compile server replayed over a recorded build
(COMPILE_SERVER_PLAN.md 3.3, step D5).

The build's compiles run in log order twice: a first build fills the server,
then the recompilation is scored. Inside a compile, fragment instances run in
the order the compile processed them, with its #define and #undef between
them. The server holds, per fragment location (configuration, file as opened,
offset), one stored version: the latest parse (p.26). A version keeps its
macro list, its uses (each used location with the version this compile had
there), and, for the scoring only, its oracle (see oracles()).

A fragment instance is reused when the variant's rule allows it. In order of
the reasons given when it does not:
  nested   the parser was inside a declaration where it starts or ends (p.33);
  param    it declares a tag in a parameter list (p.28-29);
  main     it uses a main-file declaration, which is parsed in every compile
           (ours, from p.27's precondition);
  repeat   this compile processed the location already (p.28);
  new      nothing is stored there;
  macro    the macro check fails;
  uses     a location it used was not processed in this compile at the
           version it saw: p.28's timestamp rule, read through version
           identity, which one version per location makes equivalent;
  unsafe   the rule allows it, but the stored version's oracle differs: its
           parse would mean something else. Counted, and charged as a parse,
           as a correct server must.
Whatever is not reused is parsed and becomes the stored version.

Rules for a parse and for repeats:
  R0  the paper's: every parse is a new version, and a repeat is parsed;
  R1  ours: a parse identical to the stored version (same oracle) keeps it,
      so its users stay valid, and a version's own location is left out of
      its uses (otherwise a header read twice, as Clang's __stddef_size_t.h
      is, depends on itself and is never valid again);
  R2  as R1, but locations are numbered by occurrence in the compile, so the
      second reading of a header is a location of its own: the variant most
      favourable to Bothner.
Macro checks:
  V0  none, as the prototype ("does not yet check", p.29);
  V1  the macro list: every identifier the fragment sent to the
      macro-expansion decision must be in the same state (no macro, or the
      same definition): p.30's "obvious solution", with expands-to (ours);
  V2  p.30's two bits per identifier, kept per configuration: used as a
      non-macro, then also defined as a macro, which invalidates every stored
      version; afterwards a fragment checks the state of such identifiers.
Bounds, beside the variants:
  tokens  D2's v0: R1, the tokens must match, no uses;
  ideal   any earlier parse at the location (numbered as R2) with the same
          oracle: the best a cache of fragments could do with free restore.

Time: plain clang's front end for each compile, less the measured time of
its reused fragments, at no cost for restoring them, less CREDIT_MS of
compiler start-up a persistent server skips (p.24; its driver still runs,
p.22-23; measured 1.57-1.68 ms with redis's flags).

usage: simulate.py <record dir> <timing dir> [recompile json]
                   [--trace INDEX] [--variant NAME] [--out per-compile.json]
                   [--reused reused.json]
The recompile JSON (recompile.py) gives plain clang's and the cache's
recompilation times; without it, the timing run's plain clang with modules.
env: CREDIT_MS (default 1.65); SAVED_SCALE (default 1): multiplies the time
every reused fragment saves, to widen the bound by the time model's error
(blank.py measured it).
"""
import argparse, collections, json, os, statistics, sys

ap = argparse.ArgumentParser()
ap.add_argument("records")
ap.add_argument("timing")
ap.add_argument("recompile", nargs="?")
ap.add_argument("--trace", type=int, help="print every decision of this compile (log index)")
ap.add_argument("--variant", default="R1 V1", help="the variant --trace follows")
ap.add_argument("--out", help="write per-compile results here")
ap.add_argument("--reused", help="write, per compile, the instances --variant reuses in the recompilation "
                "(file as opened, offset, occurrence): blank.py's input")
args = ap.parse_args()
recomp = json.load(open(args.recompile)) if args.recompile else None
CREDIT = float(os.environ.get("CREDIT_MS", "1.65")) / 1e3
SCALE = float(os.environ.get("SAVED_SCALE", "1"))

VARIANTS = {  # name: (rule, macro check)
    "R1 V1": ("R1", "V1"), "R0 V1": ("R0", "V1"), "R2 V1": ("R2", "V1"),
    "R1 V0": ("R1", "V0"), "R1 V2": ("R1", "V2"),
    "tokens": ("R1", "tokens"), "ideal": ("R2", "ideal"),
}

# --- The recorded build ---------------------------------------------------
intern = {}
def name_of(s):
    return intern.setdefault(s, s)

builds = []
for f in sorted(os.listdir(args.records)):
    if not f.endswith(".json"):
        continue
    r = json.load(open(os.path.join(args.records, f)))
    t = json.load(open(os.path.join(args.timing, f)))
    if not r["data"] or not t["data"]:
        continue
    rd, td = r["data"], t["data"]
    assert len(rd["frags"]) == len(td["frags"]), f
    names = [name_of(n) for n in rd["names"]]
    xs = []
    for a, b in zip(rd["frags"], td["frags"]):
        name = name_of(rd["entries"][a[0]][0])
        assert (name, a[1]) == (td["entries"][b[0]][0], b[1]), f
        # By name: the record lists them by a name id each compile assigns.
        ids = tuple(sorted((names[n], s) for n, s in a[15]))
        xs.append({"loc": (rd["config"], name, a[1]), "tok": a[7], "pragma": a[11],
                   "nested": not (a[8] == 1 and a[9] == 1), "order": a[10],
                   "flags": a[13], "uses": a[14], "deps": a[16], "ids": ids, "mlist": hash(ids), "ns": b[5]})
    order = sorted(range(len(xs)), key=lambda k: xs[k]["order"])
    pos = {k: p for p, k in enumerate(order)}
    xs = [xs[k] for k in order]
    seen = collections.Counter()
    for x in xs:
        x["uses"] = [pos[u] for u in x["uses"]]
        x["deps"] = [pos[u] for u in x["deps"]]
        seen[x["loc"]] += 1
        x["occ"] = seen[x["loc"]]
    # Fragments and #define/#undef in processing order.
    seq = [(x["order"], 1, x) for x in xs] + [(e, 0, (names[n], d)) for e, n, d in rd["defs"]]
    seq.sort(key=lambda s: (s[0], s[1]))
    builds.append({"i": r["meta"]["i"], "src": r["meta"]["src"], "cwd": r["meta"]["cwd"],
                   "config": rd["config"], "xs": xs, "seq": seq, "span": td["span_ns"],
                   "plainM": t["meta"].get("plain_wall", 0)})


def oracles(b):
    """Each instance's oracle: its tokens, its pragma state, the distinct
    oracles of the fragments holding earlier declarations of what it used, and
    the main file where it depends on that. Earlier readings of its own
    location are left out, whatever numbering a rule uses: a header read twice
    redeclares its own first reading. Each oracle counts once: Clang's
    __stddef_size_t.h, read a varying number of times, gives a varying number
    of identical earlier declarations."""
    out = []
    for x in b["xs"]:
        deps = sorted({out[d] for d in x["deps"] if b["xs"][d]["loc"] != x["loc"]})
        out.append(hash((x["tok"], x["pragma"], tuple(deps), (b["cwd"], b["src"]) if x["flags"] & 2 else None)))
    return out


# --- The server ------------------------------------------------------------
class Version:
    __slots__ = ("id", "oracle", "mlist", "tok", "pragma", "uses", "ts", "check")


def simulate(rule, macro, trace=None):
    store = {}                       # key -> Version, or for "ideal" key -> set of oracles
    clock, ids = 0, 0
    nonmacro = collections.defaultdict(set)    # V2, per configuration
    alsomacro = collections.defaultdict(set)
    first_valid = collections.defaultdict(int)
    out = []
    for phase in (0, 1):
        for b in builds:
            clock += 1
            cfg = b["config"]
            processed = {}           # key -> version id this compile has there
            ver = [None] * len(b["xs"])
            key_of = (lambda x: (x["loc"], x["occ"])) if rule == "R2" else (lambda x: x["loc"])
            orc = oracles(b)
            why, saved, unsafe, reused = collections.Counter(), 0, collections.Counter(), []
            p = 0
            for _, kind, item in b["seq"]:
                if kind == 0:        # #define or #undef
                    name, defined = item
                    if macro == "V2" and defined and name in nonmacro[cfg] and name not in alsomacro[cfg]:
                        alsomacro[cfg].add(name)
                        clock += 1
                        first_valid[cfg] = clock
                    continue
                x, k = item, p
                p += 1
                key = key_of(x)
                v = store.get(key)
                if x["nested"]:
                    reason = "nested"
                elif x["flags"] & 4:
                    reason = "param"
                elif x["flags"] & 1:
                    reason = "main"
                elif key in processed:
                    reason = "repeat"
                elif v is None:
                    reason = "new"
                elif macro == "ideal":
                    reason = None if orc[k] in v else "changed"
                elif macro == "tokens":
                    reason = None if v.tok == x["tok"] else "macro"
                else:
                    reason = None
                    if macro == "V1" and v.mlist != x["mlist"]:
                        reason = "macro"
                    elif macro == "V2":
                        now = dict(x["ids"])
                        if v.ts <= first_valid[cfg] or any(now.get(n, -1) != s for n, s in v.check):
                            reason = "macro"
                    if reason is None and any(processed.get(lu) != vu for lu, vu in v.uses):
                        reason = "uses"
                    if reason is None and v.oracle != orc[k]:
                        reason = "unsafe"
                        # Why the parse would differ, and where.
                        cause = ("tokens" if v.tok != x["tok"] else "pragma" if v.pragma != x["pragma"]
                                 else "earlier declarations")
                        unsafe[(cause, x["loc"][1])] += 1
                if macro == "V2":
                    nonmacro[cfg].update(n for n, s in x["ids"] if s == 0)
                if reason is None:   # reused
                    if macro == "ideal":
                        processed[key] = orc[k]
                    else:
                        processed[key] = v.id
                        ver[k] = v.id
                    saved += x["ns"]
                    why["reused"] += 1
                    reused.append(k)
                else:                # parsed: the stored version from now on
                    why[reason] += 1
                    clock += 1
                    if macro == "ideal":
                        store.setdefault(key, set()).add(orc[k])
                        processed[key] = orc[k]
                    else:
                        uses = []
                        for u in x["uses"]:
                            lu = key_of(b["xs"][u])
                            if rule != "R0" and lu == key:
                                continue
                            uses.append((lu, ver[u]))
                        if rule != "R0" and v is not None and v.oracle == orc[k]:
                            nv = v   # R1: an identical parse keeps the version
                        else:
                            nv = Version()
                            ids += 1
                            nv.id = ids
                            store[key] = nv
                        nv.oracle, nv.mlist, nv.tok, nv.uses, nv.ts = orc[k], x["mlist"], x["tok"], uses, clock
                        nv.pragma = x["pragma"]
                        nv.check = [(n, s) for n, s in x["ids"] if n in alsomacro[cfg]] if macro == "V2" else ()
                        processed[key] = nv.id
                        ver[k] = nv.id
                if trace is not None and phase == 1 and b["i"] == trace:
                    print(f"  {x['loc'][1]}:{x['loc'][2]}#{x['occ']}  {reason or 'reused':8} {x['ns'] / 1e3:8.1f} us"
                          f"  uses {[(b['xs'][u]['loc'][1], b['xs'][u]['loc'][2]) for u in x['uses']]}")
            if phase == 1:
                out.append({"i": b["i"], "why": why, "saved": saved, "unsafe": unsafe, "reused": reused})
    return out


# --- Scoring ---------------------------------------------------------------
def plain_of(b):
    if recomp and str(b["i"]) in recomp["compiles"]:
        return recomp["compiles"][str(b["i"])]["plain"], recomp["compiles"][str(b["i"])]["cache"]
    return b["plainM"], None


if args.trace is not None:
    rule, macro = VARIANTS[args.variant]
    print(f"compile {args.trace}, {args.variant}, recompilation:")
    simulate(rule, macro, trace=args.trace)
    sys.exit()

results = {name: simulate(*v) for name, v in VARIANTS.items()}
by_build = {b["i"]: b for b in builds}
total_ns = sum(x["ns"] for b in builds for x in b["xs"])
n_inst = sum(len(b["xs"]) for b in builds)
print(f"{len(builds)} compiles; recompilation: {n_inst} fragment instances, {total_ns / 1e9:.2f} s in fragments")
REASONS = ["reused", "nested", "param", "main", "repeat", "new", "changed", "macro", "uses", "unsafe"]
print(f"\n{'variant':8} " + " ".join(f"{r:>8}" for r in REASONS) + "   reused time")
for name, res in results.items():
    why = collections.Counter()
    for r in res:
        why.update(r["why"])
    saved = sum(r["saved"] for r in res)
    print(f"{name:8} " + " ".join(f"{why[r] / n_inst * 100:7.1f}%" if why[r] else f"{'-':>8}" for r in REASONS)
          + f"   {saved / total_ns * 100:5.1f}%")
for name in ("R1 V1", "R1 V0", "R2 V1"):
    u = collections.Counter()
    for r in results[name]:
        u.update(r["unsafe"])
    causes = collections.Counter()
    for (c, _), n in u.items():
        causes[c] += n
    print(f"\n{name}: unsafe decisions by cause: {dict(causes)}; most frequent places:")
    for (c, where), n in u.most_common(8):
        print(f"  {n:6}  {c:22} {where}")


def table(label, sel):
    bs = [b for b in builds if sel(b)]
    if not bs:
        return
    P = sum(plain_of(b)[0] for b in bs)
    print(f"\n{label}: {len(bs)} compiles; plain clang {P:.2f} s; compiler start-up credited "
          f"{CREDIT * 1e3:.2f} ms per compile in every Bothner row"
          + (f"; time saved by reuse scaled by {SCALE}" if SCALE != 1 else ""))
    rows = []
    if all(plain_of(b)[1] is not None for b in bs):
        rows.append(("header cache, recompilation", {b["i"]: plain_of(b)[1] for b in bs}))
    for name, res in results.items():
        saved = {r["i"]: SCALE * r["saved"] / 1e9 for r in res}
        rows.append((f"Bothner {name}", {b["i"]: plain_of(b)[0] - saved[b["i"]] - CREDIT for b in bs}))
    rows.append(("every fragment reused", {b["i"]: plain_of(b)[0] - SCALE * sum(x["ns"] for x in b["xs"]) / 1e9 - CREDIT
                                           for b in bs}))
    for name, times in rows:
        T = sum(times.values())
        g = statistics.geometric_mean([plain_of(b)[0] / times[b["i"]] for b in bs])
        print(f"  {name:30} {T:7.2f} s  {P / T:5.2f}x in total, {g:5.2f}x geometric mean per compile")


table("all", lambda b: True)
table("redis-server (src/)", lambda b: "/redis/src" in b["cwd"])

if args.reused:
    json.dump({str(r["i"]): [[by_build[r["i"]]["xs"][k]["loc"][1], by_build[r["i"]]["xs"][k]["loc"][2],
                              by_build[r["i"]]["xs"][k]["occ"]] for k in r["reused"]]
               for r in results[args.variant]}, open(args.reused, "w"))

if args.out:
    json.dump({name: [{"i": r["i"], "src": by_build[r["i"]]["src"], "why": dict(r["why"]), "saved_ns": r["saved"]}
                      for r in res] for name, res in results.items()}, open(args.out, "w"))
