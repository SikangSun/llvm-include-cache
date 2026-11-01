"""Codebase header profile: the header dependency graph of one project's
build, and the statistics the header cache's speed may follow, from the
traces ddg_run.py writes (CLANG_HEADER_CACHE_DDG: every compile, every
header read as text).

Trace lines, one file per compile:
  R <header> <includer> <copy hash> <size>   a read of a header; size is
      its own lines' and everything inside it, in the tracker's event clock
  E <header> <kind> <name> <provider> <in|out> <value>   a name the
      header's own lines used: C an #if test, S a macro (or not) where an
      identifier appeared, D a declaration, T a tag; provider is the header
      that defined it ("-": none, or predefined); out: from outside the
      header and the headers it includes.

Writes <out>/<project>.graphml (nodes are headers; include edges, and use
edges from a header to the header that provided a name it used from
outside) and prints one JSON line of statistics.

A header is contingent if any read of it used a macro or a declaration
from outside that its #includes do not explain (contingency.py). Its copies are the distinct copy hashes of its reads; its units are
the distinct outside states its own lines read.

usage: profile_graph.py <trace dir> <project> <out dir> [--sample N]"""
import argparse, collections, hashlib, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import contingency
from xml.sax.saxutils import quoteattr

ap = argparse.ArgumentParser()
ap.add_argument("tdir"); ap.add_argument("project"); ap.add_argument("out")
ap.add_argument("--sample", type=int, default=1, help="every Nth compile")
a = ap.parse_args()

files = sorted((f for f in os.listdir(a.tdir) if f.endswith(".ddg")),
               key=lambda n: int(n.split(".")[0]))[::a.sample]
SYSTEM = ("/usr/", "/lib/clang/", "/include/clang/")

class H:
    __slots__ = ("reads", "direct", "own", "copies", "units", "contingent", "compiles")
    def __init__(self):
        self.reads = 0; self.direct = 0; self.own = 0
        self.copies = set(); self.units = set(); self.contingent = False; self.compiles = 0

hdr = collections.defaultdict(H)
rule = contingency.Rule()
use = collections.Counter()          # (header, provider, kind) -> reads
seen_units = set()
tot_own = rep_own = 0
compiles = 0
for fn in files:
    reads = []   # [path, includer, copy, size, [(kind, name, prov, io, val)]]
    with open(os.path.join(a.tdir, fn), errors="replace") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if p[0] == "R":
                reads.append([p[1], p[2], p[3], int(p[4]), []])
            elif p[0] == "E" and reads and len(p) >= 7:
                reads[-1][4].append((p[2], p[3], p[4], p[5], p[6]))
    if not reads:
        continue
    compiles += 1
    # Own sizes: a read's size less the reads directly inside it (a stack
    # over the reads in entry order, as dup.py does).
    own = [r[3] for r in reads]
    stack = []
    for i, r in enumerate(reads):
        if r[3] >= (1 << 63):          # the main file
            own[i] = 0; stack = [i]; continue
        while stack and reads[stack[-1]][0] != r[1]:
            stack.pop()
        if stack and reads[stack[-1]][3] < (1 << 63):
            own[stack[-1]] -= r[3]
        stack.append(i)
    in_compile = set()
    for i, (path, parent, copy, size, uses) in enumerate(reads):
        if size >= (1 << 63) or path.endswith(".c"):
            continue
        h = hdr[path]
        h.reads += 1
        h.own += max(own[i], 0)
        h.copies.add(copy)
        if path not in in_compile:
            in_compile.add(path); h.compiles += 1
        if parent.endswith(".c"):
            h.direct += 1
        rule.read(path, parent, uses)
        guard = None
        state = []
        for kind, name, prov, io, val in uses:
            if io != "out":
                continue
            # The header's own include guard: its first test of a macro
            # nothing defined, which is no dependency.
            if guard is None and kind == "C" and prov == "-" and val == "0":
                guard = name
                continue
            state.append((kind, name, val))
            if prov not in ("-", path) and not prov.endswith(".c"):
                use[(path, prov, kind)] += 1
        unit = hashlib.blake2b((path + "\0" + "\0".join("\1".join(s) for s in sorted(set(state)))).encode(),
                               digest_size=8).digest()
        h.units.add(unit)
        w = max(own[i], 0)
        tot_own += w
        if unit in seen_units:
            rep_own += w
        seen_units.add(unit)

hidden, _ = rule.finish(set(hdr))
inc = rule.inc
for s, t in hidden:
    hdr[s].contingent = True

# The graph.
os.makedirs(a.out, exist_ok=True)
ids = {p: f"n{k}" for k, p in enumerate(sorted(hdr))}
with open(f"{a.out}/{a.project}.graphml", "w") as g:
    g.write('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<graphml xmlns="http://graphml.graphdrawing.org/xmlns">\n')
    for k, t in (("label", "string"), ("system", "boolean"), ("contingent", "boolean"), ("reads", "int"),
                 ("compiles", "int"), ("direct", "int"), ("own", "long"), ("copies", "int"), ("units", "int")):
        g.write(f'  <key id="{k}" for="node" attr.name="{k}" attr.type="{t}"/>\n')
    for k, t in (("kind", "string"), ("weight", "int")):
        g.write(f'  <key id="e{k}" for="edge" attr.name="{k}" attr.type="{t}"/>\n')
    g.write(f'  <graph id={quoteattr(a.project)} edgedefault="directed">\n')
    for p, h in hdr.items():
        sysh = p.startswith(SYSTEM) or "/lib/clang/" in p
        g.write(f'    <node id="{ids[p]}"><data key="label">{quoteattr(p)[1:-1]}</data>'
                f'<data key="system">{str(sysh).lower()}</data><data key="contingent">{str(h.contingent).lower()}</data>'
                f'<data key="reads">{h.reads}</data><data key="compiles">{h.compiles}</data><data key="direct">{h.direct}</data>'
                f'<data key="own">{h.own}</data><data key="copies">{len(h.copies)}</data><data key="units">{len(h.units)}</data></node>\n')
    for (s, t), w in inc.items():
        if s in ids and t in ids:
            g.write(f'    <edge source="{ids[s]}" target="{ids[t]}"><data key="ekind">include</data><data key="eweight">{w}</data></edge>\n')
    for (s, t, kind), w in use.items():
        if s in ids and t in ids:
            g.write(f'    <edge source="{ids[s]}" target="{ids[t]}"><data key="ekind">use-{kind}</data><data key="eweight">{w}</data></edge>\n')
    g.write("  </graph>\n</graphml>\n")

# Statistics, each weighted by own size where it says so.
W = sum(h.own for h in hdr.values()) or 1
included = {(s, t) for (s, t) in inc}
use_pairs = {(s, t) for (s, t, k) in use}
apps = [h for p, h in hdr.items() if not (p.startswith(SYSTEM) or "/lib/clang/" in p)]
stats = dict(
    project=a.project, compiles=compiles, sample=a.sample, headers=len(hdr),
    application_headers=len(apps),
    header_reads_per_compile=round(sum(h.reads for h in hdr.values()) / max(compiles, 1), 1),
    include_edges=len(included), use_edges=len(use_pairs),
    use_edges_no_include=len({(s, t) for (s, t) in hidden if t != "main file"}),
    contingent_share=round(sum(1 for h in hdr.values() if h.contingent) / max(len(hdr), 1), 3),
    contingent_share_by_size=round(sum(h.own for h in hdr.values() if h.contingent) / W, 3),
    copies_per_header_by_size=round(sum(h.own * len(h.copies) for h in hdr.values()) / W, 2),
    units_per_header_by_size=round(sum(h.own * len(h.units) for h in hdr.values()) / W, 2),
    repeated_work_share=round(rep_own / max(tot_own, 1), 4),
    direct_share_by_size=round(sum(h.own * h.direct / max(h.reads, 1) for h in hdr.values()) / W, 3),
)
print(json.dumps(stats))
