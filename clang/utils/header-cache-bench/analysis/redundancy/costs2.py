"""Cost constants from one COUNTER=1 run (RUN dir), all four projects pooled:
hit fixed cost = mean cost of hits on copies under 16 KB; hit slope = fit of
hit cost against the header's median read time as text; miss ask = mean
ask+check of missed lookups; write fixed = mean of single small writes
(parse < 1 ms); write slope = fit over single writes."""
import json, re, os, sys, statistics as st, collections
import numpy as np
RUN = sys.argv[1]
hits = []; miss = []; W = []; HT = []
for p in ['redis', 'cpython', 'openssl', 'ffmpeg']:
    J = json.load(open(f'{RUN}/{p}.json'))
    tt = collections.defaultdict(list)
    for s in J['S']['counter']:
        for m in re.finditer(r'\n  text (\S+) (\d+) depth 1 own \d+', s): tt[m[1]].append(int(m[2]))
    med = {k: st.median(v) for k, v in tt.items()}
    for s in J['S']['counter']:
        for m in re.finditer(r'\n  query (\S+) (hit|miss) total=\d+ ask=(\d+) check=(\d+) checked=\d+ load=(\d+) replay=(\d+) bytes=(\d+)', s):
            v = list(map(int, m.groups()[2:]))
            if m[2] == 'hit':
                hits.append(v)
                if m[1] in med: HT.append((med[m[1]], sum(v[:4])))
            else: miss.append(v)
        m = re.search(r'exported (\d+) parts.*?([\d.]+) ms writing and pushing', s)
        mt = [int(x[1]) for x in re.finditer(r'\n  text \S+ (\d+) depth 1 own \d+ why (?:rejected|no-saved)', s)]
        if int(m[1]) == 1 and len(mt) == 1: W.append((mt[0], float(m[2]) * 1e3))
H = np.array(hits, float); M = np.array(miss, float); W = np.array(W, float); HT = np.array(HT, float)
small = H[:, 4] < 16e3; tot = H[:, :4].sum(1)
print('small hits n=%d: mean %.0f us (ask %.0f check %.0f load %.0f replay %.0f), median %.0f, p90 ask %.0f' % (
    small.sum(), tot[small].mean(), *H[small, :4].mean(0), np.median(tot[small]), np.percentile(H[:, 0], 90)))
b = np.linalg.lstsq(np.c_[np.ones(len(HT)), HT[:, 0]], HT[:, 1], rcond=None)[0]
print('hit fit: %.0f us + %.3f x parse' % tuple(b))
print('miss lookup mean ask+check %.0f us (median %.0f)' % ((M[:, 0] + M[:, 1]).mean(), np.median(M[:, 0] + M[:, 1])))
sw = W[:, 0] < 1000
wb = np.linalg.lstsq(np.c_[np.ones(len(W)), W[:, 0]], W[:, 1], rcond=None)[0]
print('small single writes n=%d mean %.0f us median %.0f; write fit %.0f us + %.3f x parse' % (sw.sum(), W[sw, 1].mean(), np.median(W[sw, 1]), *wb))
