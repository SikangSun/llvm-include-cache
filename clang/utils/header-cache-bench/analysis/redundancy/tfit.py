"""Cost model in parse-time units, COUNTER=1 runs of results-2026-10-30/lay.
Hit: cost (ask+check+load+replay) vs the header's median read time as text
(whole subtree, depth 1, other compiles). Write: per-compile write ms vs
exported parts and the text time of the depth-1 misses."""
import json, re, sys, statistics as st, collections
import numpy as np
H = []; W = []
for proj in sys.argv[1:]:
    J = json.load(open(f'/home/sikang/c/code/scratch/e2e/multi/results-2026-10-30/lay/{proj}.json'))
    tt = collections.defaultdict(list)
    for s in J['S']['counter']:
        for m in re.finditer(r'\n  text (\S+) (\d+) depth 1 own \d+(?: why (\S+))?', s):
            tt[m[1]].append(int(m[2]))
    med = {k: st.median(v) for k, v in tt.items()}
    for s in J['S']['counter']:
        for m in re.finditer(r'\n  query (\S+) hit total=\d+ ask=(\d+) check=(\d+) checked=\d+ load=(\d+) replay=(\d+) bytes=(\d+)', s):
            if m[1] in med: H.append((med[m[1]], min(int(m[2]), 400) + int(m[3]) + int(m[4]) + int(m[5]), int(m[6])))
        m = re.search(r'exported (\d+) parts.*?([\d.]+) ms writing and pushing', s)
        parts, wms = int(m[1]), float(m[2])
        mt = sum(int(x[2]) for x in re.finditer(r'\n  text (\S+) (\d+) depth 1 own \d+ why (rejected|no-saved)', s))
        if parts: W.append((parts, mt, wms))
H = np.array(H, float); W = np.array(W, float)
print('hits joined', len(H))
for lo, hi in ((0, 500), (500, 1000), (1000, 3000), (3000, 10000), (10000, 1e9)):
    m = (H[:, 0] >= lo) & (H[:, 0] < hi)
    if m.any(): print('parse %6d-%6d us n=%5d  median hit cost %5d us  ratio %.2f  median KB %d' % (lo, hi, m.sum(), np.median(H[m, 1]), np.median(H[m, 1] / H[m, 0]), np.median(H[m, 2]) / 1e3))
a, b = np.linalg.lstsq(np.c_[np.ones(len(H)), H[:, 0]], H[:, 1], rcond=None)[0]
print(f'hit cost = {a:.0f} us + {b:.3f} x parse')
c, d = np.linalg.lstsq(W[:, :2], W[:, 2] * 1e3, rcond=None)[0]
print(f'write cost = {c/1e3:.2f} ms per copy + {d:.3f} x parse time written  (n={len(W)} compiles)')
d0 = W[:, 2].sum() * 1e3 / W[:, 1].sum(); print(f'write/parse overall {d0:.3f}')
