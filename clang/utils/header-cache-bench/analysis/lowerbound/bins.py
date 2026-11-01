"""From analyze2's per-header gains: (1) by first-read own time, the share
of headers that gain and of the best possible gain, per project; (2) the
history rule with a budget of b writes: a header is a unit from its first
read (if p1 + k1 C >= C + L) and is folded for good once its gain so far is
below -b x W. usage: bins.py scale"""
import pickle, sys
scale = sys.argv[1] if len(sys.argv) > 1 else '1.0'
O = pickle.load(open(f'out2_{float(scale)}.pkl', 'rb'))
H = {}
# reads are needed for the history rule: reload them
for pj in O:
    H[pj] = pickle.load(open(f'h_{pj}.pkl', 'rb'))['H']
bins = [0, 50, 200, 500, 2000, 10000, 1e18]
names = ['<50', '50-200', '200-500', '0.5-2k', '2k-10k', '>10k']
print('share of headers that gain as units, by first read own time (us):')
print(f"{'project':9s} " + ' '.join(f'{n:>13s}' for n in names))
for pj, o in O.items():
    cells = []
    for a, b in zip(bins, bins[1:]):
        xs = [x for x in o['hs'] if a <= x['p1'] < b]
        g = sum(1 for x in xs if x['G'] > 0)
        cells.append(f'{g:4d}/{len(xs):<4d}{(100*g/len(xs) if xs else 0):4.0f}%')
    print(f'{pj:9s} ' + ' '.join(f'{c:>13s}' for c in cells))
print('share of the best possible gain, by first read own time:')
print(f"{'project':9s} " + ' '.join(f'{n:>8s}' for n in names))
for pj, o in O.items():
    orc = sum(max(0, x['G']) for x in o['hs'])
    cells = [100 * sum(max(0, x['G']) for x in o['hs'] if a <= x['p1'] < b) / orc for a, b in zip(bins, bins[1:])]
    print(f'{pj:9s} ' + ' '.join(f'{c:7.0f}%' for c in cells))
def gainr(r, C, L, W):
    kind, p, outer, inc, k = r
    if kind == 0: return p + k * C - C - L
    if kind == 1: return -C - W
    return -L
print('history rule, % of the best possible gain, by budget (writes):')
print(f"{'project':9s} " + ' '.join(f'{b:>6}' for b in (1.5, 2, 3, 5, 1e9)))
for pj, o in O.items():
    C, L, W = o['C'], o['L'], o['W']
    orc = sum(max(0, x['G']) for x in o['hs'])
    cells = []
    for budget in (1.5, 2, 3, 5, 1e9):
        tot = 0.0
        for h, reads in H[pj].items():
            if reads[0][1] + reads[0][4] * C < C + L:
                continue
            g = 0.0
            for r in reads:
                g += gainr(r, C, L, W)
                if g < -budget * W:
                    break
            tot += g
        cells.append(100 * tot / orc)
    print(f'{pj:9s} ' + ' '.join(f'{c:5.0f}%' for c in cells))
