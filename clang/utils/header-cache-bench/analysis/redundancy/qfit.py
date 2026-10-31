"""Per-hit cost by copy size, from the query lines of the COUNTER=1 runs
(results-2026-10-30/lay): median ask/check/load/replay per size bin, and a
least-squares line cost = a + b*KB over hits (ask capped at its 90th pct)."""
import json, re, sys, statistics as st
import numpy as np
rows = []
for proj in sys.argv[1:]:
    J = json.load(open(f'/home/sikang/c/code/scratch/e2e/multi/results-2026-10-30/lay/{proj}.json'))
    for s in J['S']['counter']:
        for m in re.finditer(r'\n  query \S+ (hit|miss) total=(\d+) ask=(\d+) check=(\d+) checked=(\d+) load=(\d+) replay=(\d+) bytes=(\d+)', s):
            rows.append((m[1], *map(int, m.groups()[1:])))
hits = [r for r in rows if r[0] == 'hit']; miss = [r for r in rows if r[0] == 'miss']
print('queries', len(rows), 'hits', len(hits))
print('median us: ask hit %d miss %d; check hit %d miss %d' % (st.median(r[2] for r in hits), st.median(r[2] for r in miss), st.median(r[3] for r in hits), st.median(r[3] for r in miss)))
bins = [0, 16e3, 64e3, 256e3, 1e6, 4e6, 1e9]
for lo, hi in zip(bins, bins[1:]):
    b = [r for r in hits if lo <= r[7] < hi]
    if not b: continue
    print('%7dK-%7dK n=%5d  ask %5d check %5d load %5d replay %5d' % (lo/1e3, hi/1e3, len(b), st.median(r[2] for r in b), st.median(r[3] for r in b), st.median(r[5] for r in b), st.median(r[6] for r in b)))
X = np.array([[1, r[7]/1e3] for r in hits])
for name, col in (('check', 3), ('load', 5), ('replay', 6)):
    y = np.array([r[col] for r in hits], float); cap = np.percentile(y, 95); m = y <= cap
    a, b = np.linalg.lstsq(X[m], y[m], rcond=None)[0]
    print(f'{name}: {a:.0f} us + {b*100:.1f} us per 100 KB')
