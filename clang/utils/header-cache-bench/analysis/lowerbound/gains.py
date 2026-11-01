"""Per-header gain of being a linked unit, against folding it into the copy
around it (outermost: reading it as text), all other headers units. Per
read, own parse time p, k headers it includes directly that are read:
  hit  : + p + k x C - C - L   (its lines and k asks saved; one ask, one load)
  miss : - C - W               (one ask, one write)
  link : - L                   (a copy of its own, loaded with the hit around it)
Costs per project: the profile's instructions per ask C, per copy loaded
L, per copy written W (u38prof; the median of the five for the others),
over the project's instructions per us of the trace's parse times (0.9 of
plain clang's instructions per compile, u38all, over the trace's header
time per compile). Rules: today's (first read >= 226 us with the headers
inside it), the floor (own time + k C >= C + L), the floor with the
header's own counts (folded once its gain so far is below -5 W).
usage: gains.py costscale u38all-dir (h_<proj>.pkl and icosts.json in the
current directory)"""
import json, pickle, sys, math, statistics, collections
scale = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
P = ['git', 'redis', 'cpython', 'ffmpeg', 'openssl', 'postgres', 'curl', 'nginx', 'php', 'sqlite', 'linux']
IC = json.load(open('icosts.json'))
med = {k: statistics.median(IC[p][k] for p in IC) for k in ('C', 'L', 'W')}
RES = sys.argv[2]
def gainr(r, C, L, W):
    kind, p, outer, inc, k = r
    if kind == 0: return p + k * C - C - L
    if kind == 1: return -C - W
    return -L
out = {}
print(f"{'project':9s} {'instr/us':>8s} {'C us':>5s} {'L us':>5s} {'W us':>6s} {'headers':>7s} {'gain':>5s} {'oracle %':>8s} {'all':>6s} {'226 inc':>7s} {'floor':>6s} {'hist':>6s} {'best own':>8s}")
for pj in P:
    D = pickle.load(open(f'h_{pj}.pkl', 'rb'))
    H, tot, ncomp = D['H'], D['total_us'], D['compiles']
    J = json.load(open(f'{RES}/{pj}.json'))
    plain = [x for x in J['I']['plain'] if x]
    f = 0.9 * (sum(plain) / len(plain)) / (tot / ncomp)
    c = IC.get(pj, med)
    C, L, W = (scale * c[k] / f for k in ('C', 'L', 'W'))
    hs = []
    for h, reads in H.items():
        G = sum(gainr(r, C, L, W) for r in reads)
        hs.append(dict(h=h, G=G, p1=reads[0][1], inc1=reads[0][3], n=len(reads),
                       hits=sum(r[0] == 0 for r in reads), misses=sum(r[0] == 1 for r in reads),
                       links=sum(r[0] == 2 for r in reads), pm=sum(r[1] for r in reads) / len(reads),
                       k1=reads[0][4], reads=reads))
    oracle = sum(max(0, x['G']) for x in hs)
    pc = lambda v: 100 * v / oracle if oracle else 0
    floor = C + L
    rule = lambda x: x['p1'] + x['k1'] * C >= floor
    hist = 0.0
    for x in hs:
        if not rule(x): continue
        g = 0.0
        for r in x['reads']:
            g += gainr(r, C, L, W)
            if g < -5 * W: break
        hist += g
    grid = [0, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
    best = max(grid, key=lambda T: sum(x['G'] for x in hs if x['p1'] >= T))
    out[pj] = dict(f=f, C=C, L=L, W=W, hs=[{k: v for k, v in x.items() if k != 'reads'} for x in hs], tot=tot,
                   grid={T: sum(x['G'] for x in hs if x['p1'] >= T) / oracle for T in grid})
    print(f"{pj:9s} {f:8.0f} {C:5.0f} {L:5.0f} {W:6.0f} {len(hs):7d} {sum(x['G']>0 for x in hs):5d} {100*oracle/tot:8.1f}"
          f" {pc(sum(x['G'] for x in hs)):6.0f} {pc(sum(x['G'] for x in hs if x['inc1']>=226)):7.0f}"
          f" {pc(sum(x['G'] for x in hs if rule(x))):6.0f} {pc(hist):6.0f} {best:8d}")
pickle.dump(out, open(f'out2_{scale}.pkl', 'wb'))
print('columns all/226 inc/floor/hist: % of the oracle gain; oracle %: of header text time')
