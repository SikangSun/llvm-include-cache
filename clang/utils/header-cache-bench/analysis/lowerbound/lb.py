"""Per-header gain of being its own linked unit, from the saved header-graph
traces (results-2026-10-22/ddg3: every header read as text), first build in
benchmark order, every listed header asked (model B of reuse_proj.py), the
-W list part of the key. Each read of header h, own parse time p (us):
  hit   (asked, state seen before):  unit saves p, plus h's bytes in the
        copy around it (w1 p, none if outermost); pays an ask C and a load L
  miss  (asked, new state): pays C and a write W0 (+ w1 p if outermost,
        where the folded header is in no copy)
  link  (a copy around it hit): pays L for its own copy
Gain G(h) = sum over its reads. Folded = h's lines in its parent's copy (or
plain text if outermost). All other headers stay units (marginal gain).
usage: lb.py proj units-dir weights.pkl step out.pkl logs-dir [src-root]
(units-dir: per-compile pickles of a header-graph trace, as reuse/parse.py
writes them; weights.pkl: median own us per header; logs-dir: the
project's recorded compile commands, log-<proj>.)"""
import os, pickle, statistics, sys, collections
PROJ, U, WP, STEP, OUT = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
SRC = sys.argv[7] if len(sys.argv) > 7 else '/dev/shm/bench-src/'
W = pickle.load(open(WP, 'rb'))
LOGS = sys.argv[6]
cmds = []
for name in sorted(os.listdir(LOGS), key=lambda n: int(n.split('.')[0])):
    parts = open(f'{LOGS}/{name}', 'rb').read().decode(errors='replace').split('\0')[:-1]
    cwd, args = parts[0], parts[1:]
    if '-c' not in args or '-E' in args:
        continue
    srcs = [a for i, a in enumerate(args) if a.endswith('.c') and args[i - 1] not in ('-o', '-MF', '-MT', '-MQ')]
    if len(srcs) != 1 or 'conftest' in srcs[0]:
        continue
    wl = tuple(a for a in args if (a.startswith('-W') and not a.startswith(('-Wl,', '-Wp,', '-Wa,')))
               or a in ('-w', '-pedantic', '-pedantic-errors'))
    cmds.append((srcs[0], wl))
bysrc = collections.defaultdict(list)
for f in sorted(os.listdir(U)):
    src, res = pickle.load(open(os.path.join(U, f), 'rb'))
    bysrc[src].append(f)
order = []
used = collections.Counter()
for src, wl in cmds:
    lst = bysrc.get(src) or bysrc.get(src.lstrip('./'))
    if not lst or used[src] >= len(lst):
        continue
    order.append((lst[used[src]], wl)); used[src] += 1
order = order[::STEP]
sizes = {}
def nbytes(p):
    if p not in sizes:
        q = p.replace('/home/sikang/c/benchmarks/', SRC)
        try: sizes[p] = os.path.getsize(q)
        except OSError: sizes[p] = 0
    return sizes[p]
_r = [W[p][0] / nbytes(p) for p in W if nbytes(p) > 0 and W[p][0] > 0]
upb = statistics.median(_r) if _r else 0.159
def own_us(p):
    return W[p][0] if p in W else nbytes(p) * upb
data = []
spans = collections.defaultdict(list)
for f, wl in order:
    src, res = pickle.load(open(os.path.join(U, f), 'rb'))
    data.append((res, wl))
    for r in res:
        if r[5] > 2:
            spans[r[0]].append(r[5])
medspan = {p: statistics.median(v) for p, v in spans.items()}
def read_us(r):
    if r[5] <= 2:
        return 0.0
    return own_us(r[0]) * min(3.0, r[5] / medspan.get(r[0], max(1, r[5])))
# per header: list of reads (kind, p, outer, inclusive us, headers it
# includes directly that are read), in build order
H = collections.defaultdict(list)
seen = set()
total_us = 0.0
for res, wl in data:
    wk = hash(wl)
    n = len(res)
    wt = [read_us(r) for r in res]
    inc = wt[:]
    for i in range(n - 1, -1, -1):
        if res[i][2] >= 0:
            inc[res[i][2]] += inc[i]
    free = [False] * n
    kids = [0] * n
    for i in range(n):
        if res[i][2] >= 0 and res[i][5] > 2:
            kids[res[i][2]] += 1
    for i, (p, dep, par, own, copy, ospan) in enumerate(res):
        total_us += wt[i]
        if par >= 0 and free[par]:
            free[i] = True
            if ospan > 2:
                H[p].append((2, wt[i], dep == 1, inc[i], kids[i]))
            continue
        if ospan <= 2:
            continue
        k = (copy, wk)
        if k in seen:
            free[i] = True
            H[p].append((0, wt[i], dep == 1, inc[i], kids[i]))
        else:
            seen.add(k)
            H[p].append((1, wt[i], dep == 1, inc[i], kids[i]))
pickle.dump({'proj': PROJ, 'compiles': len(data), 'total_us': total_us, 'H': dict(H),
             'weighted': sum(1 for p in H if p in W), 'headers': len(H)}, open(OUT, 'wb'))
print(PROJ, len(data), 'compiles', len(H), 'headers', f'{total_us/1e6:.1f} s of header text')
